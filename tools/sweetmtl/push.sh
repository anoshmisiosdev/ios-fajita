#!/bin/sh
# Install what build.sh made into an iOS root, at the path the kernel advertises
# (sweetmtl=../../../usr/lib/sweetmtl/SweetMTLDriver resolves to
# /usr/lib/sweetmtl/SweetMTLDriver.bundle):
#
#   /usr/lib/sweetmtl/SweetMTLDriver.bundle        the driver Metal loads
#   /usr/lib/sweetmtl/metal_compute                the test tool (README.md, "The test tool")
#   /usr/lib/sweetmtl/scale_and_offset.metallib    the kernel it dispatches
#   /usr/lib/sweetmtl/libvulkan_freedreno.dylib    Turnip, if one is staged in out/
#
# (the ICD is only copied when out/libvulkan_freedreno.dylib exists: building
# Turnip is tools/sweetgl/mesa-setup.sh's job, and a root that already has one
# there keeps it)
#
#   ./push.sh /path/to/mounted/root        # e.g. hdiutil attach ios16-root.img
#   ADB=1 ./push.sh                        # phone running TWRP / rw root
#
# The bundle must start with a Mach-O header on the phone; codesign -f -s - in
# build.sh is what makes it loadable through the AMFI relaxation in patch 0085.
set -e
H=$(cd "$(dirname "$0")" && pwd)
O=$H/out
B=$O/SweetMTLDriver.bundle
D=/usr/lib/sweetmtl
[ -d "$B" ] || { echo "run ./build.sh first"; exit 1; }

# Names only: $O may contain spaces (e.g. "Application Support"), so the
# loops below build each path quoted instead of word-splitting a path list.
FILES="metal_compute scale_and_offset.metallib libvulkan_freedreno.dylib"

if [ "${ADB:-0}" = 1 ]; then
	adb shell "mkdir -p $D"
	adb push "$B" "$D/" >/dev/null
	for n in $FILES; do
		f=$O/$n
		[ -f "$f" ] && adb push "$f" "$D/" >/dev/null
	done
	adb shell "chmod -R a+rX $D && chmod a+x $D/metal_compute 2>/dev/null; ls -l $D"
	echo "installed $D (adb)"
else
	R=${1:?usage: push.sh MOUNTED_ROOT  (or ADB=1 push.sh)}
	[ -d "$R" ] || { echo "no such root: $R"; exit 1; }
	mkdir -p "$R$D"
	rm -rf "$R$D/SweetMTLDriver.bundle"
	cp -R "$B" "$R$D/"
	for n in $FILES; do
		f=$O/$n
		[ -f "$f" ] && cp "$f" "$R$D/"
	done
	chmod a+x "$R$D/metal_compute" 2>/dev/null || true
	echo "installed $R$D"
	ls -l "$R$D"
fi
