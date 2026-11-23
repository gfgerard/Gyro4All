#!/usr/bin/env bash
# Fills third_party/ with what the build needs (git-ignored, never committed): the pinned GyroLib SDK (release zip,
# SHA256-checked against tools/gyrolib-sdk.sh), MinHook, ImGui (backends of GyroLib's ImGui tag) and Vulkan-Headers.
# Anything already there is kept. Used by the GitHub Actions workflows and on fresh clones.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tools/gyrolib-sdk.sh"
TP="$ROOT/third_party"
mkdir -p "$TP"

clone() {  # url, tag, folder
    if [ -d "$TP/$3" ]; then echo "have $3"; return; fi
    git -c advice.detachedHead=false clone -q --depth 1 --branch "$2" "$1" "$TP/$3"
    echo "cloned $3"
}
clone https://github.com/TsudaKageyu/minhook v1.3.4 minhook-1.3.4
clone https://github.com/ocornut/imgui v1.92.9b imgui-1.92.9b
clone https://github.com/KhronosGroup/Vulkan-Headers v1.4.309 Vulkan-Headers-1.4.309

if [ -f "$GYROLIB_SDK/bin/gyrolib.dll" ]; then echo "have GyroLib $GYROLIB_VERSION SDK"; exit 0; fi
ZIP_NAME="GyroLib-$GYROLIB_VERSION-sdk-windows-x64.zip"
DL="$TP/GyroLib-$GYROLIB_VERSION/_dl"
mkdir -p "$DL"
[ -f "$DL/$ZIP_NAME" ] ||
    curl -fsSL -o "$DL/$ZIP_NAME" "https://github.com/lud-berthe/GyroLib/releases/download/v$GYROLIB_VERSION/$ZIP_NAME"
# Python for the hash and the unzip: Git Bash on the runners has no guaranteed sha256sum/unzip.
python -I - "$(cygpath -w "$DL/$ZIP_NAME")" "$GYROLIB_SDK_SHA256" "$(cygpath -w "$TP/GyroLib-$GYROLIB_VERSION/sdk")" <<'PY'
import hashlib, sys, zipfile
zip_path, want, dest = sys.argv[1], sys.argv[2], sys.argv[3]
got = hashlib.sha256(open(zip_path, "rb").read()).hexdigest()
if got != want:
    sys.exit(f"{zip_path}: SHA256 {got}, expected {want} (tools/gyrolib-sdk.sh)")
zipfile.ZipFile(zip_path).extractall(dest)
PY
[ -f "$GYROLIB_SDK/bin/gyrolib.dll" ] || { echo "the SDK zip did not unpack to $GYROLIB_SDK"; exit 1; }
echo "fetched GyroLib $GYROLIB_VERSION SDK"
