#!/bin/bash
set -e

rm -rf $DEST
mkdir -p $DEST

#### copy exe ####
# Shadowlos: the exe is ShadowLosThrone.exe or ShadowLosThroneDev.exe (cmake/nkr.cmake).
# The glob needs forward slashes: in Git Bash $GITHUB_WORKSPACE is D:\a\..., whose
# backslashes the glob reads as escapes, so it matched nothing.
BUILD_DIR="$(dirname "$0")/../build"
cp "$BUILD_DIR"/ShadowLosThrone*.exe $DEST
cp "$BUILD_DIR"/ShadowLosThrone*.pdb $DEST || true

source "$(dirname "$0")/extract_core_artifact.sh"
