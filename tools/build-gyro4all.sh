#!/usr/bin/env bash
# Configures (if needed) and builds Gyro4All (winmm.dll) against the pinned GyroLib SDK.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tools/gyrolib-sdk.sh"
# CMake: $CMAKE if set, else the one on PATH, else the copy bundled with the latest Visual Studio (via vswhere).
if [ -z "${CMAKE:-}" ]; then
  if command -v cmake >/dev/null; then CMAKE=cmake
  else
    VSWHERE="$(cygpath -u "$(printenv 'ProgramFiles(x86)')")/Microsoft Visual Studio/Installer/vswhere.exe"
    VSDIR="$("$VSWHERE" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
             -property installationPath | tr -d '\r')"
    [ -n "$VSDIR" ] || { echo "no Visual Studio with the MSVC x64 tools found (or set CMAKE=<path to cmake>)"; exit 1; }
    CMAKE="$(cygpath -u "$VSDIR")/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
  fi
fi
B="$ROOT/build/Gyro4All"
SRC="$ROOT/src"
# A new GyroLib, or a cache configured from another source folder (an older layout, or a copied checkout): reconfigure.
if ! grep -q "GyroLib-$GYROLIB_VERSION" "$B/CMakeCache.txt" 2>/dev/null ||
   ! grep -qiF "CMAKE_HOME_DIRECTORY:INTERNAL=$(cygpath -m "$SRC")" "$B/CMakeCache.txt" 2>/dev/null; then rm -rf "$B"; fi
"$CMAKE" -S "$SRC" -B "$B" -A x64 "-DCMAKE_PREFIX_PATH=$(cygpath -w "$GYROLIB_SDK")" >/dev/null
"$CMAKE" --build "$B" --config Release 2>&1 | grep -E "error|warning C|-> " | sed -E 's/\[C:.*\]//'
