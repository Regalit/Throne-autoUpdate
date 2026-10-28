package dnsfallback

import (
	"context"
	"errors"
	"net"
	"net/url"
	"os"
	"strings"
	"syscall"
	"testing"
	"time"

	"github.com/sagernet/sing-box/adapter"
	"github.com/sagernet/sing-box/option"

	mDNS "github.com/miekg/dns"
)

// The chain sing-box hands back for a refused DoH dial: url.Error > OpError > SyscallError > Errno.
func refusedDial(addr string) error {
	return &url.Error{Op: "Post", URL: "https://" + addr + "/dns-query", Err: &net.OpError{
		Op: "dial", Net: "tcp", Err: os.NewSyscallError("connectex", wsaeacces),
	}}
}

type fakeTransport struct {
	name  string
	err   error
	calls *[]string
}

func (f *fakeTransport) Type() string                                                     { return "https" }
func (f *fakeTransport) Tag() string                                                      { return DirectTag }
func (f *fakeTransport) Dependencies() []string                                           { return nil }
func (f *fakeTransport) Start(adapter.StartStage) error                                   { return nil }
func (f *fakeTransport) Close() error                                                     { return nil }
func (f *fakeTransport) Reset()                                                           {}
func (f *fakeTransport) ExchangeAsync(context.Context, *mDNS.Msg, func(*mDNS.Msg, error)) {}
func (f *fakeTransport) Exchange(_ context.Context, m *mDNS.Msg) (*mDNS.Msg, error) {
	*f.calls = append(*f.calls, f.name)
	if f.err != nil {
		return nil, f.err
	}
	return new(mDNS.Msg).SetReply(m), nil
}

type clock struct{ t time.Time }

func (c *clock) now() time.Time { return c.t }

type chain struct {
	transport *fallbackTransport
	calls     []string
	fakes     map[string]*fakeTransport
	clock     *clock
}

// newChain builds primary > 8.8.8.8 > 9.9.9.9 > dns-local; withLocal false leaves dns-local out of the box.
func newChain(withLocal bool) *chain {
	c := &chain{fakes: map[string]*fakeTransport{}, clock: &clock{t: time.Unix(1_700_000_000, 0)}}
	var members []member
	for _, key := range []string{"1.1.1.1:443", "8.8.8.8:443", "9.9.9.9:443"} {
		f := &fakeTransport{name: key, calls: &c.calls}
		c.fakes[key] = f
		members = append(members, member{key: key, transport: f, owned: true})
	}
	local := &fakeTransport{name: LocalTag, calls: &c.calls}
	c.fakes[LocalTag] = local
	members = append(members, member{key: LocalTag, lookup: func() adapter.DNSTransport {
		if !withLocal {
			return nil
		}
		return local
	}})
	c.transport = newTransport(c.fakes["1.1.1.1:443"], nil, "test", members, newBlockList(c.clock.now))
	return c
}

func (c *chain) exchange(t *testing.T) error {
	t.Helper()
	c.calls = nil
	_, err := c.transport.Exchange(context.Background(), new(mDNS.Msg).SetQuestion("example.com.", mDNS.TypeA))
	return err
}

func (c *chain) expectCalls(t *testing.T, want ...string) {
	t.Helper()
	if strings.Join(c.calls, ",") != strings.Join(want, ",") {
		t.Fatalf("tried %v, want %v", c.calls, want)
	}
}

func TestIsAccessDenied(t *testing.T) {
	cases := []struct {
		name string
		err  error
		want bool
	}{
		{"nil", nil, false},
		{"wsaeacces chain", refusedDial("1.1.1.1"), true},
		{"wsaeacces as text only", errors.New("dial tcp 1.1.1.1:443: connectex: An attempt was made to access a socket in a way forbidden by its access permissions."), true},
		{"eacces", &net.OpError{Op: "dial", Err: os.NewSyscallError("connect", syscall.EACCES)}, true},
		{"eperm", &net.OpError{Op: "dial", Err: os.NewSyscallError("connect", syscall.EPERM)}, true},
		{"refused", &net.OpError{Op: "dial", Err: os.NewSyscallError("connect", syscall.ECONNREFUSED)}, false},
		{"deadline", context.DeadlineExceeded, false},
		{"timeout text", errors.New("dial tcp 1.1.1.1:443: i/o timeout"), false},
	}
	for _, tc := range cases {
		if got := IsAccessDenied(tc.err); got != tc.want {
			t.Errorf("%s: got %v, want %v", tc.name, got, tc.want)
		}
	}
}

func TestPrimaryAnswersNoFallback(t *testing.T) {
	c := newChain(true)
	if err := c.exchange(t); err != nil {
		t.Fatal(err)
	}
	c.expectCalls(t, "1.1.1.1:443")
}

func TestFallbackOrderOnAccessDenied(t *testing.T) {
	c := newChain(true)
	c.fakes["1.1.1.1:443"].err = refusedDial("1.1.1.1")
	c.fakes["8.8.8.8:443"].err = refusedDial("8.8.8.8")
	if err := c.exchange(t); err != nil {
		t.Fatal(err)
	}
	c.expectCalls(t, "1.1.1.1:443", "8.8.8.8:443", "9.9.9.9:443")

	c.fakes["9.9.9.9:443"].err = refusedDial("9.9.9.9")
	c.clock.t = c.clock.t.Add(BlockedFor + time.Second) // all fresh again
	if err := c.exchange(t); err != nil {
		t.Fatal(err)
	}
	c.expectCalls(t, "1.1.1.1:443", "8.8.8.8:443", "9.9.9.9:443", LocalTag)
}

func TestOtherErrorsDoNotFallBack(t *testing.T) {
	c := newChain(true)
	c.fakes["1.1.1.1:443"].err = &net.OpError{Op: "dial", Err: os.NewSyscallError("connect", syscall.ETIMEDOUT)}
	err := c.exchange(t)
	if err == nil {
		t.Fatal("want the primary's error")
	}
	c.expectCalls(t, "1.1.1.1:443")
	if !errors.Is(err, syscall.ETIMEDOUT) {
		t.Fatalf("the cause must stay reachable: %v", err)
	}
}

func TestBlockedIsRememberedThenRetried(t *testing.T) {
	c := newChain(true)
	c.fakes["1.1.1.1:443"].err = refusedDial("1.1.1.1")
	if err := c.exchange(t); err != nil {
		t.Fatal(err)
	}
	c.expectCalls(t, "1.1.1.1:443", "8.8.8.8:443")

	c.clock.t = c.clock.t.Add(BlockedFor - time.Second)
	if err := c.exchange(t); err != nil {
		t.Fatal(err)
	}
	c.expectCalls(t, "8.8.8.8:443")

	c.fakes["1.1.1.1:443"].err = nil // unblocked meanwhile
	c.clock.t = c.clock.t.Add(2 * time.Second)
	if err := c.exchange(t); err != nil {
		t.Fatal(err)
	}
	c.expectCalls(t, "1.1.1.1:443")
}

func TestEverythingBlockedStillDialsTheLast(t *testing.T) {
	c := newChain(true)
	for _, f := range c.fakes {
		f.err = refusedDial(f.name)
	}
	if err := c.exchange(t); err == nil {
		t.Fatal("want an error")
	}
	c.expectCalls(t, "1.1.1.1:443", "8.8.8.8:443", "9.9.9.9:443", LocalTag)
	err := c.exchange(t)
	c.expectCalls(t, LocalTag)
	if err == nil || !strings.Contains(err.Error(), "dns-direct [test]: ") {
		t.Fatalf("want the role tag on the error, got %v", err)
	}

	// Without dns-local in the config the last DoH is the one still tried.
	c = newChain(false)
	for _, f := range c.fakes {
		f.err = refusedDial(f.name)
	}
	_ = c.exchange(t)
	c.expectCalls(t, "1.1.1.1:443", "8.8.8.8:443", "9.9.9.9:443")
	_ = c.exchange(t)
	c.expectCalls(t, "9.9.9.9:443")
}

func TestApplies(t *testing.T) {
	ip := func(tag, server, detour string) bool {
		var o option.RemoteHTTPSDNSServerOptions
		o.Server = server
		o.Detour = detour
		return applies(tag, o)
	}
	if !ip(DirectTag, "1.1.1.1", "") {
		t.Error("dns-direct DoH to an IP must be wrapped")
	}
	if ip("dns-remote", "1.1.1.1", "") || ip(DirectTag, "dns.google", "") || ip(DirectTag, "1.1.1.1", "proxy") {
		t.Error("only a direct dns-direct DoH to an IP is wrapped")
	}
	if got := strings.Join(alternatesFor("8.8.8.8"), ","); got != "9.9.9.9" {
		t.Errorf("alternates for 8.8.8.8: %s", got)
	}
	if got := strings.Join(alternatesFor("1.1.1.1"), ","); got != "8.8.8.8,9.9.9.9" {
		t.Errorf("alternates for 1.1.1.1: %s", got)
	}
}
