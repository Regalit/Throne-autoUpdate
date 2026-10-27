package probe

import (
	"context"
	"errors"
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
			duration, err := measureURLTest(ctx, client, testTargets(url), firstRequestTimeout(i, tag, twice, timeout), timeout, twice)
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

// Shadowlos: sites asked next to the configured test URL. Any one of them answering through the
// server proves the handshake and real traffic, so one blocked, slow or flaky site can no longer
// mark a working server down (cp.cloudflare.com over plain HTTP, telegram.org from a Russian exit).
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

// measureURLTest is one server's verdict. A server is down only when no target answers through
// it, on the first try and on one retry. With twice, a second, warm round gives the latency;
// if only that round fails the server still answered a moment ago, so the first round's
// (cold, slower) time stands instead of an error.
func measureURLTest(ctx context.Context, client *http.Client, targets []string, firstTimeout, timeout time.Duration, twice bool) (time.Duration, error) {
	duration, err := raceURLTest(ctx, client, targets, firstTimeout)
	if err != nil && ctx.Err() == nil {
		duration, err = raceURLTest(ctx, client, targets, timeout)
	}
	if err == nil && twice {
		if warm, warmErr := raceURLTest(ctx, client, targets, timeout); warmErr == nil {
			duration = warm
		}
	}
	return duration, err
}

// raceURLTest asks every target at once through the same client. The configured URL (urls[0])
// gives the time when it answers; when it fails, the first other target that answered gives it.
// It fails only when every target fails, with the configured URL's error.
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
		primaryErr error
		fallback   time.Duration
		fallbackOK bool
	)
	for range urls {
		r := <-results
		switch {
		case r.idx == 0 && r.err == nil:
			return r.duration, nil
		case r.idx == 0:
			primaryErr = r.err
		case r.err == nil && !fallbackOK:
			fallback, fallbackOK = r.duration, true
		}
		if primaryErr != nil && fallbackOK {
			return fallback, nil
		}
	}
	if primaryErr == nil {
		return 0, errors.New("url test: no result")
	}
	return 0, primaryErr
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
