//go:build !debug

package parentcheck

import (
	"runtime"
	"testing"
)

func TestParentIsAcceptedLauncher(t *testing.T) {
	// parentIsAcceptedLauncher splits paths with the host's filepath separator,
	// so Windows-style backslash paths only parse correctly when the test runs
	// on Windows. Cases carry the OS whose path style they use and are skipped
	// elsewhere; the name-matching logic itself is identical across hosts.
	tests := []struct {
		name       string
		parentPath string
		selfPath   string
		goos       string
		pathStyle  string // OS whose path separators the fixture uses
		want       bool
	}{
		// The regression that stranded users on 1.2.1: the GUI is shipped as
		// Shadowlos.exe, and the upstream check only accepted Throne.exe.
		{
			name:       "windows shadowlos launcher",
			parentPath: `C:\Users\user\Desktop\Shadowlos\Shadowlos.exe`,
			selfPath:   `C:\Users\user\Desktop\Shadowlos\ThroneCore.exe`,
			goos:       "windows",
			pathStyle:  "windows",
			want:       true,
		},
		{
			name:       "windows stock throne launcher still accepted",
			parentPath: `C:\Apps\Throne\Throne.exe`,
			selfPath:   `C:\Apps\Throne\ThroneCore.exe`,
			goos:       "windows",
			pathStyle:  "windows",
			want:       true,
		},
		{
			name:       "windows name match is case-insensitive",
			parentPath: `C:\x\shadowlos.EXE`,
			selfPath:   `C:\x\ThroneCore.exe`,
			goos:       "windows",
			pathStyle:  "windows",
			want:       true,
		},
		{
			name:       "windows different directory rejected",
			parentPath: `C:\Temp\Shadowlos.exe`,
			selfPath:   `C:\Users\user\Desktop\Shadowlos\ThroneCore.exe`,
			goos:       "windows",
			pathStyle:  "windows",
			want:       false,
		},
		{
			name:       "windows unrelated parent rejected",
			parentPath: `C:\Users\user\Desktop\Shadowlos\explorer.exe`,
			selfPath:   `C:\Users\user\Desktop\Shadowlos\ThroneCore.exe`,
			goos:       "windows",
			pathStyle:  "windows",
			want:       false,
		},
		{
			name:       "linux throne launcher (its GUI keeps the Throne name)",
			parentPath: "/opt/shadowlos/Throne",
			selfPath:   "/opt/shadowlos/Core",
			goos:       "linux",
			pathStyle:  "unix",
			want:       true,
		},
		{
			name:       "linux shadowlos launcher",
			parentPath: "/opt/shadowlos/Shadowlos",
			selfPath:   "/opt/shadowlos/Core",
			goos:       "linux",
			pathStyle:  "unix",
			want:       true,
		},
		{
			name:       "linux is case-sensitive: throne is not Throne",
			parentPath: "/opt/shadowlos/throne",
			selfPath:   "/opt/shadowlos/Core",
			goos:       "linux",
			pathStyle:  "unix",
			want:       false,
		},
		{
			name:       "linux different directory rejected",
			parentPath: "/tmp/Throne",
			selfPath:   "/opt/shadowlos/Core",
			goos:       "linux",
			pathStyle:  "unix",
			want:       false,
		},
	}

	hostStyle := "unix"
	if runtime.GOOS == "windows" {
		hostStyle = "windows"
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if tt.pathStyle != hostStyle {
				t.Skipf("fixture uses %s path separators; host is %s", tt.pathStyle, hostStyle)
			}
			if got := parentIsAcceptedLauncher(tt.parentPath, tt.selfPath, tt.goos); got != tt.want {
				t.Errorf("parentIsAcceptedLauncher(%q, %q, %q) = %v, want %v",
					tt.parentPath, tt.selfPath, tt.goos, got, tt.want)
			}
		})
	}
}
