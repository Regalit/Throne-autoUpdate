//go:build !debug && !noparentcheck

package parentcheck

import (
	"log"
	"os"
	"path/filepath"
	"runtime"
	"strings"
)

func CheckParentProcess() {
	parentPath, err := getParentExePath(ParentPID)
	if err != nil {
		log.Fatalf("parent check: cannot read parent executable: %v", err)
	}
	parentPath = resolveFinalPath(parentPath)

	selfPath, err := os.Executable()
	if err != nil {
		log.Fatalf("parent check: cannot read own executable: %v", err)
	}
	selfPath = resolveFinalPath(selfPath)

	if !parentIsAcceptedLauncher(parentPath, selfPath, runtime.GOOS) {
		log.Fatalf("parent check failed: unexpected parent %q, selfPath is %q", parentPath, selfPath)
	}
}

// parentIsAcceptedLauncher reports whether parentPath is a launcher the core is
// willing to run under: it must sit in the same directory as the core and be one
// of the accepted GUI names. Shadowlos ships the GUI renamed (Throne.exe ->
// Shadowlos.exe up to 1.3.2-ui, ShadowLosThrone.exe / ShadowLosThroneDev.exe
// from 1.0.0 on Windows), so all of them are allowed.
func parentIsAcceptedLauncher(parentPath, selfPath, goos string) bool {
	selfDir := filepath.Dir(selfPath)
	parentDir := filepath.Dir(parentPath)
	parentBase := filepath.Base(parentPath)

	if goos == "windows" {
		if !strings.EqualFold(parentDir, selfDir) {
			return false
		}
		for _, name := range []string{"Throne.exe", "Shadowlos.exe", "ShadowLosThrone.exe", "ShadowLosThroneDev.exe"} {
			if strings.EqualFold(parentBase, name) {
				return true
			}
		}
		return false
	}

	if parentDir != selfDir {
		return false
	}
	return parentBase == "Throne" || parentBase == "Shadowlos"
}
