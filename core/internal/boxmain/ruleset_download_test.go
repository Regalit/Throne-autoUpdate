package boxmain

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

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

func serveRuleSet(t *testing.T) string {
	t.Helper()
	var buf bytes.Buffer
	plain := option.PlainRuleSet{Rules: []option.HeadlessRule{{
		Type:           C.RuleTypeDefault,
		DefaultOptions: option.DefaultHeadlessRule{Domain: []string{"example.invalid"}},
	}}}
	if err := srs.Write(&buf, plain, C.RuleSetVersion1); err != nil {
		t.Fatal(err)
	}
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write(buf.Bytes())
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
			if !strings.Contains(err.Error(), tc.wantErr) {
				t.Fatalf("error = %v, want it to contain %q", err, tc.wantErr)
			}
		})
	}
}
