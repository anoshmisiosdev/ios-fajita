#!/bin/sh
# Build the phone's sweetgpu test tool (iPhoneOS arm64, ad-hoc signed like the
# rest of the userland we add; patch 0085's code-signing relaxation runs it).
#   tools/sweetgpu-cli/build.sh      -> tools/sweetgpu-cli/out/sweetgpu
set -e
H=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$H/out"
xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -O2 -Wall -Wextra \
	-o "$H/out/sweetgpu" "$H/sweetgpu.c" -framework IOKit -framework CoreFoundation
codesign -f -s - "$H/out/sweetgpu"
echo "built $H/out/sweetgpu"
