// Package selfupdate is the ShadowLos Throne self-updater. It lives in ThroneCore
// (ThroneCore.exe selfupdate ...) rather than a separate updater.exe, which would
// add ~3MB to an archive that has to stay under Telegram's 50MB.
//
//	ThroneCore selfupdate check -base URL -appid ID -code N -work DIR
//	    Fetches BASE/latest?platform=windows, and when it names a newer build,
//	    downloads BASE/download?platform=windows into WORK and verifies its size,
//	    sha256 and ed25519 signature. While it downloads it prints progress lines,
//	    {"state":"downloading","version":V,"done":N,"size":M}, at most every 250 ms;
//	    the last line is the result: {"state":"none"|"ready"|"error",...}, an error
//	    with "kind" network, server, verify or disk.
//	ThroneCore selfupdate apply -dir APPDIR -src DIR -exe NAME -appid ID -pid PID [-deelevate]
//	    Waits for PID to exit and stops anything else running from APPDIR, copies the
//	    package from SRC into APPDIR/update and verifies that copy, moves the files it
//	    replaces into APPDIR/update/previous (journal: apply.json), puts the new ones in
//	    place, leaves SRC/pending and starts NAME. When the new build does not write
//	    SRC/ok once its core is up, the old files come back and the old build starts;
//	    after two such failures that version is skipped. An apply that died half-way is
//	    undone by the next one. Runs from APPDIR's own ThroneCore.exe (Windows lets a
//	    running image be renamed); -deelevate starts the app as the user after an
//	    elevated apply.
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
