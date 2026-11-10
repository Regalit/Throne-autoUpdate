package rpc

import (
	"context"
	"errors"
	"time"

	"ThroneCore/gen"
	"ThroneCore/internal/probe"
)

func (s *server) Test(ctx context.Context, in *gen.TestReq) (*gen.TestResp, error) {
	env, err := prepareTestEnv(in.GetTestCurrent(), in.GetNeedXray(), in.GetXrayConfig(),
		in.XrayFullConfigs, in.GetConfig(), in.OutboundTags, in.GetUseDefaultOutbound(),
		in.GetXrayOutboundDnsStrategy())
	if err != nil {
		if errors.Is(err, errInstanceNotRunning) {
			return &gen.TestResp{Results: []*gen.URLTestResp{{
				OutboundTag: To("proxy"),
				LatencyMs:   To(int32(0)),
				Error:       To(err.Error()),
			}}}, nil
		}
		return nil, err
	}
	defer env.close()

	// Held, not re-read: StopTest rearms a fresh context, uncancelled.
	testCtx := probe.TestContext()

	// A muxed config needs a warm connection; the live instance already is one.
	twice := !in.GetTestCurrent()
	results := probe.BatchURLTest(testCtx, env.box, env.tags, in.GetUrl(),
		int(in.GetMaxConcurrency()), twice, time.Duration(in.GetTestTimeoutMs())*time.Millisecond, in.GetFullCheck())

	res := make([]*gen.URLTestResp, 0, len(results))
	failed := make(map[string]bool, len(results))
	for idx, data := range results {
		errStr := ""
		if data.Error != nil {
			errStr = data.Error.Error()
		}
		failed[env.tags[idx]] = errStr != ""
		res = append(res, withCheck(&gen.URLTestResp{
			OutboundTag: To(env.tags[idx]),
			LatencyMs:   To(int32(data.Duration.Milliseconds())),
			Error:       To(errStr),
		}, data.Check))
	}

	out := &gen.TestResp{Results: res}
	// `defer env.close()` tears the box down on return, so this cannot wait.
	var pending []string
	for _, tag := range in.VpnEndpointTags {
		if failed[tag] {
			pending = append(pending, tag)
		}
	}
	if len(pending) > 0 {
		// A snapshot: the probe already sat out the handshake, so a tunnel that settles only now never carried it.
		out.VpnStatus = collectVPNStatus(testCtx, env.box, pending, 0)
	}
	return out, nil
}

func withCheck(resp *gen.URLTestResp, check *probe.ServerCheck) *gen.URLTestResp {
	if check != nil {
		resp.Check = To(check.State)
		resp.FailedServices = check.Failed
		resp.BulkBytes = To(int32(check.BulkBytes))
		resp.BulkMs = To(int32(check.BulkMs))
	}
	return resp
}

func (s *server) StopTest(ctx context.Context, in *gen.EmptyReq) (*gen.EmptyResp, error) {
	probe.CancelTests()

	return &gen.EmptyResp{}, nil
}

func (s *server) QueryURLTest(ctx context.Context, in *gen.EmptyReq) (out *gen.QueryURLTestResponse, _ error) {
	results := probe.URLReporter.Results()
	out = &gen.QueryURLTestResponse{}
	for _, r := range results {
		errStr := ""
		if r.Error != nil {
			errStr = r.Error.Error()
		}
		out.Results = append(out.Results, withCheck(&gen.URLTestResp{
			OutboundTag: To(r.Tag),
			LatencyMs:   To(int32(r.Duration.Milliseconds())),
			Error:       To(errStr),
		}, r.Check))
	}
	return
}

func (s *server) IPTest(ctx context.Context, in *gen.IPTestRequest) (*gen.IPTestResp, error) {
	// Always builds its own box: there is no test-current variant of an IP test.
	const current = false
	env, err := prepareTestEnv(current, in.GetNeedXray(), in.GetXrayConfig(),
		in.XrayFullConfigs, in.GetConfig(), in.OutboundTags, in.GetUseDefaultOutbound(),
		in.GetXrayOutboundDnsStrategy())
	if err != nil {
		return nil, err
	}
	defer env.close()

	timeout := time.Duration(in.GetTestTimeoutMs()) * time.Millisecond
	results := probe.BatchIPTest(probe.TestContext(), env.box, env.tags,
		int(in.GetMaxConcurrency()), !current, timeout)

	res := make([]*gen.IPTestRes, 0, len(results))
	for idx, data := range results {
		errStr := ""
		if data.Error != nil {
			errStr = data.Error.Error()
		}
		res = append(res, &gen.IPTestRes{
			OutboundTag: To(env.tags[idx]),
			Ip:          To(data.Result.IP),
			CountryCode: To(data.Result.CountryCode),
			Error:       To(errStr),
		})
	}
	return &gen.IPTestResp{Results: res}, nil
}

func (s *server) QueryIPTest(ctx context.Context, in *gen.EmptyReq) (out *gen.QueryIPTestResponse, _ error) {
	results := probe.IPReporter.Results()
	out = &gen.QueryIPTestResponse{}
	for _, r := range results {
		errStr := ""
		if r.Error != nil {
			errStr = r.Error.Error()
		}
		out.Results = append(out.Results, &gen.IPTestRes{
			OutboundTag: To(r.Tag),
			Ip:          To(r.Result.IP),
			CountryCode: To(r.Result.CountryCode),
			Error:       To(errStr),
		})
	}
	return
}
