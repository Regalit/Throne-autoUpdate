// Package selfupdate is the ShadowLos Throne self-updater. It lives in ThroneCore
// (ThroneCore.exe selfupdate ...) rather than a separate updater.exe, which would
// add ~3MB to an archive that has to stay under Telegram's 50MB.
//
//	ThroneCore selfupdate check -base URL -appid ID -code N -work DIR
//	    Fetches BASE/latest?platform=windows, and when it names a newer build,
//	    downloads BASE/download?platform=windows into WORK and verifies its size,
//	    sha256 and ed25519 signature. Prints one JSON line: {"state":"none"|"ready"|"error",...}.
//	ThroneCore selfupdate apply -dir APPDIR -work DIR -exe NAME -pid PID
//	    Waits for PID to exit, re-verifies the package, moves the files it replaces
//	    into WORK/previous, puts the new ones in place and starts NAME --after-update.
//	    When the new build does not write WORK/ok within the timeout, it restores
//	    the old files, marks that version bad and starts the old build again.
//	    Run it from a copy outside the app dir's files: it replaces ThroneCore.exe too.
//
// It never touches config/ (servers, routing, settings): a package that carries
// anything under config/ or update/ is refused.
package selfupdate

import (
	"fmt"
	"os"
)

// Main runs a selfupdate command and exits.
func Main(args []string) {
	if len(args) < 1 {
		fmt.Fprintln(os.Stderr, "usage: ThroneCore selfupdate check|apply [flags]")
		os.Exit(2)
	}
	var err error
	switch args[0] {
	case "check":
		err = runCheck(args[1:])
	case "apply":
		err = runApply(args[1:])
	default:
		err = fmt.Errorf("unknown command %q", args[0])
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	os.Exit(0)
}
