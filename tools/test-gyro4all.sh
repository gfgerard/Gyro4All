#!/usr/bin/env bash
# Builds Gyro4All and its unit tests, then runs the tests (tests/profile_tests.cpp: the Unreal profile's views, the
# Match=Camera choice).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
bash "$ROOT/tools/build-gyro4all.sh"
"$ROOT/build/Gyro4All/Release/gyro4all_tests.exe"
