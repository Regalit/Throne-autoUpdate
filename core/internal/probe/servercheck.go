package probe

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"net/http"
	"sync"
	"time"
)

// Shadowlos: "green means it opens". A latency answer alone (a 204 of a few hundred bytes) passes
// through filters that let the first 15-20 KB of a connection go and then freeze it, and through
// exits that reach Cloudflare but not YouTube or Instagram. After the latency step a full check
// also moves real volume and asks the services people open; the rule is shared with the Android
// client (server-check spec, 2026-11-10).
const (
	CheckOK      = "ok"      // volume and every service passed
	CheckBlocked = "blocked" // latency answered, the volume did not get through
	CheckService = "service" // volume passed, at least one service did not answer

	checkBulkBytes      = 128 * 1024
	checkBulkTimeout    = 8 * time.Second
	checkServiceTimeout = 5 * time.Second
	// Volume and services together; with the 3 s latency step one server stays within 12 s.
	checkBudget = 9 * time.Second
	// Full checks at once: each one moves ~150 KB and marks the user online on that server.
	checkMaxConcurrency = 4
)

type checkService struct {
	Name string
	URL  string
	// Refused: text of the page a service shows when it turns this exit's country away. Such a
	// page is a normal HTTP answer, so only its text tells it from the service itself.
	Refused []string
}

const (
	checkServiceBodyLimit = 256 * 1024
	// Sites answer a bare client differently from a browser; ask the way a browser does.
	checkUserAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36"
)

// Tried in order; one full download is enough.
var checkBulkURLs = []string{
	"https://speed.cloudflare.com/__down?bytes=131072",
}

// The list and its order are what the server list shows, one column per service
// (Configs::kCheckServices in the GUI).
var checkServices = []checkService{
	{Name: "YouTube", URL: "https://www.youtube.com/generate_204"},
	{Name: "Instagram", URL: "https://www.instagram.com/"},
	{Name: "Facebook", URL: "https://www.facebook.com/"},
	{Name: "WhatsApp", URL: "https://web.whatsapp.com/"},
	{Name: "X", URL: "https://x.com/"},
	{Name: "Gemini", URL: "https://gemini.google.com/app", Refused: []string{"supported in your country"}},
	{Name: "ChatGPT", URL: "https://api.openai.com/compliance/cookie_requirements", Refused: []string{"unsupported_country"}},
}

// ServiceResult is one service asked through one server.
type ServiceResult struct {
	Name string
	OK   bool
	Ms   int64 // time to the answer; meaningful only when OK
	// Note says what came back from a service that has a refusal page (status, final address,
	// page title, the words around "country" or "region"), for the journal: the refusal text
	// differs by exit country and can only be learnt from real answers.
	Note string
}

// ServerCheck is the outcome of the volume and service steps for one server.
type ServerCheck struct {
	State     string
	Failed    []string        // service names that did not answer, in checkServices order
	Services  []ServiceResult // every service asked, in checkServices order; empty when blocked
	BulkBytes int64
	BulkMs    int64
}

func serverCheck(ctx context.Context, client *http.Client) *ServerCheck {
	return runServerCheck(ctx, client, checkBulkURLs, checkServices)
}

// runServerCheck runs the volume step and the service step side by side, so a blocked server
// costs no more time than a good one.
func runServerCheck(ctx context.Context, client *http.Client, bulkURLs []string, services []checkService) *ServerCheck {
	ctx, cancel := context.WithTimeout(ctx, checkBudget)
	defer cancel()

	check := &ServerCheck{}
	results := make([]ServiceResult, len(services))
	var wg sync.WaitGroup
	for idx, svc := range services {
		wg.Add(1)
		go func() {
			defer wg.Done()
			started := time.Now()
			ok, note := serviceAnswers(ctx, client, svc)
			results[idx] = ServiceResult{Name: svc.Name, OK: ok, Ms: time.Since(started).Milliseconds(), Note: note}
		}()
	}
	begin := time.Now()
	bulkOK := false
	for _, u := range bulkURLs {
		n, err := downloadBulk(ctx, client, u)
		if n > check.BulkBytes {
			check.BulkBytes = n
		}
		if err == nil {
			bulkOK = true
			break
		}
		if ctx.Err() != nil {
			break
		}
	}
	check.BulkMs = time.Since(begin).Milliseconds()
	if !bulkOK {
		// The services no longer matter: without volume nothing opens.
		cancel()
	}
	wg.Wait()

	if !bulkOK {
		check.State = CheckBlocked
		return check
	}
	check.Services = results
	for _, res := range results {
		if !res.OK {
			check.Failed = append(check.Failed, res.Name)
		}
	}
	check.State = CheckOK
	if len(check.Failed) > 0 {
		check.State = CheckService
	}
	return check
}

// downloadBulk passes only when the whole volume arrived in time; it returns what did arrive.
func downloadBulk(ctx context.Context, client *http.Client, url string) (int64, error) {
	ctx, cancel := context.WithTimeout(ctx, checkBulkTimeout)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, "GET", url, nil)
	if err != nil {
		return 0, err
	}
	resp, err := client.Do(req)
	if err != nil {
		return 0, err
	}
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode > 299 {
		return 0, fmt.Errorf("%s: status %s", url, resp.Status)
	}
	n, err := io.Copy(io.Discard, io.LimitReader(resp.Body, checkBulkBytes))
	if err != nil {
		return n, err
	}
	if n < checkBulkBytes {
		return n, fmt.Errorf("%s: %d of %d bytes", url, n, checkBulkBytes)
	}
	return n, nil
}

// serviceAnswers is true on any HTTP answer but a 5xx: over HTTPS the answer proves the service
// itself was reached through this exit, and a 4xx or a redirect still is the service talking.
// A service that names its refusal page fails when that page is what came back.
func serviceAnswers(ctx context.Context, client *http.Client, svc checkService) (bool, string) {
	ctx, cancel := context.WithTimeout(ctx, checkServiceTimeout)
	defer cancel()
	marked := len(svc.Refused) > 0
	req, err := http.NewRequestWithContext(ctx, "GET", svc.URL, nil)
	if err != nil {
		return false, ""
	}
	req.Header.Set("User-Agent", checkUserAgent)
	req.Header.Set("Accept-Language", "en-US,en;q=0.9")
	resp, err := client.Do(req)
	if err != nil {
		if marked {
			return false, "err=" + noteText(err.Error(), 120)
		}
		return false, ""
	}
	defer resp.Body.Close()
	if !marked {
		return resp.StatusCode < 500, ""
	}
	// A cut body is not a refusal: the service did answer.
	body, readErr := io.ReadAll(io.LimitReader(resp.Body, checkServiceBodyLimit))
	note := fmt.Sprintf("status=%d len=%d at=%s%s", resp.StatusCode, len(body),
		resp.Request.URL.Host, noteText(resp.Request.URL.Path, 60))
	if readErr != nil {
		note += " cut=" + noteText(readErr.Error(), 60)
	}
	if title := between(body, "<title", "</title>"); title != "" {
		note += " title=" + noteText(title, 80)
	}
	for _, word := range []string{"country", "region"} {
		if near := around(body, word, 60); near != "" {
			note += " " + word + "=" + noteText(near, 130)
		}
	}
	if resp.StatusCode >= 500 {
		return false, note
	}
	for _, mark := range svc.Refused {
		if bytes.Contains(body, []byte(mark)) {
			return false, note + " refused=1"
		}
	}
	return true, note
}

// noteText keeps a piece of an answer on one journal line: no line breaks or quotes, cut to max.
func noteText(s string, max int) string {
	out := make([]rune, 0, max)
	for _, r := range s {
		if len(out) >= max {
			break
		}
		if r < 0x20 || r == '"' || r == '\\' {
			r = ' '
		}
		out = append(out, r)
	}
	return "'" + string(out) + "'"
}

func between(body []byte, open, end string) string {
	i := bytes.Index(body, []byte(open))
	if i < 0 {
		return ""
	}
	rest := body[i:]
	gt := bytes.IndexByte(rest, '>')
	if gt < 0 {
		return ""
	}
	rest = rest[gt+1:]
	j := bytes.Index(rest, []byte(end))
	if j < 0 {
		return ""
	}
	return string(rest[:j])
}

// around returns the text on both sides of the first word, pad bytes each way.
func around(body []byte, word string, pad int) string {
	i := bytes.Index(body, []byte(word))
	if i < 0 {
		return ""
	}
	from, to := i-pad, i+len(word)+pad
	if from < 0 {
		from = 0
	}
	if to > len(body) {
		to = len(body)
	}
	return string(bytes.ToValidUTF8(body[from:to], nil))
}
