#!/bin/sh
# Build SweetMTLDriver.bundle - the Metal driver plugin Metal.framework loads for the SweetGPU
# accelerator on iOS 16 (see README.md) - and its test tool, test/metal_compute.m. Ad-hoc signed,
# like sweetgl: patch 0085's code-signing relaxation (cs_enforcement_disable + the AMFI dyld
# policy) is what lets backboardd map it.
#
# The kernel points Metal at the bundle with (boot-args):
#   sweetaccel=1
#   sweetmtl=../../../usr/lib/sweetmtl/SweetMTLDriver      # no .bundle suffix
#   sweetmtlclass=SweetMTLDevice
#
#   ./build.sh           iPhoneOS arm64: out/SweetMTLDriver.bundle, out/metal_compute and
#                        out/scale_and_offset.metallib (the last where the Metal toolchain is
#                        installed; the tool also runs against AIR bitcode)
#   ./build.sh --host    macOS, for the local self-check (an Intel Mac gets its own arch): the same
#                        bundle built against the host's bridge, for the tool's SWEETMTL_DIRECT mode
#                        (the commands are in README.md, "Where it was tested")
#
# The bundle links the bridge (tools/sweetmtl/bridge) *statically*: the phone has no dyld path for
# a second dylib next to the bundle, and the bridge is the only user of metal2vulkan's runtime. The
# library is looked for in this order, first match wins:
#
#   $SWEETMTL_BRIDGE_ARTIFACT        an unpacked sweetmtl-bridge-ios CI artifact's directory, or
#                                    the libsweetmtl_bridge.a itself
#   bridge/out/libsweetmtl_bridge.a                          the CI job's staged output
#   bridge/target/aarch64-apple-ios/release/libsweetmtl_bridge.a   (--host: target/{release,debug})
#
# So, to build against the CI artifact:
#   gh run download --repo anoshmisiosdev/ios-sweet -n sweetmtl-bridge-ios -D /tmp/sweetmtl-bridge
#   SWEETMTL_BRIDGE_ARTIFACT=/tmp/sweetmtl-bridge ./build.sh
set -e
H=$(cd "$(dirname "$0")" && pwd)
O=$H/out
B=$O/SweetMTLDriver.bundle

MODE=ios
SDK=iphoneos
ARCH=arm64
if [ "${1:-}" = "--host" ]; then
	MODE=host
	SDK=macosx
	ARCH=$(uname -m)
fi

CC="xcrun -sdk $SDK clang -arch $ARCH"
if [ "$MODE" = host ]; then
	CC="$CC -mmacosx-version-min=13.0"
	BRIDGE_CANDIDATES="$H/bridge/target/release/libsweetmtl_bridge.a $H/bridge/target/debug/libsweetmtl_bridge.a"
	BRIDGE_HINT="cd $H/bridge && cargo build"
else
	CC="$CC -miphoneos-version-min=16.0"
	BRIDGE_CANDIDATES="$H/bridge/out/libsweetmtl_bridge.a $H/bridge/target/aarch64-apple-ios/release/libsweetmtl_bridge.a"
	BRIDGE_HINT="cd $H/bridge && cargo build --release --target aarch64-apple-ios"
fi

find_bridge() {
	if [ -n "${SWEETMTL_BRIDGE_ARTIFACT:-}" ]; then
		if [ -f "$SWEETMTL_BRIDGE_ARTIFACT" ]; then
			echo "$SWEETMTL_BRIDGE_ARTIFACT"
			return 0
		fi
		if [ -f "$SWEETMTL_BRIDGE_ARTIFACT/libsweetmtl_bridge.a" ]; then
			echo "$SWEETMTL_BRIDGE_ARTIFACT/libsweetmtl_bridge.a"
			return 0
		fi
	fi
	for candidate in $BRIDGE_CANDIDATES; do
		if [ -f "$candidate" ]; then
			echo "$candidate"
			return 0
		fi
	done
	return 1
}

BRIDGE=$(find_bridge) || {
	echo "build.sh: no libsweetmtl_bridge.a: set SWEETMTL_BRIDGE_ARTIFACT to an unpacked" >&2
	echo "          sweetmtl-bridge-ios artifact (gh run download -n sweetmtl-bridge-ios), or" >&2
	echo "          run: $BRIDGE_HINT" >&2
	exit 1
}
echo "bridge: $BRIDGE ($(wc -c < "$BRIDGE" | tr -d ' ') bytes)"

rm -rf "$B"
mkdir -p "$B/Contents/MacOS"

# -bundle (MH_BUNDLE) with Objective-C: the MTLDevice protocol is huge and the part of it the
# driver does not implement is answered by the stub in SweetMTLDevice.m, so the unimplemented-
# protocol and unsynthesized-protocol-property warnings are suppressed, as the macOS driver's
# build does.
FLAGS="-bundle -fobjc-arc -O2 -Wall -Wextra -Wno-protocol -Wno-incomplete-implementation"
FLAGS="$FLAGS -Wno-objc-protocol-property-synthesis"
if [ "$MODE" = host ]; then
	FLAGS="$FLAGS -DSWEETMTL_HOST=1"
fi

$CC $FLAGS -I "$H/bridge/include" -o "$B/Contents/MacOS/SweetMTLDriver" \
	"$H/SweetMTLDevice.m" "$H/SweetMTLObjects.m" \
	-framework Foundation -framework Metal "$BRIDGE"

cp "$H/Info.plist" "$B/Contents/Info.plist"

# The test tool (test/metal_compute.m): MTLCreateSystemDefaultDevice() and the MTL surface, linked
# against Metal.framework alone - the driver it runs on is the bundle, which Metal loads or (with
# SWEETMTL_DIRECT) the tool loads itself. Signed like the bundle, so the phone's shell can run it.
$CC -fobjc-arc -O2 -Wall -Wextra -Wno-protocol -Wno-incomplete-implementation \
	-Wno-objc-protocol-property-synthesis -I "$H" -I "$H/bridge/include" \
	-o "$O/metal_compute" "$H/test/metal_compute.m" \
	-framework Foundation -framework Metal

# The kernel the tool dispatches, for the same SDK. Xcode 16.3 and later ship the Metal compiler
# as a separate download (`xcodebuild -downloadComponent MetalToolchain`); where it is missing the
# tool still runs against AIR bitcode (SWEETMTL_AIR=1, README.md "Where it was tested"), so this
# says so and moves on rather than failing a build whose driver and tool are fine.
if xcrun -sdk "$SDK" metal --version >/dev/null 2>&1; then
	xcrun -sdk "$SDK" metal -c "$H/test/scale_and_offset.metal" -o "$O/scale_and_offset.air"
	xcrun -sdk "$SDK" metallib "$O/scale_and_offset.air" -o "$O/scale_and_offset.metallib"
	echo "metal: out/scale_and_offset.metallib ($(xcrun -sdk "$SDK" metal --version | head -1))"
else
	echo "metal: this Xcode has no Metal compiler, out/scale_and_offset.metallib not built" >&2
	echo "       (xcodebuild -downloadComponent MetalToolchain; or use SWEETMTL_AIR=1)" >&2
fi

codesign -f -s - "$B"
codesign -f -s - "$O/metal_compute"
echo "built $B"
echo "built $O/metal_compute"
