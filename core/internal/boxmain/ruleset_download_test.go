package boxmain

import (
	"bytes"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"syscall"
	"testing"
	"time"

	"github.com/sagernet/sing-box/common/srs"
	C "github.com/sagernet/sing-box/constant"
	"github.com/sagernet/sing-box/option"
)

// A remote rule set that is not cached is downloaded while the core starts, and a
// failure aborts the start. These pin how that download picks its route, using the
// same Create the GUI's config goes through. The default route is a dead proxy,
// standing in for an exit node that refuses the (Russian) mirror host.
func rulesetConfig(t *testing.T, url string, ruleSetExtra map[string]any) []byte {
	t.Helper()
	rs := map[string]any{"type": "remote", "tag": "x", "format": "binary", "url": url}
	for k, v := range ruleSetExtra {
		rs[k] = v
	}
	cfg := map[string]any{
		"log": map[string]any{"level": "error"},
		"dns": map[string]any{"servers": []any{map[string]any{"type": "local", "tag": "dns-direct"}}},
		"outbounds": []any{
			map[string]any{"type": "direct", "tag": "direct"},
			map[string]any{"type": "socks", "tag": "proxy", "server": "127.0.0.1", "server_port": 1},
		},
		"route": map[string]any{
			"rule_set":                []any{rs},
			"rules":                   []any{map[string]any{"rule_set": []string{"x"}, "outbound": "direct"}},
			"final":                   "proxy",
			"default_domain_resolver": map[string]any{"server": "dns-direct"},
		},
	}
	raw, err := json.Marshal(cfg)
	if err != nil {
		t.Fatal(err)
	}
	return raw
}

func ruleSetBytes(t *testing.T) []byte {
	t.Helper()
	var buf bytes.Buffer
	plain := option.PlainRuleSet{Rules: []option.HeadlessRule{{
		Type:           C.RuleTypeDefault,
		DefaultOptions: option.DefaultHeadlessRule{Domain: []string{"example.invalid"}},
	}}}
	if err := srs.Write(&buf, plain, C.RuleSetVersion1); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}

func serveRuleSet(t *testing.T) string {
	t.Helper()
	body := ruleSetBytes(t)
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write(body)
	}))
	t.Cleanup(srv.Close)
	return srv.URL + "/x.srs"
}

func TestRemoteRuleSetDownload(t *testing.T) {
	url := serveRuleSet(t)

	cases := []struct {
		name    string
		extra   map[string]any
		wantErr string // empty: must start
	}{
		// Why the fix exists: the default route is the proxy, which cannot reach the host.
		{"default route goes through the proxy", nil, "connection refused"},
		// What 1.3.1 shipped: sing-box refuses it for a plain direct outbound.
		{"http_client detour direct is rejected", map[string]any{"http_client": map[string]any{"detour": "direct"}}, "empty direct outbound"},
		// What generate.cpp must emit.
		{"download_detour direct downloads over direct", map[string]any{"download_detour": "direct"}, ""},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Chdir(t.TempDir()) // a fresh cache.db, so nothing is cached
			inst, cancel, err := Create(rulesetConfig(t, url, tc.extra), nil)
			if tc.wantErr == "" {
				if err != nil {
					t.Fatalf("core did not start: %v", err)
				}
				_ = inst.Close()
				cancel()
				return
			}
			if err == nil {
				_ = inst.Close()
				cancel()
				t.Fatalf("core started, want an error containing %q", tc.wantErr)
			}
			if !strings.Contains(err.Error(), tc.wantErr) && !windowsRefused(err, tc.wantErr) {
				t.Fatalf("error = %v, want it to contain %q", err, tc.wantErr)
			}
		})
	}
}

// Windows words a refused connect differently: WSAECONNREFUSED (10061), "actively refused it".
func windowsRefused(err error, want string) bool {
	if want != "connection refused" {
		return false
	}
	return errors.Is(err, syscall.Errno(10061)) || strings.Contains(err.Error(), "actively refused")
}

// A rule set already in cache.db must not be downloaded again at start: with the mirror gone (or no DNS
// answering) the core still starts on the cached copy at once.
func TestCachedRuleSetStartsOffline(t *testing.T) {
	var body []byte
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write(body)
	}))
	body = ruleSetBytes(t)
	url := srv.URL + "/x.srs"
	t.Chdir(t.TempDir())

	var cfg map[string]any
	if err := json.Unmarshal(rulesetConfig(t, url, map[string]any{"download_detour": "direct"}), &cfg); err != nil {
		t.Fatal(err)
	}
	cfg["experimental"] = map[string]any{"cache_file": map[string]any{"enabled": true}}
	config, err := json.Marshal(cfg)
	if err != nil {
		t.Fatal(err)
	}

	inst, cancel, err := Create(config, nil)
	if err != nil {
		srv.Close()
		t.Fatalf("first start (download): %v", err)
	}
	inst.CloseWithTimeout(cancel, 2*time.Second, func(...any) {}, true)

	// Same URL (the cache is keyed by it), but nothing answers there any more.
	srv.Close()
	start := time.Now()
	inst, cancel, err = Create(config, nil)
	if err != nil {
		t.Fatalf("start on the cached rule set failed: %v", err)
	}
	_ = inst.Close()
	cancel()
	if took := time.Since(start); took > 2*time.Second {
		t.Fatalf("start on the cached rule set took %v", took)
	}
}
