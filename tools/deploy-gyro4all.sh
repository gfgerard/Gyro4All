#!/usr/bin/env bash
# Generic Gyro4All installer for any UE4/UE5 game.
#   bash tools/deploy-gyro4all.sh "<game>/<Project>/Binaries/Win64"          install / update
#   bash tools/deploy-gyro4all.sh "<game>/<Project>/Binaries/Win64" remove   uninstall
#   PROXY=version.dll bash tools/deploy-gyro4all.sh ...                      install under the fallback proxy name
#   EXE=Titanfall2.exe bash tools/deploy-gyro4all.sh "<game>/bin/x64_retail"  a folder without the exe (games that load
#                                                                            their DLLs from a subfolder: Titanfall 2)
# Copies winmm.dll (or $PROXY: the same DLL renamed, see src/core/proxy.cpp) + Gyro4All/gyrolib.dll, Gyro4All/gyro4all.dll and the
# matching profile (profiles/<exe stem>.ini) if any. Use version.dll when winmm.dll doesn't load (another
# program loads the system winmm.dll first, e.g. RivaTuner in Gears of War: E-Day) or another mod already uses it.
# gyrolib.ini (user settings) is never overwritten.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WIN64="${1:?usage: deploy-gyro4all.sh <Win64 dir> [remove]}"
source "$ROOT/tools/gyrolib-sdk.sh"; SDK="$GYROLIB_SDK"
PROXY="${PROXY:-winmm.dll}"
case "${PROXY,,}" in version.dll|winmm.dll) ;; *) echo "PROXY must be winmm.dll or version.dll"; exit 1;; esac
# The game exe: $EXE, else <Project>-Win64-Shipping.exe, else the largest exe (renamed builds, e.g. Xbox/GDK GoWEDay.exe).
[ -n "${EXE:-}" ] || EXE="$(ls "$WIN64" | grep -i -E -- '-Win64-(Shipping|Test)\.exe$' | head -1 || true)"
[ -n "$EXE" ] || EXE="$(ls -S "$WIN64" | grep -i -E '\.exe$' | head -1 || true)"
[ -n "$EXE" ] || { echo "no game exe in $WIN64"; exit 1; }
STEM="${EXE%.exe}"
# Our proxy: Gyro4All, or GyroUE (its former name), by the version line in the DLL.
ours() { grep -aqsE '(Gyro4All|GyroUE) [0-9]+\.[0-9]+\.[0-9]+ in ' "$1"; }

if [ "${2:-}" = "remove" ]; then
  for p in winmm.dll version.dll; do
    if ours "$WIN64/$p"; then rm -f "$WIN64/$p"; fi
  done
  rm -rf "$WIN64/Gyro4All"; echo "Gyro4All removed from $WIN64"
  for d in GyroUE GyroUE.migrated; do [ -d "$WIN64/$d" ] && echo "kept $WIN64/$d (GyroUE settings; delete it by hand if unwanted)"; done
  exit 0
fi
if [ -e "$WIN64/$PROXY" ] && ! ours "$WIN64/$PROXY"; then
  echo "a foreign $PROXY already exists in $WIN64 (another mod?); not overwriting"; exit 1
fi
# Only one proxy of ours per folder (Gyro4All or an old GyroUE): drop the one under the other name.
for p in winmm.dll version.dll; do
  if [ "${p,,}" != "${PROXY,,}" ] && ours "$WIN64/$p"; then rm -f "$WIN64/$p"; echo "removed old $p"; fi
done
# An old GyroUE\ folder is migrated by Gyro4All itself on the first start (settings and profiles copied, folder renamed).
mkdir -p "$WIN64/Gyro4All/profiles"
cp "$ROOT/build/Gyro4All/Release/winmm.dll" "$WIN64/$PROXY"
cp "$SDK/bin/gyrolib.dll" "$WIN64/Gyro4All/gyrolib.dll"
cp "$ROOT/build/Gyro4All/panel/Release/gyro4all.dll" "$WIN64/Gyro4All/gyro4all.dll"  # D3D11/D3D10/D3D9 settings panel
# License notices that must ship with gyrolib.dll (GyroLib THIRD_PARTY.md), plus Gyro4All's own license.
mkdir -p "$WIN64/Gyro4All/licenses"
cp "$SDK/share/doc/GyroLib/licenses/"* "$WIN64/Gyro4All/licenses/"
cp "$SDK/share/doc/GyroLib/sdl-changes/README-GyroLib.md" "$WIN64/Gyro4All/licenses/SDL-CHANGES.md"
cp "$ROOT/LICENSE.txt" "$WIN64/Gyro4All/licenses/Gyro4All-LICENSE.txt"
cp "$ROOT/third_party/minhook-1.3.4/LICENSE.txt" "$WIN64/Gyro4All/licenses/MinHook-LICENSE.txt"  # linked into the proxy DLL
if [ -f "$ROOT/profiles/$STEM.ini" ]; then cp "$ROOT/profiles/$STEM.ini" "$WIN64/Gyro4All/profiles/"; echo "profile: $STEM.ini"
else echo "profile: none (generic ControlRotation view)"; fi
echo "Gyro4All deployed to $WIN64 as $PROXY for $EXE"
