package boxmain

import (
	"errors"
	"fmt"
	"os"
	"syscall"
	"testing"
)

func TestIsCacheFileBusy(t *testing.T) {
	sharing := &os.PathError{Op: "open", Path: `C:\x\config\cache.db`, Err: errorSharingViolation}
	cases := []struct {
		name string
		err  error
		want bool
	}{
		{"nil", nil, false},
		{"sharing violation", fmt.Errorf("start service: initialize cache-file: %w", sharing), true},
		{"journal text only", errors.New("start service: initialize cache-file: open C:\\x\\config\\cache.db: The process cannot access the file because it is being used by another process."), true},
		{"lock violation", fmt.Errorf("start service: initialize cache-file: %w", &os.PathError{Op: "open", Path: "cache.db", Err: errorLockViolation}), true},
		// A sharing violation elsewhere (the TUN driver, a rule-set file) is not this case.
		{"other file", fmt.Errorf("start service: initialize rule-set[0]: %w", sharing), false},
		{"cache-file denied", fmt.Errorf("start service: initialize cache-file: %w", &os.PathError{Op: "open", Path: "cache.db", Err: syscall.Errno(5)}), false},
	}
	for _, tc := range cases {
		if got := IsCacheFileBusy(tc.err); got != tc.want {
			t.Errorf("%s: got %v, want %v", tc.name, got, tc.want)
		}
	}
}
