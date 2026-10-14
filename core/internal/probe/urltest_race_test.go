package probe

import (
	"context"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

func TestRaceURLTestNeedsASecondSite(t *testing.T) {
	ok := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { w.WriteHeader(204) }))
	defer ok.Close()
	client := ok.Client()
	dead := "http://127.0.0.1:1/"

	if _, err := raceURLTest(context.Background(), client, []string{ok.URL, ok.URL + "/a"}, time.Second); err != nil {
		t.Fatalf("both answer: %v", err)
	}
	if _, err := raceURLTest(context.Background(), client, []string{ok.URL, dead, dead}, time.Second); err == nil {
		t.Fatal("only the test URL answers: want an error")
	}
	if _, err := raceURLTest(context.Background(), client, []string{dead, ok.URL}, time.Second); err == nil {
		t.Fatal("test URL down: want an error")
	}
	if _, err := raceURLTest(context.Background(), client, []string{ok.URL, dead, ok.URL + "/b"}, time.Second); err != nil {
		t.Fatalf("test URL and one other answer: %v", err)
	}
}
