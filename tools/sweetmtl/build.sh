#!/bin/sh
# Build SweetMTLDriver.bundle: the Metal driver plugin Metal.framework loads
# for the SweetGPU accelerator on iOS 16 (see README.md). Ad-hoc signed, like
# sweetgl: patch 0085's code-signing relaxation (cs_enforcement_disable + the
# AMFI dyld policy) is what lets backboardd map it.
#
# The kernel points Metal at it with (boot-args):
#   sweetaccel=1
#   sweetmtl=../../../usr/lib/sweetmtl/SweetMTLDriver      # no .bundle suffix
#   sweetmtlclass=SweetMTLDevice
set -e
H=$(cd "$(dirname "$0")" && pwd)
O=$H/out
B=$O/SweetMTLDriver.bundle
CC="xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0"

rm -rf "$B"
mkdir -p "$B/Contents/MacOS"

# -bundle (MH_BUNDLE) with Objective-C: the MTLDevice protocol is huge and is
# filled in by the metal2vulkan layer, so suppress the unimplemented-protocol
# warnings the same way the macOS driver's build does.
$CC -bundle -fobjc-arc -O2 -Wall -Wno-protocol -Wno-incomplete-implementation \
	-o "$B/Contents/MacOS/SweetMTLDriver" "$H/SweetMTLDevice.m" \
	-framework Foundation -framework Metal

cp "$H/Info.plist" "$B/Contents/Info.plist"

# Stage the Vulkan backend next to the bundle if it is present: Turnip
# (libvulkan_freedreno.dylib, Mesa built with -Dvulkan-drivers=freedreno) and
# the metal2vulkan translator. Absent for now; the load milestone only needs
# the bundle.
[ -f "$H/out/libvulkan_freedreno.dylib" ] && cp "$H/out/libvulkan_freedreno.dylib" "$B/Contents/MacOS/"
[ -f "$H/out/libmetal2vulkan.dylib" ] && cp "$H/out/libmetal2vulkan.dylib" "$B/Contents/MacOS/"

codesign -f -s - "$B"
echo "built $B"
