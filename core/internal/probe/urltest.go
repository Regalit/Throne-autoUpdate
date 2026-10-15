package probe

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	"time"

	"github.com/sagernet/sing-box/adapter"
)

var URLReporter resultBuffer[URLTestResult]

const URLTestTimeout = 3 * time.Second

type URLTestResult struct {
	Duration time.Duration
	Tag      string
	Error    error
}

func BatchURLTest(ctx context.Context, i Box, outboundTags []string, url string, maxConcurrency int, twice bool, timeout time.Duration) []*URLTestResult {
	if timeout <= 0 {
		timeout = URLTestTimeout
	}

	results := runBatch(ctx, i, outboundTags, maxConcurrency, batchProbe[URLTestResult]{
		run: func(ctx context.Context, tag string, outbound adapter.Outbound) *URLTestResult {
			if err := awaitTunnels(ctx, i, tag); err != nil {
				return &URLTestResult{Tag: tag, Error: err}
			}
			client, closeClient := outboundHTTPClient(ctx, outbound)
			defer closeClient()
			// Shadowlos: one slow or unlucky first request no longer marks a working server down, and a
			// server only counts as working when more than the test URL gets through it.
			targets := testTargets(url)
			duration, err := raceURLTest(ctx, client, targets, firstRequestTimeout(i, tag, twice, timeout))
			if err != nil && ctx.Err() == nil {
				duration, err = raceURLTest(ctx, client, targets, timeout)
			}
			if err == nil && twice {
				duration, err = raceURLTest(ctx, client, targets, timeout)
			}
			return &URLTestResult{Duration: duration, Tag: tag, Error: err}
		},
		fail: func(tag string, err error) *URLTestResult {
			return &URLTestResult{Tag: tag, Error: err}
		},
		publish: URLReporter.AddResult,
	})
	URLReporter.Reclaim(results)
	return results
}

// Shadowlos: a second site has to answer through the server as well, so a server whose exit
// reaches the test URL and nothing else does not read as working and get auto-picked.
var crossCheckURLs = []string{
	"https://www.gstatic.com/generate_204",
	"https://telegram.org/",
}

func testTargets(url string) []string {
	targets := []string{url}
	for _, u := range crossCheckURLs {
		if u != url {
			targets = append(targets, u)
		}
	}
	return targets
}

// raceURLTest asks every target at once through the same client. It succeeds with the
// configured URL's time once that URL and at least one other target have answered, and fails
// as soon as either can no longer happen, with the configured URL's error when it has one.
func raceURLTest(ctx context.Context, client *http.Client, urls []string, timeout time.Duration) (time.Duration, error) {
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()
	type result struct {
		idx      int
		duration time.Duration
		err      error
	}
	results := make(chan result, len(urls))
	for idx, u := range urls {
		go func() {
			d, err := urlTest(ctx, client, u, timeout)
			results <- result{idx, d, err}
		}()
	}
	var (
		primary      time.Duration
		primaryOK    bool
		others       int
		otherFailed  int
		lastOtherErr error
	)
	for range urls {
		r := <-results
		switch {
		case r.idx == 0 && r.err != nil:
			return 0, r.err
		case r.idx == 0:
			primary, primaryOK = r.duration, true
		case r.err != nil:
			otherFailed++
			lastOtherErr = r.err
		default:
			others++
		}
		if primaryOK && (others > 0 || len(urls) == 1) {
			return primary, nil
		}
		if otherFailed == len(urls)-1 && len(urls) > 1 {
			return 0, fmt.Errorf("only the test URL answers through this server: %w", lastOtherErr)
		}
	}
	return 0, errors.New("url test: no result")
}

func urlTest(ctx context.Context, client *http.Client, url string, timeout time.Duration) (time.Duration, error) {
	ctx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	begin := time.Now()
	req, err := http.NewRequestWithContext(ctx, "GET", url, nil)
	if err != nil {
		return 0, err
	}
	resp, err := client.Do(req)
	if err != nil {
		return 0, err
	}
	_ = resp.Body.Close()
	return time.Since(begin), nil
}
