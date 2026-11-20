package selfupdate

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"sync"
	"time"

	"golang.org/x/net/dns/dnsmessage"
)

// A second way to find our server. In «Все приложения» (TUN) mode the system resolver is the
// VPN core's DNS, and when that DNS breaks the channel's name does not resolve although the
// network is fine. Then the name is asked of public DNS-over-HTTPS servers by IP (no name to
// resolve first; in TUN mode that request goes through the tunnel like any other), and if
// that fails too, the address that last worked is used. TLS still checks the certificate for
// the name, so a wrong address fails the handshake instead of reaching a stranger.

// addrFile keeps, per host, the addresses that last connected.
const addrFile = "addr.json"

// fallbackDoH are asked in order; IP literals, so they need no DNS of their own.
var fallbackDoH = []string{"https://1.1.1.1/dns-query", "https://8.8.8.8/dns-query"}

// dohTimeout bounds one DoH question.
var dohTimeout = 5 * time.Second

// fallbackDialer dials like net.Dialer and, when the name does not resolve, tries
// DoH and then the remembered addresses. used tells how the last fallback went.
type fallbackDialer struct {
	work   string
	dialer net.Dialer
	doh    func(ctx context.Context, host string) ([]string, error)

	mu   sync.Mutex
	used string // "", "doh" or "cache": the last connection that needed the fallback
}

func newFallbackDialer(work string) *fallbackDialer {
	d := &fallbackDialer{work: work, dialer: net.Dialer{Timeout: 15 * time.Second, KeepAlive: 30 * time.Second}}
	d.doh = d.askDoH
	return d
}

// Used is how the last fallback connection found the address: "" (none was needed), doh or cache.
func (d *fallbackDialer) Used() string {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.used
}

func (d *fallbackDialer) DialContext(ctx context.Context, network, addr string) (net.Conn, error) {
	host, port, err := net.SplitHostPort(addr)
	if err != nil || net.ParseIP(host) != nil {
		return d.dialer.DialContext(ctx, network, addr)
	}
	conn, err := d.dialer.DialContext(ctx, network, addr)
	if err == nil {
		d.remember(host, conn.RemoteAddr())
		return conn, nil
	}
	var dnsErr *net.DNSError
	if !errors.As(err, &dnsErr) {
		return nil, err // the name resolved; the trouble is elsewhere
	}
	if ips, derr := d.doh(ctx, host); derr == nil {
		if c := d.dialAny(ctx, network, ips, port); c != nil {
			d.setUsed("doh")
			return c, nil
		}
	}
	if c := d.dialAny(ctx, network, d.remembered(host), port); c != nil {
		d.setUsed("cache")
		return c, nil
	}
	return nil, err
}

func (d *fallbackDialer) dialAny(ctx context.Context, network string, ips []string, port string) net.Conn {
	for _, ip := range ips {
		if c, err := d.dialer.DialContext(ctx, network, net.JoinHostPort(ip, port)); err == nil {
			return c
		}
	}
	return nil
}

func (d *fallbackDialer) setUsed(how string) {
	d.mu.Lock()
	d.used = how
	d.mu.Unlock()
}

func (d *fallbackDialer) remember(host string, a net.Addr) {
	ta, ok := a.(*net.TCPAddr)
	if !ok || d.work == "" {
		return
	}
	d.mu.Lock()
	defer d.mu.Unlock()
	m := d.readAddrs()
	ip := ta.IP.String()
	if len(m[host]) > 0 && m[host][0] == ip {
		return
	}
	m[host] = []string{ip}
	if b, err := json.Marshal(m); err == nil {
		_ = os.WriteFile(filepath.Join(d.work, addrFile), b, 0o644)
	}
}

func (d *fallbackDialer) remembered(host string) []string {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.readAddrs()[host]
}

func (d *fallbackDialer) readAddrs() map[string][]string {
	m := map[string][]string{}
	if b, err := os.ReadFile(filepath.Join(d.work, addrFile)); err == nil {
		_ = json.Unmarshal(b, &m)
	}
	return m
}

// askDoH asks each fallbackDoH server for host's A records (RFC 8484, POST).
func (d *fallbackDialer) askDoH(ctx context.Context, host string) ([]string, error) {
	q, err := dnsQuery(host)
	if err != nil {
		return nil, err
	}
	client := &http.Client{Timeout: dohTimeout, Transport: &http.Transport{DialContext: d.dialer.DialContext, ForceAttemptHTTP2: true}}
	defer client.CloseIdleConnections()
	var last error = errors.New("doh: no servers")
	for _, srv := range fallbackDoH {
		ips, err := dohGet(ctx, client, srv, q)
		if err == nil && len(ips) > 0 {
			return ips, nil
		}
		if err == nil {
			err = fmt.Errorf("doh %s: no address for %s", srv, host)
		}
		last = err
	}
	return nil, last
}

func dnsQuery(host string) ([]byte, error) {
	name, err := dnsmessage.NewName(host + ".")
	if err != nil {
		return nil, err
	}
	msg := dnsmessage.Message{
		Header:    dnsmessage.Header{RecursionDesired: true},
		Questions: []dnsmessage.Question{{Name: name, Type: dnsmessage.TypeA, Class: dnsmessage.ClassINET}},
	}
	return msg.Pack()
}

func dohGet(ctx context.Context, client *http.Client, srv string, q []byte) ([]string, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, srv, bytes.NewReader(q))
	if err != nil {
		return nil, err
	}
	req.Header.Set("Content-Type", "application/dns-message")
	req.Header.Set("Accept", "application/dns-message")
	resp, err := client.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("doh %s: HTTP %d", srv, resp.StatusCode)
	}
	body, err := io.ReadAll(io.LimitReader(resp.Body, 64<<10))
	if err != nil {
		return nil, err
	}
	return parseA(body)
}

func parseA(body []byte) ([]string, error) {
	var p dnsmessage.Parser
	if _, err := p.Start(body); err != nil {
		return nil, err
	}
	if err := p.SkipAllQuestions(); err != nil {
		return nil, err
	}
	var ips []string
	for {
		h, err := p.AnswerHeader()
		if errors.Is(err, dnsmessage.ErrSectionDone) {
			return ips, nil
		}
		if err != nil {
			return ips, err
		}
		if h.Type != dnsmessage.TypeA {
			if err := p.SkipAnswer(); err != nil {
				return ips, err
			}
			continue
		}
		a, err := p.AResource()
		if err != nil {
			return ips, err
		}
		ips = append(ips, net.IP(a.A[:]).String())
	}
}
