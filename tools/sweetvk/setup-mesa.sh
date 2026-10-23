#!/bin/sh
# Fetch Mesa 23.1.9, apply tools/sweetvk/mesa.patch and configure it for iOS 16
# arm64 with Turnip only: Vulkan, the freedreno/msm backend, no window system,
# no gallium, no LLVM. build.sh turns the result into
# out/libvulkan_freedreno.dylib (a Mesa ICD) and out/vk_compute.
#
#   ./setup-mesa.sh                 fetch, patch and configure (idempotent)
#   ./setup-mesa.sh --reconfigure   re-run meson setup with the current options
#   ./setup-mesa.sh --clean         throw the source and build directories away
#
# The Mesa source, the tarball and the build directory live in
# tools/sweetvk/build/ (gitignored); the cross file is generated there too
# (ios-cross.txt.in -> build/ios-cross.txt) because the SDK and checkout paths
# differ per machine. Ported from joranv1/ios-sailfish's tools/sweetgl.
set -e
H=$(cd "$(dirname "$0")" && pwd)
B=$H/build
V=23.1.9
SRC=$B/mesa-$V
BD=$B/mesa-ios

if [ "${1:-}" = --clean ]; then
	rm -rf "$SRC" "$BD" "$B/mesa-$V.tar.xz"
	echo "cleaned $B"
	exit 0
fi

mkdir -p "$B"

# 1. The release tarball (18 MB), kept next to the source so a rebuild is local.
if [ ! -f "$B/mesa-$V.tar.xz" ]; then
	echo "fetching mesa-$V.tar.xz"
	curl -sSLo "$B/mesa-$V.tar.xz.part" "https://archive.mesa3d.org/mesa-$V.tar.xz"
	mv "$B/mesa-$V.tar.xz.part" "$B/mesa-$V.tar.xz"
fi
if [ ! -d "$SRC" ]; then
	echo "extracting mesa-$V"
	tar xf "$B/mesa-$V.tar.xz" -C "$B"
fi

# 2. mesa.patch: only what an OS with no DRM needs (see the file).
if [ ! -f "$SRC/.sweetvk-patched" ]; then
	echo "applying mesa.patch"
	(cd "$SRC" && patch -p1 --forward --no-backup-if-mismatch < "$H/mesa.patch")
	touch "$SRC/.sweetvk-patched"
fi

# 3. Configure. build.sh is the one place that knows where the SDK is: it
#    generates the cross file, builds the shim it force-loads into the ICD and
#    sets up the python the generators need (on PATH from here on).
sh "$H/build.sh" --cross-only
PATH=$B/venv/bin:$PATH
export PATH

# Meson keeps the cross file's built-in options (the include path, -include
# sweet_compat.h, the force-load link args) in the build directory and does not
# reread them when it reconfigures, so a changed cross file needs a fresh build
# directory - otherwise the build silently keeps the old flags.
STAMP=$B/mesa-ios.cross.sha256
CROSS_SHA=$(shasum -a 256 "$B/ios-cross.txt" | awk '{print $1}')
if [ -d "$BD" ] && { [ ! -f "$STAMP" ] || [ "$(cat "$STAMP")" != "$CROSS_SHA" ]; }; then
	echo "the cross file changed: dropping $BD"
	rm -rf "$BD"
fi
printf '%s\n' "$CROSS_SHA" > "$STAMP"

OPTIONS="--buildtype=release -Db_ndebug=true -Ddefault_library=static \
	-Dplatforms= -Dgallium-drivers= -Dvulkan-drivers=freedreno -Dfreedreno-kmds=msm \
	-Dllvm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
	-Degl=disabled -Dglx=disabled -Dgbm=disabled -Dosmesa=false \
	-Dzstd=disabled -Dzlib=disabled -Dexpat=disabled -Dxmlconfig=disabled \
	-Dlibunwind=disabled -Dlmsensors=disabled -Dvalgrind=disabled -Dselinux=false \
	-Dbuild-tests=false -Dshader-cache=disabled -Dperfetto=false \
	-Dgallium-va=disabled -Dgallium-rusticl=false -Dvideo-codecs= -Dtools= \
	-Dvulkan-beta=false"

# shellcheck disable=SC2086
if [ ! -d "$BD" ]; then
	echo "configuring $BD"
	meson setup "$BD" "$SRC" --cross-file "$B/ios-cross.txt" $OPTIONS
else
	echo "reconfiguring $BD"
	meson setup --reconfigure "$BD" "$SRC" --cross-file "$B/ios-cross.txt" $OPTIONS
fi
