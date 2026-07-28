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
// Shadowlos.exe on Windows), so both names are allowed.
func parentIsAcceptedLauncher(parentPath, selfPath, goos string) bool {
	selfDir := filepath.Dir(selfPath)
	parentDir := filepath.Dir(parentPath)
	parentBase := filepath.Base(parentPath)

	if goos == "windows" {
		if !strings.EqualFold(parentDir, selfDir) {
			return false
		}
		return strings.EqualFold(parentBase, "Throne.exe") || strings.EqualFold(parentBase, "Shadowlos.exe")
	}

	if parentDir != selfDir {
		return false
	}
	return parentBase == "Throne" || parentBase == "Shadowlos"
}
