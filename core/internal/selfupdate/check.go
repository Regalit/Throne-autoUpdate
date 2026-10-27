package selfupdate

import (
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

// Files in the work dir.
const (
	readyFile   = "ready.json" // the verified release whose package is pkg.zip
	packageFile = "pkg.zip"
	partFile    = "pkg.part"
	partMeta    = "pkg.part.json" // the release pkg.part belongs to, for resuming
	badFile     = "bad.json"      // version_code -> starts that did not come up
	okFile      = "ok"            // written by the new build once its core is up (next to pending)
	logFile     = "updater.log"
)

type checkResult struct {
	State   string `json:"state"` // none, ready, error
	Version string `json:"version,omitempty"`
	Code    int    `json:"code,omitempty"`
	Notes   string `json:"notes,omitempty"`
	Error   string `json:"error,omitempty"`
	// Kind sorts an error for the GUI, which shows a sentence per kind, not Error.
	Kind string `json:"kind,omitempty"`
}

// Error kinds.
const (
	kindNetwork = "network" // the channel could not be reached or the connection broke
	kindServer  = "server"  // the channel answered, but not with a usable release
	kindVerify  = "verify"  // signature, size or sha256 did not match
	kindDisk    = "disk"    // the work dir could not be written
)

type kindError struct {
	kind string
	err  error
}

func (e *kindError) Error() string { return e.err.Error() }
func (e *kindError) Unwrap() error { return e.err }

func withKind(kind string, err error) error { return &kindError{kind: kind, err: err} }

// failed is the error result for err; an error without a kind came from the file system.
func failed(err error) checkResult {
	res := checkResult{State: "error", Error: err.Error(), Kind: kindDisk}
	var ke *kindError
	if errors.As(err, &ke) {
		res.Kind = ke.kind
	}
	return res
}

// progressLine is printed while a package downloads, before the final checkResult line:
// {"state":"downloading","version":"1.0.9-dev","done":N,"size":M}.
type progressLine struct {
	State   string `json:"state"`
	Version string `json:"version"`
	Done    int64  `json:"done"`
	Size    int64  `json:"size"`
}

// progressEvery is how often check prints a progress line at most.
const progressEvery = 250 * time.Millisecond

// progressPrinter writes progress lines to out, at most one per every, but always
// the first one and the one that reaches size.
type progressPrinter struct {
	out     io.Writer
	every   time.Duration
	now     func() time.Time
	last    time.Time
	printed bool
}

func (p *progressPrinter) report(version string, done, size int64) {
	t := p.now()
	if p.printed && done < size && t.Sub(p.last) < p.every {
		return
	}
	p.printed, p.last = true, t
	_ = json.NewEncoder(p.out).Encode(progressLine{State: "downloading", Version: version, Done: done, Size: size})
}

func runCheck(args []string) error {
	fs := flag.NewFlagSet("check", flag.ContinueOnError)
	base := fs.String("base", "", "channel base URL, e.g. https://host/api/desktop/app/dev")
	appID := fs.String("appid", "", "expected app_id")
	code := fs.Int("code", 0, "running version_code")
	work := fs.String("work", "", "work dir")
	if err := fs.Parse(args); err != nil {
		return err
	}
	if *base == "" || *appID == "" || *code <= 0 || *work == "" {
		return errors.New("check needs -base, -appid, -code and -work")
	}
	if err := os.MkdirAll(*work, 0o755); err != nil {
		return err
	}
	progress := &progressPrinter{out: os.Stdout, every: progressEvery, now: time.Now}
	res := checkWithProgress(&http.Client{Timeout: 10 * time.Minute}, strings.TrimRight(*base, "/"),
		*appID, *code, *work, progress.report)
	logf(*work, "check: %s %s %d %s %s", res.State, res.Version, res.Code, res.Kind, res.Error)
	return json.NewEncoder(os.Stdout).Encode(res)
}

func check(client *http.Client, base, appID string, code int, work string) checkResult {
	return checkWithProgress(client, base, appID, code, work, nil)
}

// checkWithProgress is check that calls progress (when not nil) as the package downloads.
func checkWithProgress(client *http.Client, base, appID string, code int, work string,
	progress func(version string, done, size int64)) checkResult {
	sweep(work)
	rel, err := fetchLatest(client, base)
	if err != nil {
		return failed(err)
	}
	if rel == nil || rel.AppID != appID || rel.VersionCode <= code || isBad(work, rel.VersionCode) {
		// Nothing newer. A stale package from an older announcement is dropped.
		if rel != nil && rel.VersionCode <= code {
			clearPackage(work)
		}
		return checkResult{State: "none"}
	}
	if err := rel.validate(appID, publicKey); err != nil {
		return checkResult{State: "error", Error: "refused: " + err.Error(), Kind: kindVerify}
	}
	ready := checkResult{State: "ready", Version: rel.Version, Code: rel.VersionCode, Notes: rel.Notes}
	if prev, err := readRelease(filepath.Join(work, readyFile)); err == nil && *prev == *rel {
		if ok, _ := fileMatches(filepath.Join(work, packageFile), rel.Size, rel.SHA256); ok {
			return ready
		}
	}
	// A finished package of another release goes; a part download stays for download to resume.
	_ = os.Remove(filepath.Join(work, readyFile))
	_ = os.Remove(filepath.Join(work, packageFile))
	if err := download(client, base, rel, work, progress); err != nil {
		return failed(err)
	}
	return ready
}

func fetchLatest(client *http.Client, base string) (*Release, error) {
	resp, err := client.Get(base + "/latest?platform=windows")
	if err != nil {
		return nil, withKind(kindNetwork, err)
	}
	defer resp.Body.Close()
	if resp.StatusCode == http.StatusNotFound {
		return nil, nil // channel not configured yet
	}
	if resp.StatusCode != http.StatusOK {
		return nil, withKind(kindServer, fmt.Errorf("latest: HTTP %d", resp.StatusCode))
	}
	var rel Release
	if err := json.NewDecoder(io.LimitReader(resp.Body, 64<<10)).Decode(&rel); err != nil {
		// A body cut off mid-way is a broken connection; anything else is the server's.
		kind := kindServer
		var ne net.Error
		if errors.As(err, &ne) || errors.Is(err, io.ErrUnexpectedEOF) {
			kind = kindNetwork
		}
		return nil, withKind(kind, fmt.Errorf("latest: %w", err))
	}
	return &rel, nil
}

// download fetches the package into pkg.part, resuming a part of the same
// release, and only renames it to pkg.zip once size and sha256 match.
func download(client *http.Client, base string, rel *Release, work string,
	progress func(version string, done, size int64)) error {
	part := filepath.Join(work, partFile)
	var have int64
	if prev, err := readRelease(filepath.Join(work, partMeta)); err == nil && *prev == *rel {
		if st, err := os.Stat(part); err == nil && st.Size() < rel.Size {
			have = st.Size()
		}
	}
	if have == 0 {
		_ = os.Remove(part)
		if err := writeJSON(filepath.Join(work, partMeta), rel); err != nil {
			return err
		}
	}

	req, err := http.NewRequest(http.MethodGet, base+"/download?platform=windows", nil)
	if err != nil {
		return err
	}
	if have > 0 {
		req.Header.Set("Range", fmt.Sprintf("bytes=%d-", have))
	}
	resp, err := client.Do(req)
	if err != nil {
		return withKind(kindNetwork, err)
	}
	defer resp.Body.Close()

	flags := os.O_WRONLY | os.O_CREATE
	switch {
	case have > 0 && resp.StatusCode == http.StatusPartialContent:
		flags |= os.O_APPEND
	case resp.StatusCode == http.StatusOK:
		flags |= os.O_TRUNC
		have = 0
	default:
		return withKind(kindServer, fmt.Errorf("download: HTTP %d", resp.StatusCode))
	}
	f, err := os.OpenFile(part, flags, 0o644)
	if err != nil {
		return err
	}
	copyErr := copyProgress(f, io.LimitReader(resp.Body, rel.Size-have+1), have, rel, progress)
	if err := f.Close(); copyErr == nil {
		copyErr = err
	}
	if copyErr != nil {
		return fmt.Errorf("download: %w", copyErr) // the part stays for the next try
	}

	ok, err := fileMatches(part, rel.Size, rel.SHA256)
	if err != nil {
		return err
	}
	if !ok {
		_ = os.Remove(part)
		_ = os.Remove(filepath.Join(work, partMeta))
		return withKind(kindVerify, errors.New("download: size or sha256 mismatch"))
	}
	if err := os.Rename(part, filepath.Join(work, packageFile)); err != nil {
		return err
	}
	_ = os.Remove(filepath.Join(work, partMeta))
	return writeJSON(filepath.Join(work, readyFile), rel)
}

// copyProgress copies body into f and reports how much of the package is held,
// counting the have bytes of a resumed part. A read error is the network's, a
// write error the disk's.
func copyProgress(f io.Writer, body io.Reader, have int64, rel *Release,
	progress func(version string, done, size int64)) error {
	report := func(done int64) {
		if progress != nil {
			progress(rel.Version, min(done, rel.Size), rel.Size)
		}
	}
	report(have)
	buf := make([]byte, 64<<10)
	for {
		n, rerr := body.Read(buf)
		if n > 0 {
			if _, werr := f.Write(buf[:n]); werr != nil {
				return withKind(kindDisk, werr)
			}
			have += int64(n)
			report(have)
		}
		if rerr == io.EOF {
			return nil
		}
		if rerr != nil {
			return withKind(kindNetwork, rerr)
		}
	}
}

// sweep removes what a finished apply could not: the old files it moved aside,
// one of them the image of the apply process itself, which Windows only lets go
// of once that process has ended. Nothing is swept while an apply is under way
// or has to be undone (its journal is still there).
func sweep(work string) {
	if exists(filepath.Join(work, applyFile)) || exists(filepath.Join(work, lockFile)) {
		return
	}
	for _, d := range []string{previousDir, stagingDir, incomingDir, trashDir} {
		_ = os.RemoveAll(filepath.Join(work, d))
	}
}

func clearPackage(work string) {
	for _, name := range []string{readyFile, packageFile, partFile, partMeta} {
		_ = os.Remove(filepath.Join(work, name))
	}
}

func readRelease(path string) (*Release, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var rel Release
	if err := json.Unmarshal(b, &rel); err != nil {
		return nil, err
	}
	return &rel, nil
}

func writeJSON(path string, v any) error {
	b, err := json.Marshal(v)
	if err != nil {
		return err
	}
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, b, 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

func isBad(work string, code int) bool {
	return readBad(work)[strconv.Itoa(code)] >= maxFailures
}

// readBad returns how often each version_code failed to come up after an apply.
func readBad(work string) map[string]int {
	failures := map[string]int{}
	if b, err := os.ReadFile(filepath.Join(work, badFile)); err == nil {
		_ = json.Unmarshal(b, &failures)
	}
	return failures
}

func logf(work, format string, args ...any) {
	f, err := os.OpenFile(filepath.Join(work, logFile), os.O_WRONLY|os.O_CREATE|os.O_APPEND, 0o644)
	if err != nil {
		return
	}
	defer f.Close()
	fmt.Fprintf(f, "%s "+format+"\n", append([]any{time.Now().UTC().Format(time.RFC3339)}, args...)...)
}
