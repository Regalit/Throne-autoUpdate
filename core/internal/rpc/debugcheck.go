package rpc

import (
	"context"
	"time"

	"ThroneCore/gen"
	"ThroneCore/internal/probe"
)

// DebugCheck is Shadowlos's per-profile diagnosis: IP change, a 1MB TCP
// download and a UDP DNS query, all through one outbound of a test box.
func (s *server) DebugCheck(ctx context.Context, in *gen.DebugCheckRequest) (*gen.DebugCheckResult, error) {
	res := &gen.DebugCheckResult{
		ProfileName: To(in.GetProfileName()),
		RealIp:      To(""),
		ProxyIp:     To(""),
		IpChanged:   To(false),
		TcpOk:       To(false),
		TcpBytes:    To(int64(0)),
		UdpOk:       To(false),
		Error:       To(""),
		TcpError:    To(""),
		UdpError:    To(""),
	}

	var tags []string
	if tag := in.GetOutboundTag(); tag != "" {
		tags = []string{tag}
	}
	env, err := prepareTestEnv(false, in.GetNeedXray(), in.GetXrayConfig(), in.XrayFullConfigs,
		in.GetConfig(), tags, in.GetUseDefaultOutbound(), in.GetXrayOutboundDnsStrategy())
	if err != nil {
		res.Error = To(err.Error())
		return res, nil
	}
	defer env.close()

	timeout := time.Duration(in.GetTimeoutMs()) * time.Millisecond
	r := probe.RunDebugCheck(probe.TestContext(), env.box, in.GetOutboundTag(), in.GetUseDefaultOutbound(), timeout)

	res.RealIp = To(r.RealIP)
	res.ProxyIp = To(r.ProxyIP)
	res.IpChanged = To(r.IPChanged)
	res.TcpOk = To(r.TCPOk)
	res.TcpBytes = To(r.TCPBytes)
	res.UdpOk = To(r.UDPOk)
	if r.Error != nil {
		res.Error = To(r.Error.Error())
	}
	if r.TCPError != nil {
		res.TcpError = To(r.TCPError.Error())
	}
	if r.UDPError != nil {
		res.UdpError = To(r.UDPError.Error())
	}
	return res, nil
}
