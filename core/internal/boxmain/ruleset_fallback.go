package boxmain

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"regexp"
	"sort"
	"strings"
	"time"

	"ThroneCore/internal/boxbox"

	"github.com/sagernet/sing-box/log"
	"github.com/sagernet/sing-box/option"
)

// A remote rule set that is not cached is downloaded while the core starts, and
// sing-box aborts the whole start if any download fails. For us that turns a
// flaky geo list into a VPN that will not connect at all. Routing lists are an
// optimisation, not a precondition, so when the start fails on them we drop the
// sets that failed (and every rule that leaned on them) and start again. The VPN
// then runs without that part of the split routing, and a later start tries the
// download again.
const maxRuleSetFallbacks = 3

// ruleSetStartBudget is how long a start that carries remote rule sets may take
// before the downloads still pending are given up on. A healthy cold download of
// the whole list takes well under a second; this only bounds the broken cases.
var ruleSetStartBudget = 15 * time.Second

var remoteRuleSetRE = regexp.MustCompile(`"type"\s*:\s*"remote"`)

func hasRemoteRuleSets(content []byte) bool {
	return remoteRuleSetRE.Match(content)
}

// Create starts the box, falling back to a start without the remote rule sets
// that could not be downloaded. Any other start error is returned untouched.
func Create(configContent []byte, onCreated func(*boxbox.Box), adjust ...func(*option.Options)) (*boxbox.Box, context.CancelFunc, error) {
	content := configContent
	var firstErr error
	for attempt := 0; ; attempt++ {
		box, cancel, err := createOnce(content, onCreated, adjust...)
		if err == nil {
			return box, cancel, nil
		}
		if firstErr == nil {
			firstErr = err
		}
		if attempt >= maxRuleSetFallbacks || !isRuleSetStartError(err) {
			return nil, nil, firstErr
		}
		next, dropped := dropRuleSets(content, failedRuleSetTags(err))
		if len(dropped) == 0 {
			return nil, nil, firstErr
		}
		log.Warn("rule sets unavailable, starting without them: ", strings.Join(dropped, ", "))
		content = next
	}
}

// errRuleSetBudget marks a start that was cut off because remote rule sets were
// still downloading when ruleSetStartBudget ran out.
var errRuleSetBudget = errors.New("rule sets did not finish downloading in time")

func isRuleSetStartError(err error) bool {
	return err != nil && (errors.Is(err, errRuleSetBudget) || strings.Contains(err.Error(), "initial rule-set:"))
}

var failedRuleSetRE = regexp.MustCompile(`initial rule-set: ([^:]+): `)

// failedRuleSetTags pulls the tags out of "initialize rule-set[N]: initial
// rule-set: <tag>: <cause>" entries, which sing-box joins with " | ".
func failedRuleSetTags(err error) map[string]bool {
	tags := map[string]bool{}
	for _, m := range failedRuleSetRE.FindAllStringSubmatch(err.Error(), -1) {
		tags[strings.TrimSpace(m[1])] = true
	}
	return tags
}

// dropRuleSets removes the named remote rule sets from the config, plus every
// rule that references one. With no names (the error did not say which) it drops
// all remote rule sets. A rule that referenced a dropped set is removed whole:
// stripping just that matcher would leave a rule that matches more than it was
// written to, and an emptied rule_set list would match everything.
func dropRuleSets(content []byte, only map[string]bool) ([]byte, []string) {
	dec := json.NewDecoder(bytes.NewReader(content))
	dec.UseNumber()
	var root map[string]any
	if err := dec.Decode(&root); err != nil {
		return content, nil // not plain JSON: leave it alone
	}

	route, _ := root["route"].(map[string]any)
	if route == nil {
		return content, nil
	}
	sets, _ := route["rule_set"].([]any)
	removed := map[string]bool{}
	kept := make([]any, 0, len(sets))
	for _, item := range sets {
		set, _ := item.(map[string]any)
		tag, _ := set["tag"].(string)
		if set != nil && set["type"] == "remote" && (len(only) == 0 || only[tag]) {
			removed[tag] = true
			continue
		}
		kept = append(kept, item)
	}
	if len(removed) == 0 {
		return content, nil
	}
	route["rule_set"] = kept

	route["rules"] = filterRules(route["rules"], removed)
	if dns, ok := root["dns"].(map[string]any); ok {
		dns["rules"] = filterRules(dns["rules"], removed)
	}
	if inbounds, ok := root["inbounds"].([]any); ok {
		for _, in := range inbounds {
			inbound, _ := in.(map[string]any)
			for _, key := range []string{"route_address_set", "route_exclude_address_set"} {
				if list, ok := inbound[key]; ok {
					inbound[key] = withoutTags(list, removed)
				}
			}
		}
	}

	out, err := json.Marshal(root)
	if err != nil {
		return content, nil
	}
	names := make([]string, 0, len(removed))
	for tag := range removed {
		names = append(names, tag)
	}
	sort.Strings(names)
	return out, names
}

func filterRules(raw any, removed map[string]bool) any {
	rules, ok := raw.([]any)
	if !ok {
		return raw
	}
	out := make([]any, 0, len(rules))
	for _, r := range rules {
		if !referencesRuleSet(r, removed) {
			out = append(out, r)
		}
	}
	return out
}

// referencesRuleSet looks through nested logical rules too.
func referencesRuleSet(node any, removed map[string]bool) bool {
	switch v := node.(type) {
	case map[string]any:
		for key, val := range v {
			if key == "rule_set" && hasTag(val, removed) {
				return true
			}
			if key == "rules" && referencesRuleSet(val, removed) {
				return true
			}
		}
	case []any:
		for _, item := range v {
			if referencesRuleSet(item, removed) {
				return true
			}
		}
	}
	return false
}

func hasTag(val any, removed map[string]bool) bool {
	switch v := val.(type) {
	case string:
		return removed[v]
	case []any:
		for _, item := range v {
			if s, ok := item.(string); ok && removed[s] {
				return true
			}
		}
	}
	return false
}

func withoutTags(val any, removed map[string]bool) any {
	switch v := val.(type) {
	case string:
		if removed[v] {
			return []any{}
		}
	case []any:
		out := make([]any, 0, len(v))
		for _, item := range v {
			if s, ok := item.(string); ok && removed[s] {
				continue
			}
			out = append(out, item)
		}
		return out
	}
	return val
}
