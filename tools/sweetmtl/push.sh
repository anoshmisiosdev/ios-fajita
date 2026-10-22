#!/bin/sh
# Install out/SweetMTLDriver.bundle into an iOS root so Metal finds it at the
# path the kernel advertises (sweetmtl=../../../usr/lib/sweetmtl/SweetMTLDriver
# resolves to /usr/lib/sweetmtl/SweetMTLDriver.bundle).
#
#   ./push.sh /path/to/mounted/root        # e.g. hdiutil attach ios16-root.img
#   ADB=1 ./push.sh                        # phone running TWRP / rw root
#
# The bundle must start with a Mach-O header on the phone; codesign -f -s - in
# build.sh is what makes it loadable through the AMFI relaxation in patch 0085.
set -e
H=$(cd "$(dirname "$0")" && pwd)
B=$H/out/SweetMTLDriver.bundle
D=/usr/lib/sweetmtl
[ -d "$B" ] || { echo "run ./build.sh first"; exit 1; }

if [ "${ADB:-0}" = 1 ]; then
	adb shell "mkdir -p $D"
	adb push "$B" "$D/" >/dev/null
	adb shell "chmod -R a+rX $D && ls -l $D/SweetMTLDriver.bundle/Contents/MacOS"
	echo "installed $D/SweetMTLDriver.bundle (adb)"
else
	R=${1:?usage: push.sh MOUNTED_ROOT  (or ADB=1 push.sh)}
	[ -d "$R" ] || { echo "no such root: $R"; exit 1; }
	mkdir -p "$R$D"
	rm -rf "$R$D/SweetMTLDriver.bundle"
	cp -R "$B" "$R$D/"
	echo "installed $R$D/SweetMTLDriver.bundle"
fi
