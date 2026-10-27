package selfupdate

import (
	"archive/zip"
	"bytes"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

// The test binary doubles as the "app" an apply starts: with UPDATER_TEST_APP
// set it writes the ok file (or crashes) and exits.
func TestMain(m *testing.M) {
	switch os.Getenv("UPDATER_TEST_APP") {
	case "ok":
		_ = os.WriteFile(filepath.Join(os.Getenv("UPDATER_TEST_WORK"), okFile), nil, 0o644)
		os.Exit(0)
	case "crash":
		os.Exit(3)
	}
	os.Exit(m.Run())
}

var testPub, testPriv, _ = ed25519.GenerateKey(nil)

func useTestKey(t *testing.T) {
	old := publicKey
	publicKey = testPub
	t.Cleanup(func() { publicKey = old })
}

func makeZip(t *testing.T, files map[string][]byte) []byte {
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	for name, data := range files {
		w, err := zw.Create(name)
		if err != nil {
			t.Fatal(err)
		}
		w.Write(data)
	}
	if err := zw.Close(); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}

func release(pkg []byte, code int, priv ed25519.PrivateKey) Release {
	sum := sha256.Sum256(pkg)
	r := Release{
		AppID: "shadowlos-throne-dev", Platform: "windows", Version: "1.0.1-dev", VersionCode: code,
		SHA256: hex.EncodeToString(sum[:]), Size: int64(len(pkg)),
	}
	r.Sig = base64.StdEncoding.EncodeToString(ed25519.Sign(priv, []byte(r.signedMessage())))
	return r
}

type fakeServer struct {
	rel       Release
	pkg       []byte
	cutAfter  int64 // >0: the first full download stops after this many bytes
	downloads atomic.Int32
	ranges    []string
}

func (f *fakeServer) start(t *testing.T) string {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case strings.HasSuffix(r.URL.Path, "/latest"):
			json.NewEncoder(w).Encode(f.rel)
		case strings.HasSuffix(r.URL.Path, "/download"):
			n := f.downloads.Add(1)
			f.ranges = append(f.ranges, r.Header.Get("Range"))
			if n == 1 && f.cutAfter > 0 && r.Header.Get("Range") == "" {
				w.Header().Set("Content-Length", "999999")
				w.Write(f.pkg[:f.cutAfter])
				w.(http.Flusher).Flush()
				if hj, ok := w.(http.Hijacker); ok {
					c, _, _ := hj.Hijack()
					c.Close()
				}
				return
			}
			http.ServeContent(w, r, "pkg.zip", time.Time{}, bytes.NewReader(f.pkg))
		default:
			http.NotFound(w, r)
		}
	}))
	t.Cleanup(srv.Close)
	return srv.URL + "/api/desktop/app/dev"
}

func TestCheckDownloadsAndVerifies(t *testing.T) {
	useTestKey(t)
	pkg := makeZip(t, map[string][]byte{"App.exe": []byte("new")})
	f := &fakeServer{pkg: pkg, rel: release(pkg, 10001, testPriv)}
	base, work := f.start(t), t.TempDir()

	res := check(http.DefaultClient, base, "shadowlos-throne-dev", 10000, work)
	if res.State != "ready" || res.Code != 10001 {
		t.Fatalf("got %+v", res)
	}
	// A second check reuses the verified package.
	if res := check(http.DefaultClient, base, "shadowlos-throne-dev", 10000, work); res.State != "ready" || f.downloads.Load() != 1 {
		t.Fatalf("got %+v after %d downloads", res, f.downloads.Load())
	}
	// The running build is already that version: nothing to do, package dropped.
	if res := check(http.DefaultClient, base, "shadowlos-throne-dev", 10001, work); res.State != "none" {
		t.Fatalf("got %+v", res)
	}
	if _, err := os.Stat(filepath.Join(work, packageFile)); !os.IsNotExist(err) {
		t.Fatal("stale package kept")
	}
}

func TestCheckRefuses(t *testing.T) {
	useTestKey(t)
	pkg := makeZip(t, map[string][]byte{"App.exe": []byte("new")})
	_, otherPriv, _ := ed25519.GenerateKey(nil)
	cases := map[string]func(r *Release){
		"signature by another key": func(r *Release) { *r = release(pkg, 10001, otherPriv) },
		"sha256 of another file":   func(r *Release) { r.SHA256 = strings.Repeat("0", 64) },
		// An old signed package announced as newer: the signature binds the version.
		"version raised by the server": func(r *Release) { r.VersionCode = 10099 },
		// The signature binds the size too; only the hex used to be signed.
		"signed over the bare sha256": func(r *Release) {
			r.Sig = base64.StdEncoding.EncodeToString(ed25519.Sign(testPriv, []byte(r.SHA256)))
		},
	}
	for name, mutate := range cases {
		t.Run(name, func(t *testing.T) {
			rel := release(pkg, 10001, testPriv)
			mutate(&rel)
			f := &fakeServer{pkg: pkg, rel: rel}
			base, work := f.start(t), t.TempDir()
			res := check(http.DefaultClient, base, "shadowlos-throne-dev", 10000, work)
			if res.State != "error" {
				t.Fatalf("got %+v", res)
			}
			if _, err := os.Stat(filepath.Join(work, readyFile)); !os.IsNotExist(err) {
				t.Fatal("refused release marked ready")
			}
		})
	}
	t.Run("signed but the file differs", func(t *testing.T) {
		rel := release(pkg, 10001, testPriv)
		f := &fakeServer{pkg: makeZip(t, map[string][]byte{"App.exe": []byte("evil")}), rel: rel}
		f.rel.Size = int64(len(f.pkg))
		base, work := f.start(t), t.TempDir()
		if res := check(http.DefaultClient, base, "shadowlos-throne-dev", 10000, work); res.State != "error" {
			t.Fatalf("got %+v", res)
		}
	})
	t.Run("prod release on the dev channel", func(t *testing.T) {
		f := &fakeServer{pkg: pkg, rel: release(pkg, 10001, testPriv)}
		f.rel.AppID = "shadowlos-throne"
		base, work := f.start(t), t.TempDir()
		if res := check(http.DefaultClient, base, "shadowlos-throne-dev", 10000, work); res.State != "none" {
			t.Fatalf("got %+v", res)
		}
	})
}

func TestCheckResumesCutDownload(t *testing.T) {
	useTestKey(t)
	pkg := makeZip(t, map[string][]byte{"App.exe": bytes.Repeat([]byte("x"), 200000)})
	f := &fakeServer{pkg: pkg, rel: release(pkg, 10001, testPriv), cutAfter: 100}
	base, work := f.start(t), t.TempDir()
	if res := check(http.DefaultClient, base, "shadowlos-throne-dev", 10000, work); res.State != "error" {
		t.Fatalf("cut download: got %+v", res)
	}
	if res := check(http.DefaultClient, base, "shadowlos-throne-dev", 10000, work); res.State != "ready" {
		t.Fatalf("resume: got %+v", res)
	}
	if len(f.ranges) != 2 || f.ranges[1] != "bytes=100-" {
		t.Fatalf("second download did not resume: ranges %q", f.ranges)
	}
}

// prepare puts an installed "old" app and a verified ready package for a new
// one into place; the new App.exe is this test binary.
func prepare(t *testing.T, extra map[string][]byte) (dir, work string) {
	useTestKey(t)
	self, err := os.ReadFile(os.Args[0])
	if err != nil {
		t.Fatal(err)
	}
	files := map[string][]byte{"App.exe": self, "lib/new.dll": []byte("new")}
	for k, v := range extra {
		files[k] = v
	}
	pkg := makeZip(t, files)
	rel := release(pkg, 10001, testPriv)
	dir = t.TempDir()
	work = filepath.Join(dir, "update")
	os.MkdirAll(filepath.Join(dir, "config"), 0o755)
	os.WriteFile(filepath.Join(dir, "config", "throne.db"), []byte("user data"), 0o644)
	os.WriteFile(filepath.Join(dir, "App.exe"), []byte("old exe"), 0o755)
	os.MkdirAll(work, 0o755)
	os.WriteFile(filepath.Join(work, packageFile), pkg, 0o644)
	if err := writeJSON(filepath.Join(work, readyFile), &rel); err != nil {
		t.Fatal(err)
	}
	return dir, work
}

func opts(dir, work string, timeout time.Duration) applyOpts {
	return applyOpts{dir: dir, src: work, exe: "App.exe", appID: "shadowlos-throne-dev", timeout: timeout}
}

func TestApplyKeepsNewBuildThatStarts(t *testing.T) {
	dir, work := prepare(t, nil)
	t.Setenv("UPDATER_TEST_APP", "ok")
	t.Setenv("UPDATER_TEST_WORK", work)
	if err := apply(opts(dir, work, 30*time.Second), work); err != nil {
		t.Fatal(err)
	}
	if b, _ := os.ReadFile(filepath.Join(dir, "App.exe")); string(b) == "old exe" {
		t.Fatal("old exe still in place")
	}
	if b, _ := os.ReadFile(filepath.Join(dir, "lib", "new.dll")); string(b) != "new" {
		t.Fatal("new file missing")
	}
	if b, _ := os.ReadFile(filepath.Join(dir, "config", "throne.db")); string(b) != "user data" {
		t.Fatal("config touched")
	}
	for _, p := range []string{previousDir, stagingDir, incomingDir, applyFile, pendingFile, readyFile, packageFile} {
		if exists(filepath.Join(work, p)) {
			t.Fatalf("%s kept after a good start", p)
		}
	}
}

func TestApplyRollsBackBuildThatDoesNotStart(t *testing.T) {
	dir, work := prepare(t, nil)
	t.Setenv("UPDATER_TEST_APP", "crash")
	if err := apply(opts(dir, work, 30*time.Second), work); err == nil {
		t.Fatal("apply of a crashing build succeeded")
	}
	if b, _ := os.ReadFile(filepath.Join(dir, "App.exe")); string(b) != "old exe" {
		t.Fatal("old exe not restored")
	}
	if _, err := os.Stat(filepath.Join(dir, "lib", "new.dll")); !os.IsNotExist(err) {
		t.Fatal("added file left behind")
	}
	if n := readBad(work)["10001"]; n != 1 || isBad(work, 10001) {
		t.Fatalf("one failure: count %d, skipped %v", n, isBad(work, 10001))
	}
	markFailure(work, 10001)
	if !isBad(work, 10001) {
		t.Fatal("not skipped after the second failure")
	}
	for _, d := range []string{stagingDir, previousDir, applyFile} {
		if _, err := os.Stat(filepath.Join(work, d)); !os.IsNotExist(err) {
			t.Fatalf("%s left after a rollback", d)
		}
	}
}

func TestApplyRefusesConfigInPackage(t *testing.T) {
	dir, work := prepare(t, map[string][]byte{"config/throne.db": []byte("overwrite")})
	if err := apply(opts(dir, work, time.Second), work); err == nil {
		t.Fatal("package with config/ applied")
	}
	if b, _ := os.ReadFile(filepath.Join(dir, "config", "throne.db")); string(b) != "user data" {
		t.Fatal("config touched")
	}
	if b, _ := os.ReadFile(filepath.Join(dir, "App.exe")); string(b) != "old exe" {
		t.Fatal("app changed")
	}
}

func TestApplyRefusesOtherAppID(t *testing.T) {
	dir, work := prepare(t, nil)
	o := opts(dir, work, time.Second)
	o.appID = "shadowlos-throne" // a prod build must not take a dev package
	if err := apply(o, work); err == nil {
		t.Fatal("dev package applied to prod")
	}
	if b, _ := os.ReadFile(filepath.Join(dir, "App.exe")); string(b) != "old exe" {
		t.Fatal("app changed")
	}
}

func TestApplyUndoesUnfinishedApply(t *testing.T) {
	dir, work := prepare(t, nil)
	// A crash after App.exe was moved out and the new one put in, with lib/new.dll still to come.
	os.MkdirAll(filepath.Join(work, previousDir), 0o755)
	os.Rename(filepath.Join(dir, "App.exe"), filepath.Join(work, previousDir, "App.exe"))
	os.WriteFile(filepath.Join(dir, "App.exe"), []byte("half-new"), 0o755)
	writeJSON(filepath.Join(work, applyFile), &applyState{Code: 10001, Replaced: []string{"App.exe"}, Added: []string{"lib/new.dll"}})
	os.Remove(filepath.Join(work, readyFile)) // nothing new to apply this time
	if err := apply(opts(dir, work, time.Second), work); err == nil {
		t.Fatal("apply without a package succeeded")
	}
	if b, _ := os.ReadFile(filepath.Join(dir, "App.exe")); string(b) != "old exe" {
		t.Fatalf("old exe not restored, got %q", b)
	}
	if exists(filepath.Join(work, applyFile)) {
		t.Fatal("journal kept after the undo")
	}
}

func TestSafeName(t *testing.T) {
	for name, want := range map[string]bool{
		"App.exe": true, "lib/new.dll": true, "translations/ru.qm": true,
		"": false, "/abs.exe": false, "../x": false, "a/../b": false, "./a": false, "a//b": false,
		"C:/x": false, "a:stream": false, `a`: false,
		"config./x": false, "config /x": false, "x.exe.": false, "x ": false,
		"CON": false, "nul.txt": false, "lib/com1.dll": false, "LPT9": false,
	} {
		if got := safeName(name); got != want {
			t.Errorf("safeName(%q) = %v, want %v", name, got, want)
		}
	}
}

func TestCheckSweepsLeftoversOfAFinishedApply(t *testing.T) {
	work := t.TempDir()
	os.MkdirAll(filepath.Join(work, previousDir), 0o755)
	os.WriteFile(filepath.Join(work, previousDir, "ThroneCore.exe"), []byte("old"), 0o644)
	sweep(work)
	if exists(filepath.Join(work, previousDir)) {
		t.Fatal("previous kept")
	}
	os.MkdirAll(filepath.Join(work, previousDir), 0o755)
	writeJSON(filepath.Join(work, applyFile), &applyState{Code: 1})
	sweep(work)
	if !exists(filepath.Join(work, previousDir)) {
		t.Fatal("previous swept while an apply still has to be undone")
	}
}
