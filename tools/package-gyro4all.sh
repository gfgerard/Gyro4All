#!/usr/bin/env bash
# Builds Gyro4All and packs the installable release zip: build/release/Gyro4All-v<version>.zip
# (version from project() in src/CMakeLists.txt). Extract the zip into the game's
# <Project>/Binaries/Win64 folder: it holds winmm.dll and the Gyro4All folder, the same files
# tools/deploy-gyro4all.sh installs, plus every profile and the user docs (README, TROUBLESHOOTING, ADVANCED, CREDITS,
# in Gyro4All/readme). No gyrolib.ini: users keep theirs.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tools/gyrolib-sdk.sh"; SDK="$GYROLIB_SDK"
VERSION="$(sed -nE 's/^project\(Gyro4All VERSION ([0-9.]+).*/\1/p' "$ROOT/src/CMakeLists.txt")"
[ -n "$VERSION" ] || { echo "no version in project() of src/CMakeLists.txt"; exit 1; }

bash "$ROOT/tools/build-gyro4all.sh"
DLL="$ROOT/build/Gyro4All/Release/winmm.dll"
# The DLL must carry this exact version (its log line), so a stale build can't be shipped under a new name.
grep -aq "Gyro4All $VERSION in " "$DLL" || { echo "$DLL is not version $VERSION (build failed?)"; exit 1; }

NAME="Gyro4All-v$VERSION"
OUT="$ROOT/build/release"
STAGE="$OUT/$NAME"
rm -rf "$STAGE" "$OUT/$NAME.zip"
mkdir -p "$STAGE/Gyro4All/profiles" "$STAGE/Gyro4All/licenses" "$STAGE/Gyro4All/readme"
cp "$DLL" "$STAGE/winmm.dll"
cp "$SDK/bin/gyrolib.dll" "$STAGE/Gyro4All/gyrolib.dll"
# The settings panel of D3D11/D3D10/D3D9 games (loaded by winmm.dll only there). Its ImGui is under GyroLib's ImGui.txt.
cp "$ROOT/build/Gyro4All/panel/Release/gyro4all.dll" "$STAGE/Gyro4All/gyro4all.dll"
cp "$ROOT/profiles/"*.ini "$STAGE/Gyro4All/profiles/"
# License notices that must ship with gyrolib.dll (GyroLib THIRD_PARTY.md), plus Gyro4All's own license.
cp "$SDK/share/doc/GyroLib/licenses/"* "$STAGE/Gyro4All/licenses/"
cp "$SDK/share/doc/GyroLib/sdl-changes/README-GyroLib.md" "$STAGE/Gyro4All/licenses/SDL-CHANGES.md"
cp "$ROOT/LICENSE.txt" "$STAGE/Gyro4All/licenses/Gyro4All-LICENSE.txt"
cp "$ROOT/third_party/minhook-1.3.4/LICENSE.txt" "$STAGE/Gyro4All/licenses/MinHook-LICENSE.txt"  # linked into winmm.dll
# Same layout as the repository (README.md beside docs/), so the links between them keep working.
mkdir -p "$STAGE/Gyro4All/readme/docs"
cp "$ROOT/README.md" "$STAGE/Gyro4All/readme/README.md"
for doc in TROUBLESHOOTING ADVANCED CREDITS GAMES; do cp "$ROOT/docs/$doc.md" "$STAGE/Gyro4All/readme/docs/$doc.md"; done

# Python's zipfile writes forward-slash paths (PowerShell 5.1's Compress-Archive writes backslashes).
python -I - "$(cygpath -w "$STAGE")" "$(cygpath -w "$OUT/$NAME.zip")" <<'EOF'
import os, sys, zipfile
src, dst = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for d, _, files in sorted(os.walk(src)):
        for f in sorted(files):
            p = os.path.join(d, f)
            z.write(p, os.path.relpath(p, src).replace(os.sep, "/"))
EOF
rm -rf "$STAGE"
echo "release: build/release/$NAME.zip"
python -I -c "import sys, zipfile; [print(f'  {i.file_size:>9}  {i.filename}') for i in zipfile.ZipFile(sys.argv[1]).infolist()]" "$(cygpath -w "$OUT/$NAME.zip")"
