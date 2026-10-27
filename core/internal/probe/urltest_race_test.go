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
