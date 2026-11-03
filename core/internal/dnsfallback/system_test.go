package dnsfallback

import (
	"context"
	"strings"
	"testing"
	"time"

	"github.com/sagernet/sing-box/adapter"
	"github.com/sagernet/sing-box/dns"

	mDNS "github.com/miekg/dns"
)

// preferringFake is a system resolver with preferred domains (neighbors), like sing-box's local transport.
type preferringFake struct {
	*fakeTransport
	preferred string
}

func (p *preferringFake) PreferredDomain(domain string) bool { return domain == p.preferred }

// newSystemChain builds dns-direct on the system resolver > 1.1.1.1 > 8.8.8.8 > 9.9.9.9, as registerSystem
// does, in the box's startup phase.
func newSystemChain() (*chain, *systemTransport) {
	return newSystemChainWithBlocks(nil)
}

func newSystemChainWithBlocks(blocks *blockList) (*chain, *systemTransport) {
	c := &chain{fakes: map[string]*fakeTransport{}, clock: &clock{t: time.Unix(1_700_000_000, 0)}}
	if blocks == nil {
		blocks = newBlockList(c.clock.now)
	}
	system := &fakeTransport{name: SystemKey, calls: &c.calls}
	c.fakes[SystemKey] = system
	primary := &preferringFake{fakeTransport: system, preferred: "printer.lan."}
	members := []member{{key: SystemKey, transport: primary, owned: true}}
	for _, server := range systemAlternates {
		key := serverKey(server, 0)
		f := &fakeTransport{name: key, calls: &c.calls}
		c.fakes[key] = f
		members = append(members, member{key: key, transport: f, owned: true})
	}
	st := newSystemTransport(primary, nil, "test", members, blocks)
	// The real budget is seconds; the order is what is checked here.
	st.budget = func() time.Duration { return 20 * time.Millisecond }
	c.transport = st.fallbackTransport
	c.front = st
	c.transport.announce = func(_ context.Context, line string) { c.announced = append(c.announced, line) }
	return c, st
}

func TestSystemAnswersNoDoH(t *testing.T) {
	c, _ := newSystemChain()
	if err := c.exchange(t); err != nil {
		t.Fatal(err)
	}
	c.expectCalls(t, SystemKey)
}

// The journal's case: the system resolver never answers and the first rule-set download must not wait
// for the whole lookup deadline; DoH takes over once the system resolver's budget is spent.
func TestSilentSystemFallsBackToDoHInTime(t *testing.T) {
	c, st := newSystemChain()
	c.fakes[SystemKey].silent = true
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	start := time.Now()
	c.calls = nil
	if _, err := st.Exchange(ctx, new(mDNS.Msg).SetQuestion("example.com.", mDNS.TypeA)); err != nil {
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

// The budget must outlast the system's own per-server timeout (5 s on Windows), or a PC whose first
// DNS server is dead would be moved to DoH although the second server answers.
func TestSystemBudgetOutlastsPerServerTimeout(t *testing.T) {
	_, st := newSystemChain()
	if st.primaryTimeout <= systemDefaultTimeout {
		t.Fatalf("default budget %v is not above the per-server timeout %v", st.primaryTimeout, systemDefaultTimeout)
	}
	if systemAttemptMargin <= 0 {
		t.Fatal("the budget needs a margin over the per-server timeout")
	}

	// A resolver that answers after its first server timed out (here: just inside the budget) is kept.
	c, st := newSystemChain()
	st.budget = func() time.Duration { return 300 * time.Millisecond }
	st.members[0].transport = &delayed{DNSTransport: c.fakes[SystemKey], delay: 100 * time.Millisecond}
	if err := c.exchange(t); err != nil {
		t.Fatal(err)
	}
	c.expectCalls(t, SystemKey)
}

// delayed answers after a while, like a system resolver whose first server is dead.
type delayed struct {
	adapter.DNSTransport
	delay time.Duration
}

func (d *delayed) Exchange(ctx context.Context, m *mDNS.Msg) (*mDNS.Msg, error) {
	select {
	case <-time.After(d.delay):
		return d.DNSTransport.Exchange(ctx, m)
	case <-ctx.Done():
		return nil, ctx.Err()
	}
}

func TestSystemFallbackOrder(t *testing.T) {
	c, _ := newSystemChain()
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
	c, _ := newSystemChain()
	c.fakes[SystemKey].err = dns.RcodeNameError
	if err := c.exchange(t); err == nil {
		t.Fatal("want the resolver's NXDOMAIN")
	}
	c.expectCalls(t, SystemKey)
}

// Every server skipped: the system resolver is still dialed, not the last DoH.
func TestSystemIsTheLastResort(t *testing.T) {
	c, _ := newSystemChain()
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

// *.local and the resolver's preferred domains never go to public DoH, and a silence there does not
// mark the system resolver as skipped.
func TestLocalNamesBypassTheFallback(t *testing.T) {
	for _, name := range []string{"nas.local.", "printer.lan."} {
		c, _ := newSystemChain()
		c.fakes[SystemKey].err = context.DeadlineExceeded
		if err := c.exchangeName(t, name); err == nil {
			t.Fatalf("%s: want the system resolver's error", name)
		}
		c.expectCalls(t, SystemKey)

		c.fakes[SystemKey].err = nil
		if err := c.exchange(t); err != nil {
			t.Fatal(err)
		}
		c.expectCalls(t, SystemKey) // not skipped
	}
}

// Once the box has started, every lookup stays with the system resolver: the fallback only covers the
// startup (the initial rule-set downloads).
func TestFallbackOnlyWhileStarting(t *testing.T) {
	c, st := newSystemChain()
	for _, stage := range []adapter.StartStage{adapter.StartStateInitialize, adapter.StartStateStart, adapter.StartStatePostStart, adapter.StartStateStarted} {
		if err := st.Start(stage); err != nil {
			t.Fatal(err)
		}
	}
	c.fakes[SystemKey].silent = true
	ctx, cancel := context.WithTimeout(context.Background(), 100*time.Millisecond)
	defer cancel()
	c.calls = nil
	if _, err := st.Exchange(ctx, new(mDNS.Msg).SetQuestion("example.com.", mDNS.TypeA)); err == nil {
		t.Fatal("want the system resolver's timeout")
	}
	c.expectCalls(t, SystemKey)
}

// The system chain and the DoH chain share the process-wide block list but not their keys: a DoH that
// timed out behind the system resolver does not hide the user's own 1.1.1.1 primary.
func TestChainsKeepSeparateBlockKeys(t *testing.T) {
	shared := newBlockList(time.Now)
	sys, _ := newSystemChainWithBlocks(shared)
	sys.fakes[SystemKey].silent = true
	sys.fakes["1.1.1.1:443"].err = context.DeadlineExceeded
	if err := sys.exchange(t); err != nil {
		t.Fatal(err)
	}
	sys.expectCalls(t, SystemKey, "1.1.1.1:443", "8.8.8.8:443")

	doh := newChain(true)
	doh.transport.blocks = shared
	doh.transport.blockPrefix = "doh-chain/"
	if err := doh.exchange(t); err != nil {
		t.Fatal(err)
	}
	doh.expectCalls(t, "1.1.1.1:443")
}
