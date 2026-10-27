package selfupdate

import (
	"archive/zip"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

// Files and dirs of an apply. The trusted work dir is <app dir>/update: under
// Program Files only an elevated apply can write there, so nothing in it can be
// swapped by an unprivileged process between a check and its use. The source
// dir is where the app downloaded the package (the same dir, or %LOCALAPPDATA%
// for a read-only install); the package is copied out of it before it is verified.
const (
	stagingDir  = "staging"
	previousDir = "previous"
	incomingDir = "incoming"
	trashDir    = "trash"
	applyFile   = "apply.json" // what this apply replaced and added, for a rollback (also after a crash)
	lockFile    = "apply.lock"
	pendingFile = "pending" // in the source dir: tells the new build to write ok once its core is up
	maxFailures = 2         // a version is skipped after this many starts that did not come up
)

type applyState struct {
	Code     int      `json:"code"`
	Replaced []string `json:"replaced"`
	Added    []string `json:"added"`
}

type applyOpts struct {
	dir, src, exe, appID string
	timeout              time.Duration
	deelevate            bool // started elevated for a read-only install: start the app as the user
}

func runApply(args []string) error {
	fs := flag.NewFlagSet("apply", flag.ContinueOnError)
	var o applyOpts
	fs.StringVar(&o.dir, "dir", "", "app dir")
	fs.StringVar(&o.src, "src", "", "dir the app downloaded the package to")
	fs.StringVar(&o.exe, "exe", "", "exe to start, a plain name in the app dir")
	fs.StringVar(&o.appID, "appid", "", "app_id of this build")
	pid := fs.Int("pid", 0, "app process to wait for")
	fs.DurationVar(&o.timeout, "timeout", 3*time.Minute, "how long the new build has to come up")
	fs.BoolVar(&o.deelevate, "deelevate", false, "start the app unelevated")
	if err := fs.Parse(args); err != nil {
		return err
	}
	if o.dir == "" || o.src == "" || o.appID == "" || o.exe == "" || filepath.Base(o.exe) != o.exe {
		return errors.New("apply needs -dir, -src, -appid and a plain -exe name")
	}
	work := filepath.Join(o.dir, "update")
	if err := os.MkdirAll(work, 0o755); err != nil {
		return err
	}
	unlock, err := lock(work)
	if err != nil {
		return err
	}
	defer unlock()

	waitExit(*pid, 60*time.Second)
	// The old core, a check still downloading: nothing may hold the files we move.
	killDirProcesses(o.dir)

	err = apply(o, work)
	if err != nil {
		logf(o.src, "apply: %v", err)
		// Whatever happened, the user gets a working app back.
		_ = startApp(o)
	}
	return err
}

func apply(o applyOpts, work string) error {
	if st, err := readApplyState(work); err == nil {
		// A previous apply died half-way (power loss, logoff): put that right first.
		logf(o.src, "apply: undoing an unfinished apply of %d", st.Code)
		rollback(o.dir, work, st, o.src)
	}
	for _, d := range []string{stagingDir, previousDir, incomingDir, trashDir} {
		_ = os.RemoveAll(filepath.Join(work, d))
	}

	rel, pkg, closePkg, err := takePackage(o, work)
	if err != nil {
		clearPackage(o.src)
		return fmt.Errorf("refused: %w", err)
	}
	staging := filepath.Join(work, stagingDir)
	files, err := extract(pkg, staging)
	closePkg()
	if err != nil {
		clearPackage(o.src)
		return fmt.Errorf("refused: %w", err)
	}
	if !contains(files, o.exe) {
		clearPackage(o.src)
		return fmt.Errorf("refused: package has no %s", o.exe)
	}

	prev := filepath.Join(work, previousDir)
	st := &applyState{Code: rel.VersionCode}
	for _, f := range files {
		dst := filepath.Join(o.dir, f)
		replaced := exists(dst)
		if replaced {
			st.Replaced = append(st.Replaced, f)
		} else {
			st.Added = append(st.Added, f)
		}
		// Journal first, so a crash between the two moves is still undone right.
		if err := writeJSON(filepath.Join(work, applyFile), st); err != nil {
			rollback(o.dir, work, st, o.src)
			return err
		}
		if replaced {
			if err := retry(func() error { return move(dst, filepath.Join(prev, f)) }); err != nil {
				rollback(o.dir, work, st, o.src)
				return fmt.Errorf("move out %s: %w", f, err)
			}
		}
		if err := retry(func() error { return move(filepath.Join(staging, f), dst) }); err != nil {
			rollback(o.dir, work, st, o.src)
			return fmt.Errorf("move in %s: %w", f, err)
		}
	}
	logf(o.src, "apply: %s in place (%d replaced, %d added), starting it", rel.Version, len(st.Replaced), len(st.Added))

	_ = os.Remove(filepath.Join(o.src, okFile))
	if err := os.WriteFile(filepath.Join(o.src, pendingFile), nil, 0o644); err != nil {
		rollback(o.dir, work, st, o.src)
		return err
	}
	if err := startAndConfirm(o); err != nil {
		killDirProcesses(o.dir)
		rollback(o.dir, work, st, o.src)
		n := markFailure(o.src, rel.VersionCode)
		clearPackage(o.src)
		cleanup(work, o.src)
		return fmt.Errorf("%s did not start (%v); rolled back (failure %d of %d before it is skipped)", rel.Version, err, n, maxFailures)
	}
	logf(o.src, "apply: %s started", rel.Version)
	clearPackage(o.src)
	cleanup(work, o.src)
	return nil
}

// takePackage copies ready.json and pkg.zip from the source dir into the
// trusted work dir and verifies that copy: this build's app_id, the signature,
// and size and sha256 read through the handle the zip is then read from.
// The caller closes that handle with the returned func.
func takePackage(o applyOpts, work string) (*Release, *zip.Reader, func(), error) {
	rel, err := readRelease(filepath.Join(o.src, readyFile))
	if err != nil {
		return nil, nil, nil, fmt.Errorf("no ready package: %w", err)
	}
	if err := rel.validate(o.appID, publicKey); err != nil {
		return nil, nil, nil, err
	}
	in := filepath.Join(work, incomingDir)
	if err := os.MkdirAll(in, 0o755); err != nil {
		return nil, nil, nil, err
	}
	copyPath := filepath.Join(in, packageFile)
	if err := copyFile(filepath.Join(o.src, packageFile), copyPath, rel.Size+1); err != nil {
		return nil, nil, nil, err
	}
	f, err := os.Open(copyPath)
	if err != nil {
		return nil, nil, nil, err
	}
	fail := func(err error) (*Release, *zip.Reader, func(), error) {
		f.Close()
		return nil, nil, nil, err
	}
	h := sha256.New()
	n, err := io.Copy(h, f)
	if err != nil {
		return fail(err)
	}
	if n != rel.Size || hex.EncodeToString(h.Sum(nil)) != rel.SHA256 {
		return fail(errors.New("package does not match its size and sha256"))
	}
	zr, err := zip.NewReader(f, n)
	if err != nil {
		return fail(err)
	}
	return rel, zr, func() { f.Close() }, nil
}

// startAndConfirm starts the new build and waits for it to write ok next to
// the pending marker once its core is up. A build that relaunches itself
// (elevation) counts as long as something runs from the app dir; when nothing
// does any more, or at the timeout, it has failed.
func startAndConfirm(o applyOpts) error {
	if err := startApp(o); err != nil {
		return err
	}
	started := time.Now()
	deadline := time.After(o.timeout)
	tick := time.NewTicker(500 * time.Millisecond)
	defer tick.Stop()
	for {
		select {
		case <-deadline:
			return errors.New("not up in time")
		case <-tick.C:
			if exists(filepath.Join(o.src, okFile)) {
				return nil
			}
			if time.Since(started) > 10*time.Second && !anyDirProcess(o.dir) {
				if exists(filepath.Join(o.src, okFile)) {
					return nil // it was up, then the user closed it
				}
				return errors.New("exited before it was up")
			}
		}
	}
}

// rollback undoes an apply from its journal: new files out of the way, the
// previous ones back. A file that cannot be removed (still mapped by a dying
// process) is renamed aside, which Windows allows.
func rollback(dir, work string, st *applyState, logDir string) {
	trash := filepath.Join(work, trashDir)
	aside := func(p string) error {
		if !exists(p) {
			return nil
		}
		return retry(func() error {
			return move(p, filepath.Join(trash, strconv.FormatInt(time.Now().UnixNano(), 36)+"-"+filepath.Base(p)))
		})
	}
	failed := 0
	for _, f := range st.Added {
		if err := aside(filepath.Join(dir, f)); err != nil {
			failed++
			logf(logDir, "rollback: %s: %v", f, err)
		}
	}
	for _, f := range st.Replaced {
		dst := filepath.Join(dir, f)
		old := filepath.Join(work, previousDir, f)
		if !exists(old) {
			continue // a crash before this file was moved out: it is still the old one
		}
		err := aside(dst)
		if err == nil {
			err = retry(func() error { return move(old, dst) })
		}
		if err != nil {
			failed++
			logf(logDir, "rollback: %s: %v", f, err)
		}
	}
	if failed == 0 {
		_ = os.Remove(filepath.Join(work, applyFile))
	}
	_ = os.RemoveAll(trash)
	logf(logDir, "rollback: done (%d restored, %d removed, %d failed)", len(st.Replaced), len(st.Added), failed)
}

func cleanup(work, src string) {
	for _, p := range []string{
		filepath.Join(work, previousDir), filepath.Join(work, stagingDir), filepath.Join(work, incomingDir),
		filepath.Join(work, trashDir), filepath.Join(work, applyFile),
		filepath.Join(src, okFile), filepath.Join(src, pendingFile),
	} {
		_ = os.RemoveAll(p)
	}
}

// extract unpacks the package into staging and returns its files (slash
// paths). It refuses names that leave the dir or that Windows would read
// differently (trailing dots and spaces, device names, case duplicates), and
// anything under config/ or update/, which belong to the user and the updater.
func extract(zr *zip.Reader, staging string) ([]string, error) {
	var files []string
	seen := map[string]bool{}
	for _, zf := range zr.File {
		if zf.FileInfo().IsDir() {
			continue
		}
		name := zf.Name
		if !safeName(name) {
			return nil, fmt.Errorf("bad path %q in package", zf.Name)
		}
		key := strings.ToLower(name)
		top := strings.SplitN(key, "/", 2)[0]
		if seen[key] || top == "config" || top == "update" {
			return nil, fmt.Errorf("bad path %q in package", zf.Name)
		}
		seen[key] = true
		dst := filepath.Join(staging, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
			return nil, err
		}
		if err := writeZipFile(zf, dst); err != nil {
			return nil, err
		}
		files = append(files, name)
	}
	if len(files) == 0 {
		return nil, errors.New("empty package")
	}
	return files, nil
}

var reservedNames = map[string]bool{"con": true, "prn": true, "aux": true, "nul": true,
	"com1": true, "com2": true, "com3": true, "com4": true, "com5": true, "com6": true, "com7": true, "com8": true, "com9": true,
	"lpt1": true, "lpt2": true, "lpt3": true, "lpt4": true, "lpt5": true, "lpt6": true, "lpt7": true, "lpt8": true, "lpt9": true}

// safeName accepts a relative slash path whose every part is a plain Windows
// file name: no empty, "." or ".." parts, no \ : * ? " < > | or control
// characters, no trailing dot or space, no device name.
func safeName(name string) bool {
	if name == "" || strings.HasPrefix(name, "/") {
		return false
	}
	for _, part := range strings.Split(name, "/") {
		if part == "" || part == "." || part == ".." || strings.TrimRight(part, ". ") != part {
			return false
		}
		if strings.ContainsAny(part, "\\:*?\"<>|") {
			return false
		}
		for _, r := range part {
			if r < 32 {
				return false
			}
		}
		base := strings.ToLower(strings.SplitN(part, ".", 2)[0])
		if reservedNames[base] {
			return false
		}
	}
	return true
}

func writeZipFile(zf *zip.File, dst string) error {
	r, err := zf.Open()
	if err != nil {
		return err
	}
	defer r.Close()
	w, err := os.OpenFile(dst, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0o755)
	if err != nil {
		return err
	}
	if _, err := io.Copy(w, io.LimitReader(r, maxPackageSize)); err != nil {
		w.Close()
		return err
	}
	return w.Close()
}

func copyFile(src, dst string, limit int64) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.OpenFile(dst, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0o644)
	if err != nil {
		return err
	}
	if _, err := io.Copy(out, io.LimitReader(in, limit)); err != nil {
		out.Close()
		return err
	}
	return out.Close()
}

// move renames, falling back to copy + remove across volumes.
func move(src, dst string) error {
	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return err
	}
	if err := os.Rename(src, dst); err == nil {
		return nil
	}
	if err := copyFile(src, dst, maxPackageSize); err != nil {
		return err
	}
	return os.Remove(src)
}

// retry gives a file another few seconds to be let go of (antivirus, a process
// on its way out).
func retry(f func() error) error {
	var err error
	for i := 0; i < 10; i++ {
		if err = f(); err == nil {
			return nil
		}
		time.Sleep(500 * time.Millisecond)
	}
	return err
}

func readApplyState(work string) (*applyState, error) {
	b, err := os.ReadFile(filepath.Join(work, applyFile))
	if err != nil {
		return nil, err
	}
	var st applyState
	if err := json.Unmarshal(b, &st); err != nil {
		return nil, err
	}
	return &st, nil
}

// lock keeps two applies (a double click, a retry) from interleaving. A lock
// older than ten minutes is from an apply that died and is taken over.
func lock(work string) (func(), error) {
	p := filepath.Join(work, lockFile)
	for i := 0; i < 2; i++ {
		f, err := os.OpenFile(p, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o644)
		if err == nil {
			f.Close()
			return func() { _ = os.Remove(p) }, nil
		}
		if st, serr := os.Stat(p); serr == nil && time.Since(st.ModTime()) > 10*time.Minute {
			_ = os.Remove(p)
			continue
		}
		break
	}
	return nil, errors.New("another apply is running")
}

func markFailure(dir string, code int) int {
	failures := readBad(dir)
	failures[strconv.Itoa(code)]++
	b, _ := json.Marshal(failures)
	_ = os.WriteFile(filepath.Join(dir, badFile), b, 0o644)
	return failures[strconv.Itoa(code)]
}

func startApp(o applyOpts) error {
	exe := filepath.Join(o.dir, o.exe)
	if o.deelevate {
		// explorer.exe starts it with the signed-in user's own, unelevated token.
		return exec.Command(filepath.Join(os.Getenv("WINDIR"), "explorer.exe"), exe).Start()
	}
	cmd := exec.Command(exe)
	cmd.Dir = o.dir
	return cmd.Start()
}

func waitExit(pid int, max time.Duration) {
	if pid <= 0 {
		return
	}
	p, err := os.FindProcess(pid)
	if err != nil {
		return
	}
	done := make(chan struct{})
	go func() { _, _ = p.Wait(); close(done) }()
	select {
	case <-done:
	case <-time.After(max):
	}
}

func exists(p string) bool {
	_, err := os.Stat(p)
	return err == nil
}

func contains(list []string, s string) bool {
	for _, v := range list {
		if v == s {
			return true
		}
	}
	return false
}
