package probe

import (
	"context"
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
			// Shadowlos: one slow or unlucky first request no longer marks a working server down.
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

// Shadowlos: besides the configured URL, two more targets answer the same probe, so one
// endpoint being slow or filtered does not mark a working server down.
var fallbackTestURLs = []string{
	"https://www.gstatic.com/generate_204",
	"https://www.youtube.com/generate_204",
}

func testTargets(url string) []string {
	targets := []string{url}
	for _, u := range fallbackTestURLs {
		if u != url {
			targets = append(targets, u)
		}
	}
	return targets
}

// raceURLTest asks every target at once through the same client and reports the first answer;
// it fails only when all of them do, with the configured URL's error.
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
	var firstErr error
	for range urls {
		r := <-results
		if r.err == nil {
			return r.duration, nil
		}
		if r.idx == 0 || firstErr == nil {
			firstErr = r.err
		}
	}
	return 0, firstErr
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
