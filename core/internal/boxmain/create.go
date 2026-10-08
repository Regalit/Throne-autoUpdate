package boxmain

import (
	"context"
	"fmt"
	"os"
	"os/signal"
	"sync/atomic"
	"syscall"
	"time"

	"ThroneCore/internal/boxbox"

	C "github.com/sagernet/sing-box/constant"
	"github.com/sagernet/sing-box/log"
	"github.com/sagernet/sing-box/option"
	E "github.com/sagernet/sing/common/exceptions"
	"github.com/sagernet/sing/common/json"
)

func parseConfig(ctx context.Context, configContent []byte) (*option.Options, error) {
	options, err := json.UnmarshalExtendedContext[option.Options](ctx, configContent)
	if err != nil {
		return nil, E.Cause(err, "decode config at ", string(configContent))
	}
	return &options, nil
}

// createOnce builds and starts the box exactly as given. Create wraps it with the rule-set fallback.
//
// onCreated runs between New and Start: the Xray sidecars start first and resolve through this box.
func createOnce(configContent []byte, onCreated func(*boxbox.Box), adjust ...func(*option.Options)) (*boxbox.Box, context.CancelFunc, error) {
	// Fresh context per call: concurrent boxes sharing one service.Registry clobber each other's OutboundManager.
	ctx := newBoxContext()
	options, err := parseConfig(ctx, configContent)
	if err != nil {
		return nil, nil, err
	}
	if disableColor {
		if options.Log == nil {
			options.Log = &option.LogOptions{}
		}
		options.Log.DisableColor = true
	}
	for _, fn := range adjust {
		fn(options)
	}
	ctx, cancel := context.WithCancel(ctx)
	instance, err := boxbox.New(boxbox.Options{
		Context: ctx,
		Options: *options,
	})
	if err != nil {
		cancel()
		return nil, nil, E.Cause(err, "create service")
	}
	if onCreated != nil {
		onCreated(instance)
	}

	osSignals := make(chan os.Signal, 1)
	signal.Notify(osSignals, os.Interrupt, syscall.SIGTERM, syscall.SIGHUP)
	defer func() {
		signal.Stop(osSignals)
		close(osSignals)
	}()
	startCtx, finishStart := context.WithCancel(context.Background())
	go func() {
		_, loaded := <-osSignals
		if loaded {
			cancel()
			closeMonitor(startCtx)
		}
	}()
	// A remote rule set that connects but never answers would hold the start forever and
	// the rule-set fallback in Create would never get its turn. Cancelling the context
	// aborts the pending downloads, which then fail as rule sets and take that path.
	var watchdog *time.Timer
	var overBudget atomic.Bool
	if hasRemoteRuleSets(configContent) {
		watchdog = time.AfterFunc(ruleSetStartBudget, func() {
			overBudget.Store(true)
			cancel()
		})
	}
	err = instance.Start()
	if watchdog != nil {
		watchdog.Stop()
	}
	finishStart()
	if err != nil && overBudget.Load() {
		// sing-box reports the abort as a bare "context canceled"; name the cause.
		err = fmt.Errorf("%w (%s): %v", errRuleSetBudget, ruleSetStartBudget, err)
	}
	if err != nil {
		cancel()
		return nil, nil, E.Cause(err, "start service")
	}
	return instance, cancel, nil
}

func closeMonitor(ctx context.Context) {
	time.Sleep(C.FatalStopTimeout)
	select {
	case <-ctx.Done():
		return
	default:
	}
	log.Fatal("sing-box did not close!")
}
