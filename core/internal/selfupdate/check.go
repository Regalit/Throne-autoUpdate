package selfupdate

import (
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
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
	res := check(&http.Client{Timeout: 10 * time.Minute}, strings.TrimRight(*base, "/"), *appID, *code, *work)
	logf(*work, "check: %s %s %d %s", res.State, res.Version, res.Code, res.Error)
	return json.NewEncoder(os.Stdout).Encode(res)
}

func check(client *http.Client, base, appID string, code int, work string) checkResult {
	sweep(work)
	rel, err := fetchLatest(client, base)
	if err != nil {
		return checkResult{State: "error", Error: err.Error()}
	}
	if rel == nil || rel.AppID != appID || rel.VersionCode <= code || isBad(work, rel.VersionCode) {
		// Nothing newer. A stale package from an older announcement is dropped.
		if rel != nil && rel.VersionCode <= code {
			clearPackage(work)
		}
		return checkResult{State: "none"}
	}
	if err := rel.validate(appID, publicKey); err != nil {
		return checkResult{State: "error", Error: "refused: " + err.Error()}
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
	if err := download(client, base, rel, work); err != nil {
		return checkResult{State: "error", Error: err.Error()}
	}
	return ready
}

func fetchLatest(client *http.Client, base string) (*Release, error) {
	resp, err := client.Get(base + "/latest?platform=windows")
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode == http.StatusNotFound {
		return nil, nil // channel not configured yet
	}
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("latest: HTTP %d", resp.StatusCode)
	}
	var rel Release
	if err := json.NewDecoder(io.LimitReader(resp.Body, 64<<10)).Decode(&rel); err != nil {
		return nil, fmt.Errorf("latest: %w", err)
	}
	return &rel, nil
}

// download fetches the package into pkg.part, resuming a part of the same
// release, and only renames it to pkg.zip once size and sha256 match.
func download(client *http.Client, base string, rel *Release, work string) error {
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
		return err
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
		return fmt.Errorf("download: HTTP %d", resp.StatusCode)
	}
	f, err := os.OpenFile(part, flags, 0o644)
	if err != nil {
		return err
	}
	_, copyErr := io.Copy(f, io.LimitReader(resp.Body, rel.Size-have+1))
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
		return errors.New("download: size or sha256 mismatch")
	}
	if err := os.Rename(part, filepath.Join(work, packageFile)); err != nil {
		return err
	}
	_ = os.Remove(filepath.Join(work, partMeta))
	return writeJSON(filepath.Join(work, readyFile), rel)
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
