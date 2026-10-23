#!/bin/sh
# Build libvulkan_freedreno.dylib - Mesa Turnip (the Vulkan driver for the
# Adreno 630 in this phone) for iOS 16 arm64 - and out/vk_compute, the CLI test
# that only passes on the phone. Both are ad-hoc signed, like tools/sweetmtl's
# bundle: the code-signing relaxation 0085 adds is what lets a phone-side
# process map them.
#
#   ./build.sh                the ICD and the test (fetches and configures Mesa
#                             on the first run; ~10 min cold, seconds warm)
#   ./build.sh --cross-only   only build/ios-cross.txt and out/libsweetvk.a
#                             (setup-mesa.sh calls this before configuring)
#
# Steps:
#   1. generate build/ios-cross.txt from ios-cross.txt.in (xcrun finds the SDK)
#   2. fetch Mesa 23.1.9 if it is not there yet: the msm emulation is compiled
#      against its drm-uapi headers
#   3. compile the msm emulation (sweet_drm.c) for iphoneos into
#      out/libsweetvk.a; Mesa's libvulkan_freedreno link force-loads it
#      (the cross file's c_link_args/cpp_link_args)
#   4. setup-mesa.sh: patch Mesa 23.1.9, meson configure
#   5. ninja the ICD, copy it to out/, name it for the phone's /usr/lib/sweetmtl
#      (SweetMTLDevice.m's SWEETMTL_ICD default), ad-hoc sign, check it exports
#      vk_icdGetInstanceProcAddr (what tools/sweetmtl's bridge dlsym's) and
#      relink it once without -undefined dynamic_lookup (see check_unresolved)
#   6. compile out/vk_compute
#
# Nothing here touches the kernel, the boot arguments or the code-signing
# relaxation; the phone side is a file to copy, nothing more.
set -e
H=$(cd "$(dirname "$0")" && pwd)
B=$H/build
O=$H/out
MV=23.1.9
SRC=$B/mesa-$MV
BD=$B/mesa-ios
DYLIB=$BD/src/freedreno/vulkan/libvulkan_freedreno.dylib

SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC=$(xcrun --find clang)
CXX=$(xcrun --find clang++)
AR=$(xcrun --find ar)
STRIP=$(xcrun --find strip)
MINVER=16.0

mkdir -p "$B" "$O"

gen_cross()
{
	shim=$O/libsweetvk.a
	# The msm emulation is a plain user-space library: force_load it into the
	# ICD link (Mesa's own link command), together with the frameworks it
	# calls. -Wl,-undefined,dynamic_lookup, which that link also needs, is *not*
	# here: meson passes the cross file's link args to its feature probes too,
	# and with the flag every "is this function there?" probe links and answers
	# yes - that is how HAVE_MEMFD_CREATE appeared and anon_file.c stopped
	# compiling. It lives in mesa.patch, on the ICD target's link_args, instead.
	link_args="['-Wl,-force_load,$shim', '-framework', 'IOKit', '-framework', 'CoreFoundation']"
	sed \
	    -e "s|@CC@|'$CC'|g" -e "s|@CXX@|'$CXX'|g" \
	    -e "s|@AR@|'$AR'|g" -e "s|@STRIP@|'$STRIP'|g" \
	    -e "s|@SDK@|'$SDK'|g" -e "s|@MINVER@|$MINVER|g" \
	    -e "s|@SHIM_INC@|$H/include|g" \
	    -e "s|@LINK_ARGS@|$link_args|g" \
	    "$H/ios-cross.txt.in" > "$B/ios-cross.txt"
}

# Mesa's generators want the mako module (and pyyaml for a few tables), and its
# meson.build checks mako's version with distutils, which Python 3.12 dropped:
# setuptools brings distutils back. Keep all of it in a virtualenv beside the
# source, and first on PATH: the generator scripts run through their
# "#!/usr/bin/env python3" shebang, so the PATH is what decides which python
# they get.
ensure_python()
{
	if [ ! -x "$B/venv/bin/python3" ]; then
		echo "creating $B/venv (mako, pyyaml, setuptools)"
		python3 -m venv "$B/venv"
		"$B/venv/bin/pip" install --quiet --upgrade pip
		"$B/venv/bin/pip" install --quiet mako pyyaml setuptools
	fi
	PATH=$B/venv/bin:$PATH
	export PATH
}

build_shim()
{
	[ -d "$SRC" ] || { echo "no Mesa source at $SRC: run setup-mesa.sh first" >&2; exit 1; }
	echo "building the msm emulation (sweet_drm.c, compat.c) for iphoneos"
	$CC -arch arm64 -isysroot "$SDK" -miphoneos-version-min=$MINVER \
	    -O2 -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden \
	    -I "$H/include" -I "$SRC/include" \
	    -c "$H/sweet_drm.c" -o "$O/sweet_drm.o"
	$CC -arch arm64 -isysroot "$SDK" -miphoneos-version-min=$MINVER \
	    -O2 -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden \
	    -I "$H/include" \
	    -c "$H/compat.c" -o "$O/compat.o"
	rm -f "$O/libsweetvk.a"
	$AR rcs "$O/libsweetvk.a" "$O/sweet_drm.o" "$O/compat.o"
}

gen_cross
ensure_python
# The shim below compiles against the drm-uapi headers in Mesa's source tree,
# and Mesa is fetched later (setup-mesa.sh, which patches and configures it),
# so a machine that has never built this has to fetch it first. This was a real
# cold-start bug: locally the source was always already there.
[ -d "$SRC" ] || sh "$H/setup-mesa.sh" --fetch-only
build_shim

if [ "${1:-}" = "--cross-only" ]; then
	echo "$B/ios-cross.txt and $O/libsweetvk.a are ready"
	exit 0
fi

# Mesa itself: fetch, patch, configure (no-op once done), then the ICD target.
sh "$H/setup-mesa.sh"
ensure_python
echo "building the ICD (ninja)"
ninja -C "$BD" src/freedreno/vulkan/libvulkan_freedreno.dylib

# The ICD's link passes -Wl,-undefined,dynamic_lookup (mesa.patch) so that the
# weak references in Mesa's generated entrypoint tables may stay unresolved, as
# they do on Linux: an entry no implementation provides stays NULL, and the
# Vulkan runtime's own vk_common_* fallback handles that entry point (that is
# why so few tu_* entry points exist in 23.1.9 - tu_device.cc sets
# queue->vk.driver_submit and the runtime's vk_common_QueueSubmit does the rest).
# The flag also silences the linker about *any* other missing symbol, though: one
# we forgot to compile in, or whose declaration has the wrong linkage, would only
# fail when dyld loads the ICD on the phone. So link once more without it and
# check where ld's refused references come from: they must all be references from
# those generated tables, and a reference from anywhere else - our shim, the msm
# backend, any other Mesa object - fails the build here.
check_unresolved()
{
	echo "checking what the ICD link leaves unresolved (relink without dynamic_lookup)"
	ninja -C "$BD" -t commands src/freedreno/vulkan/libvulkan_freedreno.dylib | tail -1 |
	    sed "s| -Wl,-undefined,dynamic_lookup||; s| -o src/freedreno/vulkan/libvulkan_freedreno.dylib| -Wl,-undefined,error -o strict-check.dylib|" \
	    > "$BD/strict-check.sh"
	(cd "$BD" && sh strict-check.sh) > "$BD/strict-check.log" 2>&1 || true
	from=$(sed -n 's/.* in \(.*\)$/\1/p' "$BD/strict-check.log" | sed 's/^[[:space:]]*//' | sort -u)
	bad=$(printf '%s\n' "$from" | grep -vE 'entrypoints\.[a-z]*c\.o\)?$' || true)
	if [ -n "$bad" ]; then
		echo "error: the ICD link leaves references unresolved that do not come" >&2
		echo "from Mesa's generated entrypoint tables. Referenced from:" >&2
		printf '  %s\n' $bad >&2
		echo "ld's full list is in $BD/strict-check.log. A symbol of ours is not" >&2
		echo "being linked in, or a declaration of ours does not match the linkage" >&2
		echo "of its definition (ld says 'declaration possibly missing extern \"C\"')." >&2
		exit 1
	fi
	rm -f "$BD/strict-check.dylib" "$BD/strict-check.sh"
	echo "unresolved: $(grep -c '^  "' "$BD/strict-check.log" || true) Mesa table symbols, nothing else"
}
check_unresolved

cp "$DYLIB" "$O/libvulkan_freedreno.dylib"
# The bridge (tools/sweetmtl) loads the ICD by path; give it the name of the
# place it will live on the phone.
xcrun install_name_tool -id /usr/lib/sweetmtl/libvulkan_freedreno.dylib "$O/libvulkan_freedreno.dylib"
codesign -f -s - "$O/libvulkan_freedreno.dylib"

if ! nm -gU "$O/libvulkan_freedreno.dylib" | grep -q _vk_icdGetInstanceProcAddr; then
	echo "error: the ICD does not export vk_icdGetInstanceProcAddr" >&2
	exit 1
fi

echo "building the test (test/vk_compute.c)"
$CC -arch arm64 -isysroot "$SDK" -miphoneos-version-min=$MINVER \
	    -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
	    -I "$SRC/include" \
	    "$H/test/vk_compute.c" -o "$O/vk_compute"
codesign -f -s - "$O/vk_compute"

echo
echo "built:"
ls -l "$O/libvulkan_freedreno.dylib" "$O/vk_compute"
echo "the ICD exports $(nm -gU "$O/libvulkan_freedreno.dylib" | awk '{print $NF}' | grep -c '^_vk') vk_* symbols"

# Only what a phone run needs is left in out/: the archive's objects are
# intermediates and build_shim remakes them every run.
rm -f "$O/sweet_drm.o" "$O/compat.o"
