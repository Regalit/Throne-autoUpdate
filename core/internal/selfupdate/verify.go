package selfupdate

import (
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"os"
	"regexp"
)

// publicKey verifies update packages. The private half never leaves the
// release machine and the root-only backup on the server; see FORK.md.
var publicKey = mustKey("C11WG4t6CW8+UqfFefQJOnp3W2CdoDxWZH4ybp8kQbw=")

func mustKey(b64 string) ed25519.PublicKey {
	raw, err := base64.StdEncoding.DecodeString(b64)
	if err != nil || len(raw) != ed25519.PublicKeySize {
		panic("bad update public key")
	}
	return ed25519.PublicKey(raw)
}

var sha256Hex = regexp.MustCompile(`^[0-9a-f]{64}$`)

// Release is latest.json as served by /api/desktop/app[/dev]/latest.
type Release struct {
	AppID            string `json:"app_id"`
	Platform         string `json:"platform"`
	Version          string `json:"version"`
	VersionCode      int    `json:"version_code"`
	SHA256           string `json:"sha256"`
	Size             int64  `json:"size"`
	Sig              string `json:"sig"`
	MinSupportedCode int    `json:"min_supported_code"`
	Notes            string `json:"notes"`
}

const maxPackageSize = 300 << 20

// validate checks everything that can be checked before the download,
// including the signature over the announced sha256.
func (r *Release) validate(appID string, key ed25519.PublicKey) error {
	switch {
	case r.AppID != appID:
		return fmt.Errorf("app_id %q, want %q", r.AppID, appID)
	case r.Platform != "windows":
		return fmt.Errorf("platform %q, want windows", r.Platform)
	case r.VersionCode <= 0 || r.Version == "":
		return errors.New("no version")
	case !sha256Hex.MatchString(r.SHA256):
		return errors.New("bad sha256")
	case r.Size <= 0 || r.Size > maxPackageSize:
		return fmt.Errorf("bad size %d", r.Size)
	}
	sig, err := base64.StdEncoding.DecodeString(r.Sig)
	if err != nil || !ed25519.Verify(key, []byte(r.SHA256), sig) {
		return errors.New("bad signature")
	}
	return nil
}

// fileMatches reports whether path has exactly the given size and sha256.
func fileMatches(path string, size int64, sum string) (bool, error) {
	f, err := os.Open(path)
	if err != nil {
		return false, err
	}
	defer f.Close()
	h := sha256.New()
	n, err := io.Copy(h, f)
	if err != nil {
		return false, err
	}
	return n == size && hex.EncodeToString(h.Sum(nil)) == sum, nil
}
