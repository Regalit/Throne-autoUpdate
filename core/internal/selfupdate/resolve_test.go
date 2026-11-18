package selfupdate

import (
	"context"
	"errors"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"golang.org/x/net/dns/dnsmessage"
)

// A name that does not resolve (the VPN core's DNS is down) is found over DoH,
// and once a connection worked its address is remembered for when DoH fails too.
func TestFallbackDialerFindsServerWhenNameDoesNotResolve(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { w.Write([]byte("hi")) }))
	t.Cleanup(srv.Close)
	_, port, _ := net.SplitHostPort(strings.TrimPrefix(srv.URL, "http://"))
	const host = "update.invalid" // .invalid never resolves
	work := t.TempDir()

	get := func(d *fallbackDialer) error {
		c := &http.Client{Transport: &http.Transport{DialContext: d.DialContext}}
		resp, err := c.Get("http://" + net.JoinHostPort(host, port) + "/")
		if err == nil {
			resp.Body.Close()
		}
		return err
	}

	d := newFallbackDialer(work)
	d.doh = func(ctx context.Context, h string) ([]string, error) {
		if h != host {
			t.Fatalf("asked DoH for %q", h)
		}
		return []string{"127.0.0.1"}, nil
	}
	if err := get(d); err != nil {
		t.Fatal(err)
	}
	if d.Used() != "doh" {
		t.Fatalf("used %q", d.Used())
	}

	// A connection by name that works is remembered (here: the server by IP is not, by
	// design; remember it by hand as a real by-name connect would).
	d.remember(host, &net.TCPAddr{IP: net.ParseIP("127.0.0.1")})
	d2 := newFallbackDialer(work)
	d2.doh = func(context.Context, string) ([]string, error) { return nil, errors.New("doh blocked") }
	if err := get(d2); err != nil {
		t.Fatal(err)
	}
	if d2.Used() != "cache" {
		t.Fatalf("used %q", d2.Used())
	}

	// Nothing to fall back on: the original DNS error comes back.
	d3 := newFallbackDialer(t.TempDir())
	d3.doh = func(context.Context, string) ([]string, error) { return nil, errors.New("doh blocked") }
	err := get(d3)
	var dnsErr *net.DNSError
	if !errors.As(err, &dnsErr) {
		t.Fatalf("got %v", err)
	}
}

func TestDoHQueryAndAnswer(t *testing.T) {
	var asked string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var p dnsmessage.Parser
		body := make([]byte, 512)
		n, _ := r.Body.Read(body)
		hdr, err := p.Start(body[:n])
		if err != nil {
			t.Fatal(err)
		}
		q, _ := p.Question()
		asked = q.Name.String()
		b := dnsmessage.NewBuilder(nil, dnsmessage.Header{ID: hdr.ID, Response: true})
		b.StartQuestions()
		b.Question(q)
		b.StartAnswers()
		b.AResource(dnsmessage.ResourceHeader{Name: q.Name, Type: dnsmessage.TypeA, Class: dnsmessage.ClassINET, TTL: 60}, dnsmessage.AResource{A: [4]byte{203, 0, 113, 9}})
		out, _ := b.Finish()
		w.Header().Set("Content-Type", "application/dns-message")
		w.Write(out)
	}))
	t.Cleanup(srv.Close)
	old := fallbackDoH
	fallbackDoH = []string{srv.URL}
	t.Cleanup(func() { fallbackDoH = old })
	ips, err := newFallbackDialer(t.TempDir()).askDoH(context.Background(), "api.example.com")
	if err != nil || len(ips) != 1 || ips[0] != "203.0.113.9" || asked != "api.example.com." {
		t.Fatalf("ips %v err %v asked %q", ips, err, asked)
	}
}
