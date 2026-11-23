#!/usr/bin/env bash
# Builds Gyro4All and packs the installable release zip and its SHA256SUMS.txt (tools/package-only-gyro4all.sh):
# build/release/Gyro4All-v<version>.zip, version from project() in src/CMakeLists.txt.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
bash "$ROOT/tools/build-gyro4all.sh"
bash "$ROOT/tools/package-only-gyro4all.sh"
