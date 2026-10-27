package selfupdate

import (
	"bufio"
	"bytes"
	"encoding/json"
	"net"
	"net/http"
	"strings"
	"testing"
	"time"
)

// What the GUI reads: progress lines, then the result line, each one JSON object.
func TestCheckPrintsProgressThenResult(t *testing.T) {
	useTestKey(t)
	pkg := makeZip(t, map[string][]byte{"App.exe": bytes.Repeat([]byte("x"), 1<<20)})
	f := &fakeServer{pkg: pkg, rel: release(pkg, 10001, testPriv)}
	base, work := f.start(t), t.TempDir()

	var out bytes.Buffer
	p := &progressPrinter{out: &out, every: 0, now: time.Now}
	res := checkWithProgress(http.DefaultClient, base, "shadowlos-throne-dev", 10000, work, p.report)
	json.NewEncoder(&out).Encode(res)

	var lines []map[string]any
	sc := bufio.NewScanner(&out)
	for sc.Scan() {
		var m map[string]any
		if err := json.Unmarshal(sc.Bytes(), &m); err != nil {
			t.Fatalf("not a JSON line: %q", sc.Text())
		}
		lines = append(lines, m)
	}
	if len(lines) < 3 {
		t.Fatalf("want progress lines and a result, got %v", lines)
	}
	last := lines[len(lines)-1]
	if last["state"] != "ready" || last["version"] != "1.0.1-dev" || last["code"] != float64(10001) {
		t.Fatalf("result line %v", last)
	}
	prev := -1.0
	for _, m := range lines[:len(lines)-1] {
		done, size := m["done"].(float64), m["size"].(float64)
		if m["state"] != "downloading" || m["version"] != "1.0.1-dev" || size != float64(len(pkg)) ||
			done < prev || done > size {
			t.Fatalf("progress line %v after done=%v", m, prev)
		}
		prev = done
	}
	if lines[0]["done"] != float64(0) || prev != float64(len(pkg)) {
		t.Fatalf("progress runs %v..%v, want 0..%d", lines[0]["done"], prev, len(pkg))
	}

	// A package already downloaded and verified prints the result alone.
	out.Reset()
	res = checkWithProgress(http.DefaultClient, base, "shadowlos-throne-dev", 10000, work, p.report)
	if res.State != "ready" || out.Len() != 0 {
		t.Fatalf("got %+v and output %q", res, out.String())
	}
}

func TestProgressPrinterThrottles(t *testing.T) {
	var out bytes.Buffer
	clock := time.Unix(0, 0)
	p := &progressPrinter{out: &out, every: 250 * time.Millisecond, now: func() time.Time { return clock }}
	for done := int64(0); done <= 1000; done += 10 {
		p.report("2.0.0", done, 1000)
		clock = clock.Add(10 * time.Millisecond) // 1 s in all
	}
	var dones []int64
	for _, line := range strings.Split(strings.TrimSpace(out.String()), "\n") {
		var l progressLine
		if err := json.Unmarshal([]byte(line), &l); err != nil || l.State != "downloading" || l.Version != "2.0.0" {
			t.Fatalf("line %q", line)
		}
		dones = append(dones, l.Done)
	}
	// The first, one per 250 ms after it, and the last always.
	want := []int64{0, 250, 500, 750, 1000}
	if len(dones) != len(want) {
		t.Fatalf("printed %v, want %v", dones, want)
	}
	for i := range want {
		if dones[i] != want[i] {
			t.Fatalf("printed %v, want %v", dones, want)
		}
	}
}

// The GUI words an error by its kind.
func TestCheckErrorKinds(t *testing.T) {
	useTestKey(t)
	pkg := makeZip(t, map[string][]byte{"App.exe": []byte("new")})

	t.Run("channel unreachable", func(t *testing.T) {
		ln, err := net.Listen("tcp", "127.0.0.1:0")
		if err != nil {
			t.Fatal(err)
		}
		addr := ln.Addr().String()
		ln.Close()
		res := check(http.DefaultClient, "http://"+addr+"/api/desktop/app/dev", "shadowlos-throne-dev", 10000, t.TempDir())
		if res.State != "error" || res.Kind != kindNetwork {
			t.Fatalf("got %+v", res)
		}
	})
	t.Run("bad signature", func(t *testing.T) {
		rel := release(pkg, 10001, testPriv)
		rel.VersionCode = 10099
		f := &fakeServer{pkg: pkg, rel: rel}
		res := check(http.DefaultClient, f.start(t), "shadowlos-throne-dev", 10000, t.TempDir())
		if res.State != "error" || res.Kind != kindVerify {
			t.Fatalf("got %+v", res)
		}
	})
	t.Run("file differs from the signed sha256", func(t *testing.T) {
		// Same size, one byte flipped: the announcement and its signature are fine.
		evil := bytes.Clone(pkg)
		evil[len(evil)-1] ^= 0xff
		f := &fakeServer{pkg: evil, rel: release(pkg, 10001, testPriv)}
		res := check(http.DefaultClient, f.start(t), "shadowlos-throne-dev", 10000, t.TempDir())
		if res.State != "error" || res.Kind != kindVerify {
			t.Fatalf("got %+v", res)
		}
	})
	t.Run("download cut off", func(t *testing.T) {
		big := makeZip(t, map[string][]byte{"App.exe": bytes.Repeat([]byte("x"), 200000)})
		f := &fakeServer{pkg: big, rel: release(big, 10001, testPriv), cutAfter: 100}
		res := check(http.DefaultClient, f.start(t), "shadowlos-throne-dev", 10000, t.TempDir())
		if res.State != "error" || res.Kind != kindNetwork {
			t.Fatalf("got %+v", res)
		}
	})
	t.Run("channel answers 500", func(t *testing.T) {
		srv := http.Server{}
		ln, _ := net.Listen("tcp", "127.0.0.1:0")
		srv.Handler = http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { http.Error(w, "boom", 500) })
		go srv.Serve(ln)
		t.Cleanup(func() { srv.Close() })
		res := check(http.DefaultClient, "http://"+ln.Addr().String()+"/api/desktop/app/dev", "shadowlos-throne-dev", 10000, t.TempDir())
		if res.State != "error" || res.Kind != kindServer {
			t.Fatalf("got %+v", res)
		}
	})
}
