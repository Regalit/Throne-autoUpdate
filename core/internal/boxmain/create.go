package boxmain

import (
	"context"
	"errors"
	"os"
	"os/signal"
	"strings"
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

// Roles tag a box's dns-direct errors in the log, so a journal line says which config failed.
const (
	RoleProfile = "profile"
	RoleTest    = "test"
)

// onCreated runs between New and Start: the Xray sidecars start first and resolve through this box.
func Create(configContent []byte, onCreated func(*boxbox.Box), adjust ...func(*option.Options)) (*boxbox.Box, context.CancelFunc, error) {
	return CreateWithRole(RoleProfile, configContent, onCreated, adjust...)
}

// CreateWithRole is Create for a box that is not the running profile (a latency or speed test).
func CreateWithRole(role string, configContent []byte, onCreated func(*boxbox.Box), adjust ...func(*option.Options)) (*boxbox.Box, context.CancelFunc, error) {
	deadline := time.Now().Add(cacheFileBusyWait)
	for {
		instance, cancel, err := createOnce(role, configContent, onCreated, adjust...)
		if err == nil || !IsCacheFileBusy(err) || time.Now().Add(cacheFileBusyRetry).After(deadline) {
			return instance, cancel, err
		}
		log.Warn("cache file still in use, starting again in ", cacheFileBusyRetry, ": ", err)
		time.Sleep(cacheFileBusyRetry)
	}
}

// A server switch stops the old box and starts the new one 5 ms later. Stop returns only once the old box
// has closed cache.db, yet Windows can still refuse the open for a moment ("being used by another
// process"): a scanner or indexer opens the file the instant it is released. The start fails before
// anything else is up, so it is simply tried again for a short while.
const (
	cacheFileBusyWait  = 3 * time.Second
	cacheFileBusyRetry = 100 * time.Millisecond
)

// ERROR_SHARING_VIOLATION and ERROR_LOCK_VIOLATION.
const (
	errorSharingViolation = syscall.Errno(32)
	errorLockViolation    = syscall.Errno(33)
)

// IsCacheFileBusy reports whether a start failed only because cache.db was held by someone else.
func IsCacheFileBusy(err error) bool {
	if err == nil {
		return false
	}
	text := err.Error()
	if !strings.Contains(text, "cache-file") {
		return false
	}
	var errno syscall.Errno
	if errors.As(err, &errno) && (errno == errorSharingViolation || errno == errorLockViolation) {
		return true
	}
	// The errno is lost where a layer flattens the error into text.
	return strings.Contains(text, "being used by another process") ||
		strings.Contains(text, "another process has locked a portion of the file")
}

func createOnce(role string, configContent []byte, onCreated func(*boxbox.Box), adjust ...func(*option.Options)) (*boxbox.Box, context.CancelFunc, error) {
	// Fresh context per call: concurrent boxes sharing one service.Registry clobber each other's OutboundManager.
	ctx := newBoxContext(role)
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
	err = instance.Start()
	finishStart()
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
