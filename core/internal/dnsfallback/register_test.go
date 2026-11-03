package dnsfallback_test

import (
	"fmt"
	"strings"
	"testing"

	"ThroneCore/internal/boxmain"
	"ThroneCore/internal/dnsfallback"

	"github.com/sagernet/sing-box/adapter"
	"github.com/sagernet/sing/service"
)

// The config generator's shape for a test: dns-direct over DoH to an IP, dns-local the system resolver.
const boxConfig = `{
  "log": {"disabled": true},
  "dns": {
    "servers": [
      {"type": "https", "tag": "dns-direct", "server": "1.1.1.1", "path": "/dns-query", "domain_resolver": "dns-local"},
      {"type": "https", "tag": "dns-other", "server": "1.1.1.1", "path": "/dns-query"},
      {"type": "local", "tag": "dns-local"}
    ],
    "final": "dns-direct"
  },
  "outbounds": [{"type": "direct", "tag": "direct"}]
}`

// No query is sent: this only checks which transports the registry wraps in a real box.
func TestRegisterWrapsOnlyDirectDoH(t *testing.T) {
	box, cancel, err := boxmain.CreateWithRole(boxmain.RoleTest, []byte(boxConfig), nil)
	if err != nil {
		t.Fatalf("create box: %v", err)
	}
	t.Cleanup(func() {
		_ = box.Close()
		cancel()
	})
	manager := service.FromContext[adapter.DNSTransportManager](box.Context())
	direct, _ := manager.Transport(dnsfallback.DirectTag)
	other, _ := manager.Transport("dns-other")
	if got := fmt.Sprintf("%T", direct); got != "*dnsfallback.fallbackTransport" {
		t.Fatalf("dns-direct is %s", got)
	}
	if direct.Type() != "https" || direct.Tag() != dnsfallback.DirectTag {
		t.Fatalf("wrapper must keep type and tag: %s %s", direct.Type(), direct.Tag())
	}
	if got := fmt.Sprintf("%T", other); got == "*dnsfallback.fallbackTransport" {
		t.Fatal("only dns-direct is wrapped")
	}
}

// GUI direct DNS "local": dns-direct on the system resolver gets the DoH chain behind it; dns-local,
// the DoH chain's own last resort, stays plain.
const systemBoxConfig = `{
  "log": {"disabled": true},
  "dns": {
    "servers": [
      {"type": "local", "tag": "dns-direct", "domain_resolver": "dns-local"},
      {"type": "local", "tag": "dns-local"}
    ],
    "final": "dns-direct"
  },
  "outbounds": [{"type": "direct", "tag": "direct"}]
}`

func TestRegisterWrapsOnlyDirectSystem(t *testing.T) {
	box, cancel, err := boxmain.CreateWithRole(boxmain.RoleTest, []byte(systemBoxConfig), nil)
	if err != nil {
		t.Fatalf("create box: %v", err)
	}
	t.Cleanup(func() {
		_ = box.Close()
		cancel()
	})
	manager := service.FromContext[adapter.DNSTransportManager](box.Context())
	direct, _ := manager.Transport(dnsfallback.DirectTag)
	local, _ := manager.Transport(dnsfallback.LocalTag)
	if got := fmt.Sprintf("%T", direct); got != "*dnsfallback.systemTransport" {
		t.Fatalf("dns-direct is %s", got)
	}
	if direct.Type() != "local" || direct.Tag() != dnsfallback.DirectTag {
		t.Fatalf("wrapper must keep type and tag: %s %s", direct.Type(), direct.Tag())
	}
	if _, ok := direct.(adapter.DNSTransportWithEnvironment); !ok {
		t.Fatal("the system resolver's environment must stay visible to the DNS cache")
	}
	if got := fmt.Sprintf("%T", local); strings.Contains(got, "dnsfallback") {
		t.Fatalf("dns-local must stay plain, is %s", got)
	}
}
