# Sourced by build/deploy scripts: the GyroLib SDK this project builds and ships with.
GYROLIB_VERSION=1.4.0
# SHA256 of GyroLib-$GYROLIB_VERSION-sdk-windows-x64.zip (the release's SHA256SUMS.txt), checked by tools/fetch-deps.sh.
GYROLIB_SDK_SHA256=6613863a9e04e44897f5e91baa55e197972996fb6719138e735d175a419bdfb2
GYROLIB_SDK="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/third_party/GyroLib-$GYROLIB_VERSION/sdk/GyroLib-$GYROLIB_VERSION-sdk-windows-x64"
