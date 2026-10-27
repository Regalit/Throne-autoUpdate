package selfupdate

import (
	"archive/zip"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"
)

const (
	stagingDir  = "staging"
	previousDir = "previous"
	applyFile   = "apply.json" // what this apply replaced and added, for a rollback
)

type applyState struct {
	Code     int      `json:"code"`
	Replaced []string `json:"replaced"`
	Added    []string `json:"added"`
}

func runApply(args []string) error {
	fs := flag.NewFlagSet("apply", flag.ContinueOnError)
	dir := fs.String("dir", "", "app dir")
	work := fs.String("work", "", "work dir")
	exe := fs.String("exe", "", "exe to start, relative to the app dir")
	pid := fs.Int("pid", 0, "app process to wait for")
	timeout := fs.Duration("timeout", 90*time.Second, "how long the new build has to write ok")
	if err := fs.Parse(args); err != nil {
		return err
	}
	if *dir == "" || *work == "" || *exe == "" || filepath.Base(*exe) != *exe {
		return errors.New("apply needs -dir, -work and a plain -exe name")
	}
	waitExit(*pid, 60*time.Second)

	err := apply(*dir, *work, *exe, *timeout)
	if err != nil {
		logf(*work, "apply: %v", err)
		// Whatever happened, the user gets a working app back.
		_ = startApp(*dir, *exe)
	}
	return err
}

func apply(dir, work, exe string, timeout time.Duration) error {
	rel, err := readRelease(filepath.Join(work, readyFile))
	if err != nil {
		return fmt.Errorf("no ready package: %w", err)
	}
	if err := rel.validate(rel.AppID, publicKey); err != nil {
		return fmt.Errorf("refused: %w", err)
	}
	if ok, err := fileMatches(filepath.Join(work, packageFile), rel.Size, rel.SHA256); err != nil || !ok {
		clearPackage(work)
		return errors.New("refused: package does not match its sha256")
	}

	staging := filepath.Join(work, stagingDir)
	_ = os.RemoveAll(staging)
	files, err := extract(filepath.Join(work, packageFile), staging)
	if err != nil {
		clearPackage(work)
		return fmt.Errorf("refused: %w", err)
	}
	if !contains(files, exe) {
		clearPackage(work)
		return fmt.Errorf("refused: package has no %s", exe)
	}

	prev := filepath.Join(work, previousDir)
	_ = os.RemoveAll(prev)
	st := applyState{Code: rel.VersionCode}
	for _, f := range files {
		dst := filepath.Join(dir, f)
		if _, err := os.Stat(dst); err == nil {
			if err := move(dst, filepath.Join(prev, f)); err != nil {
				rollback(dir, work, &st)
				return fmt.Errorf("move out %s: %w", f, err)
			}
			st.Replaced = append(st.Replaced, f)
		} else {
			st.Added = append(st.Added, f)
		}
		if err := writeJSON(filepath.Join(work, applyFile), &st); err != nil {
			rollback(dir, work, &st)
			return err
		}
		if err := move(filepath.Join(staging, f), dst); err != nil {
			rollback(dir, work, &st)
			return fmt.Errorf("move in %s: %w", f, err)
		}
	}
	logf(work, "apply: %s in place (%d replaced, %d added), starting it", rel.Version, len(st.Replaced), len(st.Added))

	_ = os.Remove(filepath.Join(work, okFile))
	if err := startAndConfirm(dir, work, exe, timeout); err != nil {
		rollback(dir, work, &st)
		markBad(work, rel.VersionCode)
		clearPackage(work)
		_ = os.RemoveAll(staging)
		_ = os.RemoveAll(prev)
		return fmt.Errorf("%s did not start (%v); rolled back and skipped", rel.Version, err)
	}
	logf(work, "apply: %s started", rel.Version)
	for _, p := range []string{prev, staging, filepath.Join(work, applyFile), filepath.Join(work, okFile)} {
		_ = os.RemoveAll(p)
	}
	clearPackage(work)
	return nil
}

// startAndConfirm starts the new build and waits for it to write the ok file
// once its core is up. Exiting first, or the timeout, counts as a failure, and
// the new build is stopped.
func startAndConfirm(dir, work, exe string, timeout time.Duration) error {
	cmd := exec.Command(filepath.Join(dir, exe), "--after-update")
	cmd.Dir = dir
	if err := cmd.Start(); err != nil {
		return err
	}
	exited := make(chan error, 1)
	go func() { exited <- cmd.Wait() }()
	deadline := time.After(timeout)
	tick := time.NewTicker(500 * time.Millisecond)
	defer tick.Stop()
	for {
		select {
		case err := <-exited:
			if _, statErr := os.Stat(filepath.Join(work, okFile)); statErr == nil {
				return nil // it was up, then the user closed it
			}
			return fmt.Errorf("exited before it was up: %v", err)
		case <-deadline:
			_ = cmd.Process.Kill()
			return errors.New("not up in time")
		case <-tick.C:
			if _, err := os.Stat(filepath.Join(work, okFile)); err == nil {
				return nil
			}
		}
	}
}

// rollback undoes an apply: new files out, the previous ones back.
func rollback(dir, work string, st *applyState) {
	for _, f := range st.Added {
		_ = os.Remove(filepath.Join(dir, f))
	}
	for _, f := range st.Replaced {
		dst := filepath.Join(dir, f)
		_ = os.Remove(dst)
		if err := move(filepath.Join(work, previousDir, f), dst); err != nil {
			logf(work, "rollback: %s: %v", f, err)
		}
	}
	_ = os.Remove(filepath.Join(work, applyFile))
	logf(work, "rollback: done (%d restored, %d removed)", len(st.Replaced), len(st.Added))
}

// extract unpacks the package into staging and returns its files (slash
// paths). It refuses paths that leave the dir, and anything under config/ or
// update/, which belong to the user and the updater.
func extract(pkg, staging string) ([]string, error) {
	zr, err := zip.OpenReader(pkg)
	if err != nil {
		return nil, err
	}
	defer zr.Close()
	var files []string
	for _, zf := range zr.File {
		name := filepath.ToSlash(zf.Name)
		if zf.FileInfo().IsDir() {
			continue
		}
		clean := filepath.ToSlash(filepath.Clean(name))
		top := strings.ToLower(strings.SplitN(clean, "/", 2)[0])
		if clean != name || filepath.IsAbs(clean) || strings.HasPrefix(clean, "../") || clean == ".." ||
			strings.Contains(clean, ":") || top == "config" || top == "update" {
			return nil, fmt.Errorf("bad path %q in package", zf.Name)
		}
		dst := filepath.Join(staging, filepath.FromSlash(clean))
		if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
			return nil, err
		}
		if err := writeZipFile(zf, dst); err != nil {
			return nil, err
		}
		files = append(files, clean)
	}
	if len(files) == 0 {
		return nil, errors.New("empty package")
	}
	return files, nil
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

// move renames, falling back to copy + remove across volumes.
func move(src, dst string) error {
	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return err
	}
	if err := os.Rename(src, dst); err == nil {
		return nil
	}
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	out, err := os.OpenFile(dst, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0o755)
	if err != nil {
		in.Close()
		return err
	}
	_, err = io.Copy(out, in)
	in.Close()
	if cerr := out.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		return err
	}
	return os.Remove(src)
}

func markBad(work string, code int) {
	codes := append(readBad(work), code)
	b, _ := json.Marshal(codes)
	_ = os.WriteFile(filepath.Join(work, badFile), b, 0o644)
}

func startApp(dir, exe string) error {
	cmd := exec.Command(filepath.Join(dir, exe))
	cmd.Dir = dir
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
	time.Sleep(500 * time.Millisecond) // let Windows release the exe's file lock
}

func contains(list []string, s string) bool {
	for _, v := range list {
		if v == s {
			return true
		}
	}
	return false
}
