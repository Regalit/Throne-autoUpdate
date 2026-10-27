package probe

import (
	"context"
	"net/http"
	"net/http/httptest"
	"sync/atomic"
	"testing"
	"time"
)

const deadURL = "http://127.0.0.1:1/"

// A server is working as soon as any target answers through it; only all targets failing is an error.
func TestRaceURLTestAnyTargetProvesTheServer(t *testing.T) {
	ok := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { w.WriteHeader(204) }))
	defer ok.Close()
	client := ok.Client()

	cases := []struct {
		name    string
		urls    []string
		wantErr bool
	}{
		{"all answer", []string{ok.URL, ok.URL + "/a", ok.URL + "/b"}, false},
		{"only the test URL answers", []string{ok.URL, deadURL, deadURL}, false},
		{"test URL down, a cross-check site answers", []string{deadURL, ok.URL}, false},
		{"test URL down, the last site answers", []string{deadURL, deadURL, ok.URL}, false},
		{"no target answers", []string{deadURL, deadURL, deadURL}, true},
		{"single dead test URL", []string{deadURL}, true},
	}
	for _, c := range cases {
		_, err := raceURLTest(context.Background(), client, c.urls, time.Second)
		if (err != nil) != c.wantErr {
			t.Errorf("%s: err = %v, wantErr %v", c.name, err, c.wantErr)
		}
	}
}

// The configured URL's time is reported whenever it answers, even when another target was faster.
func TestRaceURLTestPrefersTheTestURLTime(t *testing.T) {
	slow := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		time.Sleep(150 * time.Millisecond)
		w.WriteHeader(204)
	}))
	defer slow.Close()
	fast := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { w.WriteHeader(204) }))
	defer fast.Close()

	d, err := raceURLTest(context.Background(), http.DefaultClient, []string{slow.URL, fast.URL}, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if d < 150*time.Millisecond {
		t.Fatalf("got the cross-check time %v, want the test URL's (>= 150ms)", d)
	}
}

// With twice, a failing warm round keeps the first round's result rather than marking the server down.
func TestMeasureURLTestKeepsAWorkingFirstRound(t *testing.T) {
	var hits atomic.Int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if hits.Add(1) > 1 {
			// Every request after the first one dies: the warm round fails.
			hj, _ := w.(http.Hijacker)
			conn, _, _ := hj.Hijack()
			_ = conn.Close()
			return
		}
		w.WriteHeader(204)
	}))
	defer srv.Close()
	client := &http.Client{Transport: &http.Transport{DisableKeepAlives: true}}

	d, err := measureURLTest(context.Background(), client, []string{srv.URL}, time.Second, time.Second, true)
	if err != nil {
		t.Fatalf("first round answered, warm round failed: want success, got %v", err)
	}
	if d <= 0 {
		t.Fatalf("want the first round's time, got %v", d)
	}
}

// A first round that fails is retried once before the server is called down.
func TestMeasureURLTestRetriesOnce(t *testing.T) {
	var hits atomic.Int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if hits.Add(1) == 1 {
			hj, _ := w.(http.Hijacker)
			conn, _, _ := hj.Hijack()
			_ = conn.Close()
			return
		}
		w.WriteHeader(204)
	}))
	defer srv.Close()
	client := &http.Client{Transport: &http.Transport{DisableKeepAlives: true}}

	if _, err := measureURLTest(context.Background(), client, []string{srv.URL}, time.Second, time.Second, false); err != nil {
		t.Fatalf("second try answered: want success, got %v", err)
	}
	if _, err := measureURLTest(context.Background(), client, []string{deadURL}, time.Second, time.Second, true); err == nil {
		t.Fatal("nothing answers: want an error")
	}
}

const deadTLSURL = "https://127.0.0.1:1/"

func statusServer(code int) *httptest.Server {
	return httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { w.WriteHeader(code) }))
}

// A 4xx/5xx over plain HTTP can come from the proxy server itself or a block page: not alive on its own.
func TestRaceURLTestPlainHTTPErrorStatusIsNotAlive(t *testing.T) {
	for _, code := range []int{400, 403, 503} {
		primary := statusServer(code)
		_, err := raceURLTest(context.Background(), primary.Client(), []string{primary.URL, deadTLSURL, deadTLSURL}, time.Second)
		primary.Close()
		if err == nil {
			t.Errorf("plain HTTP %d with both HTTPS targets dead: want an error", code)
		}
	}
	// A plain-HTTP cross-check target is held to the same rule.
	bad := statusServer(502)
	defer bad.Close()
	if _, err := raceURLTest(context.Background(), bad.Client(), []string{deadURL, bad.URL}, time.Second); err == nil {
		t.Error("only a plain HTTP 502 answered: want an error")
	}
}

// Over HTTPS any status proves the tunnel (the TLS handshake with the real host already did), so a
// bad primary status is covered by an HTTPS target that answers, even with a 404.
func TestRaceURLTestHTTPSAnswerCoversABadPrimaryStatus(t *testing.T) {
	primary := statusServer(500)
	defer primary.Close()
	tlsSite := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { w.WriteHeader(404) }))
	defer tlsSite.Close()

	if _, err := raceURLTest(context.Background(), tlsSite.Client(), []string{primary.URL, tlsSite.URL, deadTLSURL}, time.Second); err != nil {
		t.Fatalf("plain HTTP 500 but an HTTPS target answered: want alive, got %v", err)
	}
}

// A primary that hangs until its timeout does not sink a server whose fallback answered; the
// fallback's own time is reported, not the primary's timeout.
func TestRaceURLTestHangingPrimaryFallsBack(t *testing.T) {
	hang := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { <-r.Context().Done() }))
	defer hang.Close()
	tlsSite := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { w.WriteHeader(204) }))
	defer tlsSite.Close()

	const timeout = 300 * time.Millisecond
	begin := time.Now()
	d, err := raceURLTest(context.Background(), tlsSite.Client(), []string{hang.URL, tlsSite.URL}, timeout)
	if err != nil {
		t.Fatalf("fallback answered: want alive, got %v", err)
	}
	if d >= timeout {
		t.Fatalf("got %v, want the fallback's time, below the %v timeout", d, timeout)
	}
	if waited := time.Since(begin); waited < timeout {
		t.Fatalf("returned after %v: the primary's time is preferred, so it should be waited for", waited)
	}
}

// With twice, the warm round's time replaces the cold first round's.
func TestMeasureURLTestWarmRoundReplacesColdTime(t *testing.T) {
	var hits atomic.Int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if hits.Add(1) == 1 {
			time.Sleep(200 * time.Millisecond) // cold: handshake and first dial
		}
		w.WriteHeader(204)
	}))
	defer srv.Close()

	d, err := measureURLTest(context.Background(), srv.Client(), []string{srv.URL}, time.Second, time.Second, true)
	if err != nil {
		t.Fatal(err)
	}
	if d >= 200*time.Millisecond {
		t.Fatalf("got %v: want the warm round's time, not the cold one", d)
	}
}
