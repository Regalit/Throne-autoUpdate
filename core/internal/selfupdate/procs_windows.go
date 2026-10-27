package selfupdate

import (
	"os"
	"path/filepath"
	"strings"
	"unsafe"

	"golang.org/x/sys/windows"
)

// dirProcesses returns the pids of processes (other than this one) whose
// image lives in dir or below it.
func dirProcesses(dir string) []uint32 {
	prefix := strings.ToLower(filepath.Clean(dir)) + `\`
	snap, err := windows.CreateToolhelp32Snapshot(windows.TH32CS_SNAPPROCESS, 0)
	if err != nil {
		return nil
	}
	defer windows.CloseHandle(snap)
	self := uint32(os.Getpid())
	var pids []uint32
	var e windows.ProcessEntry32
	e.Size = uint32(unsafe.Sizeof(e))
	for err = windows.Process32First(snap, &e); err == nil; err = windows.Process32Next(snap, &e) {
		if e.ProcessID == self || e.ProcessID == 0 {
			continue
		}
		h, err := windows.OpenProcess(windows.PROCESS_QUERY_LIMITED_INFORMATION, false, e.ProcessID)
		if err != nil {
			continue
		}
		buf := make([]uint16, windows.MAX_LONG_PATH)
		n := uint32(len(buf))
		if windows.QueryFullProcessImageName(h, 0, &buf[0], &n) == nil {
			if strings.HasPrefix(strings.ToLower(windows.UTF16ToString(buf[:n])), prefix) {
				pids = append(pids, e.ProcessID)
			}
		}
		windows.CloseHandle(h)
	}
	return pids
}

func anyDirProcess(dir string) bool {
	return len(dirProcesses(dir)) > 0
}

// killDirProcesses ends every other process running from dir and waits for
// each to be gone, so its files can be moved.
func killDirProcesses(dir string) {
	for _, pid := range dirProcesses(dir) {
		h, err := windows.OpenProcess(windows.PROCESS_TERMINATE|windows.SYNCHRONIZE, false, pid)
		if err != nil {
			continue
		}
		_ = windows.TerminateProcess(h, 1)
		_, _ = windows.WaitForSingleObject(h, 10000)
		windows.CloseHandle(h)
	}
}
