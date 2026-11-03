// Package dnsfallback keeps dns-direct answering when something on the PC forbids its DoH destination.
//
// On some Windows installs every dial to https://1.1.1.1/dns-query fails at once with WSAEACCES (10013,
// "forbidden by its access permissions") while proxy server dials work: an antivirus, a firewall rule or
// Defender Network Protection blocks known DoH addresses, not the exe. The GUI points dns-direct at a DoH IP
// for tests and the start retry because the core's own "local" lookups time out on some PCs, and a DoH IP also
// keeps a poisoned system resolver from handing out fake server addresses. So on such a block this tries other
// DoH IPs first and the system resolver (dns-local) last, and remembers the blocked ones for a while.
package dnsfallback

import (
	"context"
	"errors"
	"net/netip"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/sagernet/sing-box/adapter"
	C "github.com/sagernet/sing-box/constant"
	"github.com/sagernet/sing-box/dns"
	"github.com/sagernet/sing-box/dns/transport"
	"github.com/sagernet/sing-box/dns/transport/local"
	"github.com/sagernet/sing-box/log"
	"github.com/sagernet/sing-box/option"
	E "github.com/sagernet/sing/common/exceptions"
	"github.com/sagernet/sing/service"

	mDNS "github.com/miekg/dns"
)

// Match tags::dnsDirect and tags::dnsLocal in the config builder (src/configs/generate.cpp).
const (
	DirectTag = "dns-direct"
	LocalTag  = "dns-local"
)

// How long a destination that refused us stays skipped; a later lookup then tries it again.
const BlockedFor = 10 * time.Minute

// One fallback DoH that drops packets silently must not use up the whole lookup budget: the next server
// (in the end the system resolver) still needs time to answer.
const fallbackAttemptTimeout = 2 * time.Second

// How long a dns-direct on the system resolver gets before the DoH servers are asked. The first remote
// rule-set download at start resolves through dns-direct, and the start waits on it: a system resolver
// that never answered ran that lookup into its deadline and failed the start, and the GUI's DoH retry
// came only after that. A healthy resolver answers well within this.
const systemAttemptTimeout = 3 * time.Second

// How long a system resolver that did not answer is skipped. Shorter than BlockedFor: a silent resolver
// is usually a network change or a waking adapter, not a standing block.
const SystemSkippedFor = time.Minute

// SystemKey names the system resolver at the head of dns-direct's chain, in the block list and the log.
const SystemKey = "system"

// Tried in this order after the configured DoH, each only when the one before was refused locally.
var alternateServers = []string{"8.8.8.8", "9.9.9.9"}

// WSAEACCES: Windows refuses the socket itself (AV, firewall, Network Protection), before any packet leaves.
const wsaeacces = syscall.Errno(10013)

// IsAccessDenied reports whether err is a local refusal to dial, the one case worth another server:
// a timeout or a bad answer would only be repeated against the next one.
func IsAccessDenied(err error) bool {
	if err == nil {
		return false
	}
	var errno syscall.Errno
	if errors.As(err, &errno) && (errno == wsaeacces || errno == syscall.EACCES || errno == syscall.EPERM) {
		return true
	}
	// The errno can be lost where a layer flattens the error into text.
	text := err.Error()
	return strings.Contains(text, "forbidden by its access permissions") || strings.Contains(text, "permission denied")
}

// Register wraps the https DNS type so that a dns-direct DoH to an IP falls back as described above.
// role tags this box's DNS errors in the log ("profile", "test"), so a journal line says which config failed.
func Register(registry *dns.TransportRegistry, role string) {
	registerSystem(registry, role)
	dns.RegisterTransport[option.RemoteHTTPSDNSServerOptions](registry, C.DNSTypeHTTPS,
		func(ctx context.Context, logger log.ContextLogger, tag string, options option.RemoteHTTPSDNSServerOptions) (adapter.DNSTransport, error) {
			primary, err := transport.NewHTTPS(ctx, logger, tag, options)
			if err != nil || !applies(tag, options) {
				return primary, err
			}
			members := []member{{key: serverKey(options.Server, options.ServerPort), transport: primary, owned: true}}
			for _, server := range alternatesFor(options.Server) {
				var alternate option.RemoteHTTPSDNSServerOptions
				// Same dialer settings (bind interface, routing mark), the server's own name and path.
				alternate.RawLocalDNSServerOptions = options.RawLocalDNSServerOptions
				alternate.Server = server
				altTransport, altErr := transport.NewHTTPS(ctx, logger, tag, alternate)
				if altErr != nil {
					// A spare that cannot be built must not stop the box; the chain just goes without it.
					logger.Warn(tag, ": fallback DNS ", server, " skipped: ", altErr)
					continue
				}
				members = append(members, member{key: serverKey(server, 0), transport: altTransport, owned: true})
			}
			// The box's own system resolver; looked up per query, since it may be built after this one.
			members = append(members, member{key: LocalTag, lookup: func() adapter.DNSTransport {
				manager := service.FromContext[adapter.DNSTransportManager](ctx)
				if manager == nil {
					return nil
				}
				local, loaded := manager.Transport(LocalTag)
				if !loaded {
					return nil
				}
				return local
			}})
			return newTransport(primary, logger, role, members, defaultBlocked), nil
		})
}

// Only a direct dial to a DoH IP: a detour goes through the proxy, where no local filter applies, and a
// falling back to the system resolver there would leak the query outside the tunnel.
func applies(tag string, options option.RemoteHTTPSDNSServerOptions) bool {
	if tag != DirectTag || options.Detour != "" {
		return false
	}
	_, err := netip.ParseAddr(options.Server)
	return err == nil
}

// registerSystem wraps the local DNS type so that a dns-direct on the system resolver ("local" as the
// GUI's direct DNS) moves on to DoH 1.1.1.1 > 8.8.8.8 > 9.9.9.9 when the system resolver gives no answer
// within systemAttemptTimeout. dns-local stays as it is: it is the DoH chain's own last resort.
func registerSystem(registry *dns.TransportRegistry, role string) {
	dns.RegisterTransport[option.LocalDNSServerOptions](registry, C.DNSTypeLocal,
		func(ctx context.Context, logger log.ContextLogger, tag string, options option.LocalDNSServerOptions) (adapter.DNSTransport, error) {
			primary, err := local.NewTransport(ctx, logger, tag, options)
			if err != nil || tag != DirectTag || options.Detour != "" {
				return primary, err
			}
			members := []member{{key: SystemKey, transport: primary, owned: true}}
			for _, server := range systemAlternates {
				var alternate option.RemoteHTTPSDNSServerOptions
				// Same dialer settings (bind interface, routing mark) as the system resolver.
				alternate.RawLocalDNSServerOptions = options.RawLocalDNSServerOptions
				alternate.Server = server
				altTransport, altErr := transport.NewHTTPS(ctx, logger, tag, alternate)
				if altErr != nil {
					logger.Warn(tag, ": fallback DNS ", server, " skipped: ", altErr)
					continue
				}
				members = append(members, member{key: serverKey(server, 0), transport: altTransport, owned: true})
			}
			return newSystemTransport(primary, logger, role, members, defaultBlocked), nil
		})
}

// The DoH servers behind the system resolver, in order: the GUI's own retry server first.
var systemAlternates = []string{"1.1.1.1", "8.8.8.8", "9.9.9.9"}

func newSystemTransport(primary adapter.DNSTransport, logger log.ContextLogger, role string, members []member, blocks *blockList) *systemTransport {
	t := newTransport(primary, logger, role, members, blocks)
	t.primaryTimeout = systemAttemptTimeout
	t.primarySkipFor = SystemSkippedFor
	t.primaryMovesOn = systemFailed
	t.primaryIsLastResort = true
	t.switchReason = "did not answer"
	return &systemTransport{fallbackTransport: t}
}

// systemFailed: anything but an answer from the resolver (an NXDOMAIN is one) is worth asking DoH.
func systemFailed(err error) bool {
	var rcode dns.RcodeError
	return !errors.As(err, &rcode)
}

// systemTransport keeps the local transport's optional interfaces: its preferred domains (neighbors,
// .local) and its environment, which keys the DNS cache to the network an answer came from.
type systemTransport struct {
	*fallbackTransport
}

var (
	_ adapter.DNSTransportWithPreferredDomain = (*systemTransport)(nil)
	_ adapter.DNSTransportWithEnvironment     = (*systemTransport)(nil)
)

func (t *systemTransport) PreferredDomain(domain string) bool {
	if p, ok := t.primary.(adapter.DNSTransportWithPreferredDomain); ok {
		return p.PreferredDomain(domain)
	}
	return false
}

func (t *systemTransport) Environment() []string {
	if e, ok := t.primary.(adapter.DNSTransportWithEnvironment); ok {
		return e.Environment()
	}
	return nil
}

func alternatesFor(server string) []string {
	out := make([]string, 0, len(alternateServers))
	for _, alternate := range alternateServers {
		if alternate != server {
			out = append(out, alternate)
		}
	}
	return out
}

func serverKey(server string, port uint16) string {
	if port == 0 {
		port = 443
	}
	return netip.AddrPortFrom(netip.MustParseAddr(server), port).String()
}

// blockList remembers destinations that refused us; process-wide, so a test box built a second later
// does not hit the same block again.
type blockList struct {
	access sync.Mutex
	until  map[string]time.Time
	now    func() time.Time
}

var defaultBlocked = newBlockList(time.Now)

func newBlockList(now func() time.Time) *blockList {
	return &blockList{until: make(map[string]time.Time), now: now}
}

func (b *blockList) blocked(key string) bool {
	b.access.Lock()
	defer b.access.Unlock()
	until, ok := b.until[key]
	if !ok {
		return false
	}
	if !b.now().Before(until) {
		delete(b.until, key)
		return false
	}
	return true
}

// mark reports whether key was not blocked already, so the caller logs a block once.
func (b *blockList) mark(key string) bool {
	return b.markFor(key, BlockedFor)
}

func (b *blockList) markFor(key string, d time.Duration) bool {
	b.access.Lock()
	defer b.access.Unlock()
	now := b.now()
	until, ok := b.until[key]
	b.until[key] = now.Add(d)
	return !ok || !now.Before(until)
}

type member struct {
	key       string
	transport adapter.DNSTransport
	lookup    func() adapter.DNSTransport
	// Built here, so started and closed here; dns-local belongs to the box's transport manager.
	owned bool
}

func (m member) get() adapter.DNSTransport {
	if m.transport != nil {
		return m.transport
	}
	if m.lookup != nil {
		return m.lookup()
	}
	return nil
}

type fallbackTransport struct {
	primary adapter.DNSTransport
	logger  log.ContextLogger
	role    string
	members []member
	blocks  *blockList
	// The fallback server last announced in the log; the switch line is written once per change.
	announceAccess sync.Mutex
	announced      string
	// Logs the switch at warn: the dev journal keeps a warning as ok, it is not a failure. Swapped in tests.
	announce func(ctx context.Context, line string)

	// The primary's own budget (0: the whole lookup), how long it is skipped once it failed (0: BlockedFor),
	// and which of its failures move on to the next server (a local refusal, for a DoH primary).
	primaryTimeout time.Duration
	primarySkipFor time.Duration
	primaryMovesOn func(error) bool
	// With every server skipped, the primary is the one still dialed rather than the last member.
	primaryIsLastResort bool
	// How the switch line words the primary's failure.
	switchReason string
}

var _ adapter.DNSTransport = (*fallbackTransport)(nil)

func newTransport(primary adapter.DNSTransport, logger log.ContextLogger, role string, members []member, blocks *blockList) *fallbackTransport {
	t := &fallbackTransport{primary: primary, logger: logger, role: role, members: members, blocks: blocks,
		primaryMovesOn: IsAccessDenied, switchReason: "blocked locally"}
	t.announce = func(ctx context.Context, line string) {
		if t.logger != nil {
			t.logger.WarnContext(ctx, line)
		}
	}
	return t
}

func (t *fallbackTransport) Type() string           { return t.primary.Type() }
func (t *fallbackTransport) Tag() string            { return t.primary.Tag() }
func (t *fallbackTransport) Dependencies() []string { return t.primary.Dependencies() }

func (t *fallbackTransport) Start(stage adapter.StartStage) error {
	for _, m := range t.members {
		if m.owned {
			if err := m.transport.Start(stage); err != nil {
				return err
			}
		}
	}
	return nil
}

func (t *fallbackTransport) Close() error {
	var err error
	for _, m := range t.members {
		if m.owned {
			err = E.Append(err, m.transport.Close(), func(err error) error { return err })
		}
	}
	return err
}

func (t *fallbackTransport) Reset() {
	for _, m := range t.members {
		if m.owned {
			m.transport.Reset()
		}
	}
}

func (t *fallbackTransport) ExchangeAsync(ctx context.Context, message *mDNS.Msg, callback func(response *mDNS.Msg, err error)) {
	go func() {
		callback(t.Exchange(ctx, message))
	}()
}

// candidates is the order to try: members not known to be blocked, or, when every one is, the last one
// that exists (the system resolver), so a query is never refused without a dial.
func (t *fallbackTransport) candidates() []member {
	var available, open []member
	for _, m := range t.members {
		tr := m.get()
		if tr == nil {
			continue
		}
		m.transport = tr
		available = append(available, m)
		if !t.blocks.blocked(m.key) {
			open = append(open, m)
		}
	}
	if len(open) == 0 && len(available) > 0 {
		open = available[len(available)-1:]
		if t.primaryIsLastResort && available[0].key == t.members[0].key {
			open = available[:1]
		}
	}
	return open
}

// Exchange tries the configured DoH, and moves on only when it is refused locally: a timeout there is not a
// block, and the next server would be no likelier to answer. Once past it (refused now, or remembered as
// refused), any failure of a fallback server moves on, up to the system resolver, and is remembered too.
func (t *fallbackTransport) Exchange(ctx context.Context, message *mDNS.Msg) (*mDNS.Msg, error) {
	candidates := t.candidates()
	primaryKey := t.members[0].key
	var lastErr error
	for i, m := range candidates {
		if i > 0 && ctx.Err() != nil {
			break
		}
		isPrimary := m.key == primaryKey
		last := i == len(candidates)-1
		attemptCtx := ctx
		timeout := fallbackAttemptTimeout
		if isPrimary {
			timeout = t.primaryTimeout
		}
		if timeout > 0 && !last {
			var cancel context.CancelFunc
			attemptCtx, cancel = context.WithTimeout(ctx, timeout)
			defer cancel()
		}
		response, err := m.transport.Exchange(attemptCtx, message)
		if err == nil {
			t.announceSwitch(ctx, primaryKey, m.key, isPrimary)
			return response, nil
		}
		lastErr = err
		if isPrimary && (ctx.Err() != nil || !t.primaryMovesOn(err)) {
			break
		}
		skipFor := BlockedFor
		if isPrimary && t.primarySkipFor > 0 {
			skipFor = t.primarySkipFor
		}
		if t.blocks.markFor(m.key, skipFor) && t.logger != nil {
			t.logger.DebugContext(ctx, t.Tag(), " [", t.role, "]: ", m.key, " failed (", err,
				"), trying the next DNS server; skipped for ", skipFor)
		}
	}
	if lastErr == nil {
		lastErr = E.New("no DNS server available")
	}
	return nil, E.Cause(lastErr, t.Tag(), " [", t.role, "]")
}

// The primary answering again clears the memo, so a later switch is logged again.
func (t *fallbackTransport) announceSwitch(ctx context.Context, blocked, using string, isPrimary bool) {
	if isPrimary {
		using = ""
	}
	t.announceAccess.Lock()
	changed := t.announced != using
	t.announced = using
	t.announceAccess.Unlock()
	if changed && using != "" {
		t.announce(ctx, t.Tag()+" ["+t.role+"]: "+blocked+" "+t.switchReason+", using "+using)
	}
}
