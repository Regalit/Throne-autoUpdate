#!/bin/bash
set -e

# Shadowlos: no with_tailscale/with_openvpn/with_openconnect. Our users only run
# our VLESS subscription, and those three add ~7MB to the zipped core, which
# pushed the Windows archive past Telegram's 50MB bot upload limit.
TAGS="with_clash_api,with_gvisor,with_quic,with_wireguard,with_utls,with_dhcp,badlinkname,tfogo_checklinkname0"

rm -rf $DEST
mkdir -p $DEST

[[ "$GOOS" =~ legacy$ ]] && IS_LEGACY=true && GOCMD="$PWD/golang.org/go/bin/go" && GOOS="${GOOS%legacy}" || { IS_LEGACY=false; GOCMD="go"; }

if [[ "$GOOS" == "windows" || "$GOOS" == "linux" ]]; then
    FILE=$([[ "$GOOS" == "windows" ]] && echo "updater-windows-x${GOARCH: -2}.exe" || echo "updater-linux-$GOARCH")
    curl -fLso "$DEST/updater$([[ "$GOOS" == "windows" ]] && echo ".exe")" "https://github.com/throneproj/updater/releases/latest/download/$FILE"
    [[ "$GOOS" == "linux" ]] && chmod +x "$DEST/updater"
fi

case "$GOOS" in
  windows)
    export CGO_ENABLED=0
    # Shadowlos: no NaiveProxy, so no 9MB libcronet.dll in the archive.
    ;;
  darwin)
    TAGS+=",with_naive_outbound"
    export CGO_ENABLED=1 CGO_LDFLAGS="-weak_framework UniformTypeIdentifiers"
    # cgo otherwise builds for the runner's SDK and hard-links every API newer than 10.15
    if $IS_LEGACY; then
      export MACOSX_DEPLOYMENT_TARGET=10.15
    fi
    ;;
  linux)
    # Shadowlos: no NaiveProxy (it statically links Chromium's network stack).
    export CGO_ENABLED=1
    ;;
esac

#### Go: core ####
pushd core
pushd gen
protoc -I . --go_out=. --go-grpc_out=. libcore.proto
popd
if false; then # Shadowlos: NaiveProxy is not built, so libcronet.dll is not needed
  # The lib module ships the DLL, so it is always the binding's generation.
  CRONET_LIB=github.com/sagernet/cronet-go/lib/windows_$GOARCH
  go mod download $CRONET_LIB
  install -m 644 "$(go list -m -f '{{.Dir}}' $CRONET_LIB)/libcronet.dll" $DEST/
fi
if [[ "$GOOS" == "darwin" ]]; then
  # cgo cannot detect a binding/lib ABI skew, so compare the headers the darwin lib was built against.
  CRONET_LIB=github.com/sagernet/cronet-go/lib/darwin_$GOARCH
  go mod download github.com/sagernet/cronet-go $CRONET_LIB
  CRONET_LIB_INCLUDE="$(go list -m -f '{{.Dir}}' $CRONET_LIB)/include"
  for header in "$(go list -m -f '{{.Dir}}' github.com/sagernet/cronet-go)"/include/*.h; do
    cmp -s "$header" "$CRONET_LIB_INCLUDE/${header##*/}" || { echo "cronet-go binding and $CRONET_LIB differ in ${header##*/}" >&2; exit 1; }
  done
fi
VERSION_SINGBOX=$(go list -m -f '{{.Version}}' github.com/sagernet/sing-box)
$GOCMD build -v -o $DEST -trimpath -ldflags "-w -s -X 'github.com/sagernet/sing-box/constant.Version=${VERSION_SINGBOX}' -X 'internal/godebug.defaultGODEBUG=multipathtcp=0' -checklinkname=0" -tags "$TAGS"
popd
