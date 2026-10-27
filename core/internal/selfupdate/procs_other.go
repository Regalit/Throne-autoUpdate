//go:build !windows

package selfupdate

// The self-updater is Windows only; elsewhere these only keep the package building.

func anyDirProcess(dir string) bool { return false }

func killDirProcesses(dir string) {}
