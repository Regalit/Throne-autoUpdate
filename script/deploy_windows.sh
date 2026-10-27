#!/bin/bash
set -e

rm -rf $DEST
mkdir -p $DEST

#### copy exe ####
# Shadowlos: the exe is ShadowLosThrone.exe or ShadowLosThroneDev.exe (cmake/nkr.cmake).
cp $GITHUB_WORKSPACE/build/ShadowLosThrone*.exe $DEST
cp $GITHUB_WORKSPACE/build/ShadowLosThrone*.pdb $DEST || true

source "$(dirname "$0")/extract_core_artifact.sh"
