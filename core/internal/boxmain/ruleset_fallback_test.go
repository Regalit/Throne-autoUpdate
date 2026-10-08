package boxmain

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func decode(t *testing.T, raw []byte) map[string]any {
	t.Helper()
	var m map[string]any
	if err := json.Unmarshal(raw, &m); err != nil {
		t.Fatal(err)
	}
	return m
}

func tagsOf(sets any) []string {
	var out []string
	for _, s := range sets.([]any) {
		out = append(out, s.(map[string]any)["tag"].(string))
	}
	return out
}

const fallbackFixture = `{
  "route": {
    "rule_set": [
      {"type":"remote","tag":"a","format":"binary","url":"https://x/a.srs"},
      {"type":"remote","tag":"b","format":"binary","url":"https://x/b.srs"},
      {"type":"inline","tag":"c","rules":[{"ip_cidr":["10.0.0.0/8"]}]}
    ],
    "rules": [
      {"rule_set":["a"],"outbound":"direct"},
      {"rule_set":"b","outbound":"direct"},
      {"type":"logical","mode":"and","rules":[{"domain_suffix":["z"]},{"rule_set":["b"]}],"outbound":"direct"},
      {"domain_suffix":["y"],"outbound":"direct"},
      {"rule_set":["c"],"outbound":"direct"}
    ]
  },
  "dns": {"rules":[{"rule_set":["b"],"server":"s"},{"domain_suffix":["q"],"server":"s"}]},
  "inbounds": [{"type":"tun","route_exclude_address_set":["b","c"]}]
}`

func TestDropOnlyTheFailedRuleSets(t *testing.T) {
	out, dropped := dropRuleSets([]byte(fallbackFixture), map[string]bool{"b": true})
	if strings.Join(dropped, ",") != "b" {
		t.Fatalf("dropped = %v, want [b]", dropped)
	}
	m := decode(t, out)
	route := m["route"].(map[string]any)
	if got := strings.Join(tagsOf(route["rule_set"]), ","); got != "a,c" {
		t.Errorf("rule sets = %s, want a,c (inline c must survive, remote a must stay)", got)
	}
	rules := route["rules"].([]any)
	if len(rules) != 3 {
		t.Fatalf("route rules = %d, want 3: the a rule, the plain domain rule and the c rule: %s", len(rules), out)
	}
	for _, r := range rules {
		rs, has := r.(map[string]any)["rule_set"]
		if has && len(rs.([]any)) == 0 {
			t.Errorf("a rule was left with an empty rule_set, which would match everything: %v", r)
		}
	}
	if dns := m["dns"].(map[string]any)["rules"].([]any); len(dns) != 1 {
		t.Errorf("dns rules = %d, want only the one that never used b", len(dns))
	}
	exclude := m["inbounds"].([]any)[0].(map[string]any)["route_exclude_address_set"].([]any)
	if len(exclude) != 1 || exclude[0] != "c" {
		t.Errorf("tun exclude set = %v, want [c]", exclude)
	}
}

func TestDropAllRemoteRuleSetsWhenTheErrorNamesNone(t *testing.T) {
	out, dropped := dropRuleSets([]byte(fallbackFixture), nil)
	if strings.Join(dropped, ",") != "a,b" {
		t.Fatalf("dropped = %v, want [a b]", dropped)
	}
	route := decode(t, out)["route"].(map[string]any)
	if got := strings.Join(tagsOf(route["rule_set"]), ","); got != "c" {
		t.Errorf("rule sets = %s, want only the inline c", got)
	}
}

func TestDropRuleSetsLeavesAnUnrelatedConfigAlone(t *testing.T) {
	in := []byte(`{"route":{"rule_set":[{"type":"inline","tag":"c","rules":[]}],"rules":[{"domain_suffix":["y"]}]}}`)
	out, dropped := dropRuleSets(in, nil)
	if len(dropped) != 0 || string(out) != string(in) {
		t.Errorf("config was changed although nothing was droppable: dropped=%v out=%s", dropped, out)
	}
	if _, dropped := dropRuleSets([]byte("not json"), nil); len(dropped) != 0 {
		t.Error("garbage input must be left alone")
	}
}

func TestFailedRuleSetTagsAreReadFromTheJoinedError(t *testing.T) {
	err := errString(`start service: (initialize rule-set[4]: initial rule-set: geosite-x-srs-12: Get "https://h/x.srs": EOF | initialize rule-set[0]: initial rule-set: geoip-private-srs-7: Get "https://h/p.srs": lookup h: timeout)`)
	tags := failedRuleSetTags(err)
	if len(tags) != 2 || !tags["geosite-x-srs-12"] || !tags["geoip-private-srs-7"] {
		t.Errorf("tags = %v", tags)
	}
}

type errString string

func (e errString) Error() string { return string(e) }

// startConfig has a reachable rule set "good" and one "bad" whose host refuses
// connections, each with a rule that sends its matches direct, and a dead default
// proxy so that a rule leaking through as match-all would be visible.
func startConfig(t *testing.T, goodURL string, badURLs ...string) []byte {
	t.Helper()
	sets := []any{}
	rules := []any{}
	if goodURL != "" {
		sets = append(sets, map[string]any{"type": "remote", "tag": "good", "format": "binary", "url": goodURL, "download_detour": "direct"})
		rules = append(rules, map[string]any{"rule_set": []string{"good"}, "outbound": "direct"})
	}
	for i, u := range badURLs {
		tag := "bad" + string(rune('0'+i))
		sets = append(sets, map[string]any{"type": "remote", "tag": tag, "format": "binary", "url": u, "download_detour": "direct"})
		rules = append(rules, map[string]any{"rule_set": []string{tag}, "outbound": "direct"})
	}
	cfg := map[string]any{
		"log": map[string]any{"level": "error"},
		"dns": map[string]any{"servers": []any{map[string]any{"type": "local", "tag": "dns-direct"}}},
		"outbounds": []any{
			map[string]any{"type": "direct", "tag": "direct"},
			map[string]any{"type": "socks", "tag": "proxy", "server": "127.0.0.1", "server_port": 1},
		},
		"route": map[string]any{
			"rule_set": sets, "rules": rules, "final": "proxy",
			"default_domain_resolver": map[string]any{"server": "dns-direct"},
		},
	}
	raw, err := json.Marshal(cfg)
	if err != nil {
		t.Fatal(err)
	}
	return raw
}

func TestCreateStartsWhenOneRuleSetCannotBeDownloaded(t *testing.T) {
	good := serveRuleSet(t)
	t.Chdir(t.TempDir())
	inst, cancel, err := Create(startConfig(t, good, "http://127.0.0.1:1/bad.srs"), nil)
	if err != nil {
		t.Fatalf("a failing rule set must not stop the core: %v", err)
	}
	defer func() { _ = inst.Close(); cancel() }()
}

func TestCreateStartsWhenEveryRuleSetFails(t *testing.T) {
	t.Chdir(t.TempDir())
	inst, cancel, err := Create(startConfig(t, "", "http://127.0.0.1:1/a.srs", "http://127.0.0.1:1/b.srs"), nil)
	if err != nil {
		t.Fatalf("the core must start without any rule set: %v", err)
	}
	defer func() { _ = inst.Close(); cancel() }()
}

func TestCreateKeepsOtherStartErrors(t *testing.T) {
	t.Chdir(t.TempDir())
	raw := []byte(`{"outbounds":[{"type":"direct","tag":"direct"}],"route":{"final":"does-not-exist"}}`)
	_, _, err := Create(raw, nil)
	if err == nil {
		t.Fatal("a broken config must still fail")
	}
	if isRuleSetStartError(err) {
		t.Fatalf("unrelated error was reported as a rule-set error: %v", err)
	}
}

func TestCreateStartsWhenARuleSetServerNeverAnswers(t *testing.T) {
	prev := ruleSetStartBudget
	ruleSetStartBudget = 1500 * time.Millisecond
	defer func() { ruleSetStartBudget = prev }()

	release := make(chan struct{})
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		select {
		case <-r.Context().Done():
		case <-release:
		}
	}))
	defer srv.Close()
	defer close(release)

	t.Chdir(t.TempDir())
	started := time.Now()
	inst, cancel, err := Create(startConfig(t, "", srv.URL+"/x.srs"), nil)
	if err != nil {
		t.Fatalf("a stalled download must not block the start: %v", err)
	}
	defer func() { _ = inst.Close(); cancel() }()
	if took := time.Since(started); took > 10*time.Second {
		t.Errorf("start took %s, want it bounded by the budget", took)
	}
}
