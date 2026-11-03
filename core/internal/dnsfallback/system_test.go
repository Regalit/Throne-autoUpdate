package dnsfallback

import (
	"context"
	"strings"
	"testing"
	"time"

	"github.com/sagernet/sing-box/dns"

	mDNS "github.com/miekg/dns"
)

// newSystemChain builds dns-direct on the system resolver > 1.1.1.1 > 8.8.8.8 > 9.9.9.9, as registerSystem does.
func newSystemChain() *chain {
	c := &chain{fakes: map[string]*fakeTransport{}, clock: &clock{t: time.Unix(1_700_000_000, 0)}}
	system := &fakeTransport{name: SystemKey, calls: &c.calls}
	c.fakes[SystemKey] = system
	members := []member{{key: SystemKey, transport: system, owned: true}}
	for _, server := range systemAlternates {
		key := serverKey(server, 0)
		f := &fakeTransport{name: key, calls: &c.calls}
		c.fakes[key] = f
		members = append(members, member{key: key, transport: f, owned: true})
	}
	st := newSystemTransport(system, nil, "test", members, newBlockList(c.clock.now))
	// The real budget is seconds; the order is what is checked here.
	st.primaryTimeout = 20 * time.Millisecond
	c.transport = st.fallbackTransport
	c.transport.announce = func(_ context.Context, line string) { c.announced = append(c.announced, line) }
	return c
}

func TestSystemAnswersNoDoH(t *testing.T) {
	c := newSystemChain()
	if err := c.exchange(t); err != nil {
		t.Fatal(err)
	}
	c.expectCalls(t, SystemKey)
}

// The journal's case: the system resolver never answers and the first rule-set download must not wait
// for the whole lookup deadline; DoH takes over within the system resolver's own budget.
func TestSilentSystemFallsBackToDoHInTime(t *testing.T) {
	c := newSystemChain()
	c.fakes[SystemKey].silent = true
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	start := time.Now()
	c.calls = nil
	if _, err := c.transport.Exchange(ctx, new(mDNS.Msg).SetQuestion("example.com.", mDNS.TypeA)); err != nil {
		t.Fatal(err)
	}
	if took := time.Since(start); took > time.Second {
		t.Fatalf("fallback took %v, the system resolver's budget must bound it", took)
	}
	c.expectCalls(t, SystemKey, "1.1.1.1:443")
	want := "dns-direct [test]: system did not answer, using 1.1.1.1:443"
	if len(c.announced) != 1 || c.announced[0] != want {
		t.Fatalf("announced %q, want %q", c.announced, want)
	}

	// Skipped for a while, so later lookups do not pay the wait again; then tried again.
	if err := c.exchange(t); err != nil {
		t.Fatal(err)
	}
	c.expectCalls(t, "1.1.1.1:443")
	c.fakes[SystemKey].silent = false
	c.clock.t = c.clock.t.Add(SystemSkippedFor + time.Second)
	if err := c.exchange(t); err != nil {
		t.Fatal(err)
	}
	c.expectCalls(t, SystemKey)
}

func TestSystemFallbackOrder(t *testing.T) {
	c := newSystemChain()
	c.fakes[SystemKey].silent = true
	c.fakes["1.1.1.1:443"].err = refusedDial("1.1.1.1")
	c.fakes["8.8.8.8:443"].err = context.DeadlineExceeded
	if err := c.exchange(t); err != nil {
		t.Fatal(err)
	}
	c.expectCalls(t, SystemKey, "1.1.1.1:443", "8.8.8.8:443", "9.9.9.9:443")
}

// An NXDOMAIN is the resolver's answer (an intranet name, say); DoH would only contradict it.
func TestSystemAnswerIsNotRetried(t *testing.T) {
	c := newSystemChain()
	c.fakes[SystemKey].err = dns.RcodeNameError
	if err := c.exchange(t); err == nil {
		t.Fatal("want the resolver's NXDOMAIN")
	}
	c.expectCalls(t, SystemKey)
}

// Every server skipped: the system resolver is still dialed, not the last DoH.
func TestSystemIsTheLastResort(t *testing.T) {
	c := newSystemChain()
	for _, f := range c.fakes {
		f.err = refusedDial(f.name)
	}
	_ = c.exchange(t)
	c.expectCalls(t, SystemKey, "1.1.1.1:443", "8.8.8.8:443", "9.9.9.9:443")
	err := c.exchange(t)
	c.expectCalls(t, SystemKey)
	if err == nil || !strings.Contains(err.Error(), "dns-direct [test]: ") {
		t.Fatalf("want the role tag on the error, got %v", err)
	}
}
