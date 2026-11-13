package probe

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"net/http"
	"regexp"
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
	// Country finds, in the page, the country the service takes this exit for; Closed lists the
	// countries it does not serve. For a service that hands every country the same page and
	// refuses only later, inside the page, this is the one place the refusal shows.
	Country *regexp.Regexp
	Closed  []string
}

const (
	checkServiceBodyLimit = 384 * 1024
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
	// Gemini: the app page is the same everywhere; its page data carries the country Google sees
	// (about 140 KB in), and the app then refuses the countries outside Google's supported list.
	{Name: "Gemini", URL: "https://gemini.google.com/app",
		Country: regexp.MustCompile(`"vXmutd":"%\.@\.\\"([A-Z]{2})\\"`),
		Closed:  []string{"RU", "BY", "CN", "HK", "MO", "IR", "KP", "CU", "SY"}},
	{Name: "ChatGPT", URL: "https://api.openai.com/compliance/cookie_requirements", Refused: []string{"unsupported_country"}},
}

// ServiceResult is one service asked through one server.
type ServiceResult struct {
	Name string
	OK   bool
	Ms   int64 // time to the answer; meaningful only when OK
	// Note says what came back from a service that can refuse a country (status, the country it
	// saw, whether it refused), for the journal.
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
	marked := len(svc.Refused) > 0 || svc.Country != nil
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
	body, _ := io.ReadAll(io.LimitReader(resp.Body, checkServiceBodyLimit))
	note := fmt.Sprintf("status=%d", resp.StatusCode)
	if resp.StatusCode >= 500 {
		return false, note
	}
	for _, mark := range svc.Refused {
		if bytes.Contains(body, []byte(mark)) {
			return false, note + " refused=1"
		}
	}
	if svc.Country != nil {
		found := svc.Country.FindSubmatch(body)
		if found == nil {
			// The page changed or was cut: nothing says the country is refused.
			return true, note + " country=none"
		}
		country := string(found[1])
		note += " country=" + country
		for _, closed := range svc.Closed {
			if country == closed {
				return false, note + " refused=1"
			}
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
