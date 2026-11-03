package boxmain

import (
	"path/filepath"
	"syscall"
	"testing"
	"time"
)

const cacheFileConfig = `{
  "log": {"disabled": true},
  "outbounds": [{"type": "direct", "tag": "direct"}],
  "experimental": {"cache_file": {"enabled": true}}
}`

// holdExclusive opens path with no sharing, as a scanner grabbing a just-released cache.db does.
func holdExclusive(t *testing.T, path string) syscall.Handle {
	t.Helper()
	name, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		t.Fatal(err)
	}
	h, err := syscall.CreateFile(name, syscall.GENERIC_READ|syscall.GENERIC_WRITE, 0, nil,
		syscall.OPEN_ALWAYS, syscall.FILE_ATTRIBUTE_NORMAL, 0)
	if err != nil {
		t.Fatal(err)
	}
	return h
}

// The journal's server switch: the new box's cache-file open hits a sharing violation for a moment.
// The start must wait it out instead of failing.
func TestCreateWaitsForBusyCacheFile(t *testing.T) {
	dir := t.TempDir()
	t.Chdir(dir)
	h := holdExclusive(t, filepath.Join(dir, "cache.db"))
	released := make(chan struct{})
	go func() {
		time.Sleep(400 * time.Millisecond)
		_ = syscall.CloseHandle(h)
		close(released)
	}()
	start := time.Now()
	inst, cancel, err := Create([]byte(cacheFileConfig), nil)
	<-released
	if err != nil {
		t.Fatalf("start failed although cache.db was released after 400 ms: %v", err)
	}
	_ = inst.Close()
	cancel()
	if took := time.Since(start); took < 300*time.Millisecond {
		t.Fatalf("started in %v while cache.db was held: the test did not hold it", took)
	}
}

// Held for longer than the wait: the start still fails, with the error the GUI explains.
func TestCreateGivesUpOnHeldCacheFile(t *testing.T) {
	dir := t.TempDir()
	t.Chdir(dir)
	h := holdExclusive(t, filepath.Join(dir, "cache.db"))
	defer syscall.CloseHandle(h)
	start := time.Now()
	inst, cancel, err := Create([]byte(cacheFileConfig), nil)
	if err == nil {
		_ = inst.Close()
		cancel()
		t.Fatal("started although cache.db was held throughout")
	}
	if !IsCacheFileBusy(err) {
		t.Fatalf("error = %v, want the cache-file sharing violation", err)
	}
	if took := time.Since(start); took > cacheFileBusyWait+2*time.Second {
		t.Fatalf("gave up after %v, the wait is bounded by %v", took, cacheFileBusyWait)
	}
}

// After a box is closed (what Stop waits for), cache.db is free: nothing of the old box still holds it.
func TestCacheFileReleasedOnClose(t *testing.T) {
	dir := t.TempDir()
	t.Chdir(dir)
	first, cancel, err := Create([]byte(cacheFileConfig), nil)
	if err != nil {
		t.Fatal(err)
	}
	first.CloseWithTimeout(cancel, 2*time.Second, func(...any) {}, true)
	name, _ := syscall.UTF16PtrFromString(filepath.Join(dir, "cache.db"))
	h, err := syscall.CreateFile(name, syscall.GENERIC_READ|syscall.GENERIC_WRITE, 0, nil,
		syscall.OPEN_EXISTING, syscall.FILE_ATTRIBUTE_NORMAL, 0)
	if err != nil {
		t.Fatalf("cache.db still held after Close returned: %v", err)
	}
	_ = syscall.CloseHandle(h)
}
