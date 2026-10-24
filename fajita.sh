#!/usr/bin/env bash
# One-stop setup for the OnePlus 6T (fajita) port on a fresh Linux machine.
#
#   ./fajita.sh all              deps, kernel, ipsw, root, qemu-smoke, then qemu-ios
#   ./fajita.sh deps             check host tools (prints the install command)
#   ./fajita.sh kernel [RUN_ID]  download the XNU kernel built by GitHub Actions
#   ./fajita.sh ipsw [BUILD]     fetch the iPhone X iOS 16 parts (~7 GB, from Apple)
#   ./fajita.sh root             build the iOS root image for the phone (sudo)
#   ./fajita.sh qemu-smoke       boot the kernel alone in QEMU (~1 min)
#   ./fajita.sh qemu-ios         boot iOS in QEMU from a RAM disk and save screenshots
#   ./fajita.sh gpu-fw [DIR]     collect the Adreno 630 firmware pulled off your phone
#   ./fajita.sh phone            build the boot image for the real phone
#   ./fajita.sh phone-test       a read-only test image: boots the kernel, writes nothing
#
# Everything lands in build/ (git-ignored). Steps are re-runnable: finished
# downloads and images are reused. Nothing here ever touches a phone: the
# fastboot commands are printed for you to run (see FAJITA.md).
#
# Environment overrides:
#   FAJITA_REPO=owner/repo    where the kernel CI runs (default: this clone's origin)
#   FAJITA_BRANCH=fajita      branch whose latest successful build is used
#   GITHUB_TOKEN=...          for artifact downloads when `gh` is not logged in
#   QEMU_SECS=10800           how long qemu-ios runs (seconds, default 3 h)
#   QEMU_ACCEL=auto|tcg|kvm|hvf
#   SWEETGPU=0                GPU bring-up level for the phone (0 off; see FAJITA.md)
set -euo pipefail

cd "$(dirname "$0")"
ROOT=$(pwd)
B=$ROOT/build
mkdir -p "$B"
export PATH="$B/bin:$PATH"	# tools built by this script (apfs-fuse)

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

repo_slug() {
	if [[ -n ${FAJITA_REPO:-} ]]; then echo "$FAJITA_REPO"; return; fi
	local u
	u=$(git remote get-url origin 2>/dev/null || true)
	u=${u%.git}
	case $u in
	*github.com[:/]*) echo "${u#*github.com[:/]}" ;;
	*) echo anoshmisiosdev/ios-sweet ;;
	esac
}
BRANCH=${FAJITA_BRANCH:-fajita}

# ---------------------------------------------------------------- deps
declare -A PKG_APT=(
	[curl]=curl [unzip]=unzip [python3]=python3 [git]=git [rsync]=rsync [cpio]=cpio
	[qemu-system-aarch64]=qemu-system-arm [lzfse]=lzfse
	[mkfs.hfsplus]=hfsprogs [fsck.hfsplus]=hfsprogs [img2simg]=android-sdk-libsparse-utils
	[aarch64-linux-gnu-gcc]=gcc-aarch64-linux-gnu [mkbootimg]=mkbootimg
)
declare -A PKG_DNF=(
	[qemu-system-aarch64]=qemu-system-aarch64 [mkfs.hfsplus]=hfsplus-tools [fsck.hfsplus]=hfsplus-tools
	[img2simg]=android-tools [aarch64-linux-gnu-gcc]=gcc-aarch64-linux-gnu [mkbootimg]=android-tools
)

need() {	# need STEP TOOL...
	local step=$1 missing=() t
	shift
	for t in "$@"; do command -v "$t" >/dev/null 2>&1 || missing+=("$t"); done
	[[ ${#missing[@]} == 0 ]] && return 0
	warn "$step needs: ${missing[*]}"
	if command -v apt-get >/dev/null; then
		local p=() ; for t in "${missing[@]}"; do [[ $t == apfs-fuse ]] || p+=("${PKG_APT[$t]:-$t}"); done
		echo "  sudo apt install $(printf '%s\n' "${p[@]}" | sort -u | tr '\n' ' ')" >&2
	elif command -v dnf >/dev/null; then
		local p=() ; for t in "${missing[@]}"; do [[ $t == apfs-fuse ]] || p+=("${PKG_DNF[$t]:-${PKG_APT[$t]:-$t}}"); done
		echo "  sudo dnf install $(printf '%s\n' "${p[@]}" | sort -u | tr '\n' ' ')" >&2
	elif command -v pacman >/dev/null; then
		echo "  pacman: install the packages providing ${missing[*]} (apfs-fuse and hfsprogs are in the AUR)" >&2
	fi
	return 1
}

# Ubuntu and Fedora do not package apfs-fuse: build it into build/bin.
ensure_apfs_fuse() {
	command -v apfs-fuse >/dev/null && return 0
	[[ $(uname -s) == Linux ]] || return 1
	say "apfs-fuse not installed: building it from source into build/bin"
	if ! command -v cmake >/dev/null || ! pkg-config --exists fuse3 2>/dev/null; then
		warn "needs cmake, g++ and the fuse3, zlib, bzip2 and attr headers:"
		echo "  sudo apt install cmake g++ fuse3 libfuse3-dev zlib1g-dev libbz2-dev libattr1-dev" >&2
		echo "  sudo dnf install cmake gcc-c++ fuse3 fuse3-devel zlib-devel bzip2-devel libattr-devel" >&2
		return 1
	fi
	local src=$B/src/apfs-fuse
	[[ -d $src ]] || git clone -q --recursive https://github.com/sgan81/apfs-fuse.git "$src"
	cmake -S "$src" -B "$src/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
	make -C "$src/build" -j"$(nproc)" apfs-fuse >/dev/null
	mkdir -p "$B/bin"
	cp "$src/build/apfs-fuse" "$B/bin/"
	say "apfs-fuse: build/bin/apfs-fuse"
}

cmd_deps() {
	ensure_apfs_fuse || true
	local ok=0
	need "kernel download" curl unzip python3 || ok=1
	need "IPSW fetch"      curl python3 lzfse || ok=1
	need "root image"      apfs-fuse mkfs.hfsplus fsck.hfsplus rsync img2simg || ok=1
	need "QEMU"            qemu-system-aarch64 || ok=1
	need "phone image"     aarch64-linux-gnu-gcc mkbootimg cpio gzip || ok=1
	command -v gh >/dev/null && gh auth status >/dev/null 2>&1 \
		|| warn "gh is not logged in: set GITHUB_TOKEN (any token with public read) for the kernel download"
	[[ $ok == 0 ]] && say "all host tools present"
	return $ok
}

# ---------------------------------------------------------------- kernel
api() {
	local h=(-H 'Accept: application/vnd.github+json')
	[[ -n ${GITHUB_TOKEN:-} ]] && h+=(-H "Authorization: Bearer $GITHUB_TOKEN")
	curl -fsSL "${h[@]}" "https://api.github.com/repos/$1"
}

cmd_kernel() {
	need "kernel download" curl unzip python3 || exit 1
	local slug run=${1:-} out=$ROOT/xnu/out
	slug=$(repo_slug)
	if [[ -z $run ]]; then
		run=$(api "$slug/actions/workflows/xnu.yml/runs?branch=$BRANCH&status=success&per_page=1" |
			python3 -c 'import json,sys; r=json.load(sys.stdin)["workflow_runs"]; print(r[0]["id"] if r else "")')
		[[ -n $run ]] || die "no successful xnu build on $slug@$BRANCH yet (Actions tab -> xnu -> Run workflow)"
	fi
	if [[ -f $out/RUN_ID && $(cat "$out/RUN_ID") == "$run" && -f $out/kernel.development.sweet ]]; then
		say "kernel from run $run already in xnu/out"; return 0
	fi
	say "downloading kernel from $slug run $run"
	rm -rf "$out"; mkdir -p "$out"
	if command -v gh >/dev/null && gh auth status >/dev/null 2>&1; then
		gh run download "$run" -R "$slug" -n xnu-kernel -D "$out"
	else
		[[ -n ${GITHUB_TOKEN:-} ]] || die "artifact downloads need auth: run 'gh auth login' or export GITHUB_TOKEN"
		local url
		url=$(api "$slug/actions/runs/$run/artifacts" |
			python3 -c 'import json,sys; print(next(a["archive_download_url"] for a in json.load(sys.stdin)["artifacts"] if a["name"]=="xnu-kernel"))')
		curl -fL -H "Authorization: Bearer $GITHUB_TOKEN" -o "$B/xnu-kernel.zip" "$url"
		unzip -q -o "$B/xnu-kernel.zip" -d "$out"
	fi
	[[ -f $out/kernel.development.sweet ]] || die "artifact has no kernel.development.sweet"
	echo "$run" > "$out/RUN_ID"
	(cd "$out" && sha256sum -c SHA256SUMS 2>/dev/null | grep -v ': OK$' || true)
	say "kernel: xnu/out/kernel.development.sweet (run $run)"
}

# ---------------------------------------------------------------- ipsw
cmd_ipsw() {
	need "IPSW fetch" curl python3 lzfse || exit 1
	local build=${1:-20H392}
	if [[ -f $B/ipsw/manifest.json ]] && python3 -c "import json,sys; sys.exit(json.load(open('$B/ipsw/manifest.json'))['build']!='$build')"; then
		say "IPSW parts for $build already in build/ipsw"; return 0
	fi
	say "fetching iPhone X iOS 16 ($build) parts from Apple, ~7 GB (resumable)"
	python3 tools/ipsw-fetch.py "$B/ipsw" --build "$build"
}

ipsw_path() { python3 -c "import json; print(json.load(open('$B/ipsw/manifest.json'))['$1'])"; }

# ---------------------------------------------------------------- root
tweak_image() {	# fsck, mount, apply tools/sweet-root-tweaks.py, unmount
	local img=$1 mnt
	mnt=$(mktemp -d)
	sudo fsck.hfsplus -fy "$img" >/dev/null || true
	sudo mount -t hfsplus -o loop,force "$img" "$mnt"
	sudo python3 tools/sweet-root-tweaks.py "$mnt" || { sudo umount "$mnt"; die "root tweaks failed"; }
	df -h "$mnt" | tail -1
	sudo umount "$mnt"
	rmdir "$mnt"
}

cmd_root() {
	[[ $(uname -s) == Linux ]] || die "building the root needs Linux (apfs-fuse, hfsprogs)"
	ensure_apfs_fuse || true
	need "root image" apfs-fuse mkfs.hfsplus fsck.hfsplus rsync img2simg || exit 1
	[[ -f $B/ipsw/manifest.json ]] || cmd_ipsw
	local img=$B/ios16-root.img
	if [[ ! -f $img ]]; then
		say "building the iOS root image (sudo; ~30 min)"
		tools/mkiosroot.sh "$(ipsw_path os)" "$(ipsw_path systemos)" "$(ipsw_path appos)" "$img" 16G
		tweak_image "$img"
	else
		say "root image exists: $img"
	fi
	if [[ ! -f $B/ios16-root.simg || $B/ios16-root.simg -ot $img ]]; then
		img2simg "$img" "$B/ios16-root.simg"
	fi
	say "phone root: build/ios16-root.simg (fastboot -S 512M flash userdata ..., see FAJITA.md)"
}

# ---------------------------------------------------------------- qemu
pick_accel() {
	local a=${QEMU_ACCEL:-auto}
	if [[ $a == auto ]]; then
		if [[ $(uname -s) == Darwin && $(uname -m) == arm64 ]]; then a=hvf
		elif [[ $(uname -m) == aarch64 && -w /dev/kvm ]]; then a=kvm
		else a=tcg; fi
	fi
	echo "$a"
}

cmd_qemu_smoke() {
	need "QEMU" qemu-system-aarch64 python3 || exit 1
	[[ -f xnu/out/kernel.development.sweet ]] || cmd_kernel
	local d=$B/qemu-smoke
	mkdir -p "$d"
	python3 tools/mkadt.py "$d/q.adt" --qemu 2>/dev/null
	local extra=""
	[[ ${SWEETACCEL:-0} != 0 ]] && extra=" sweetaccel=1 sweetmtl=${SWEETMTL:-../../../usr/lib/sweetmtl/SweetMTLDriver} sweetmtlclass=${SWEETMTLCLASS:-SweetMTLDevice}"
	python3 tools/mkpayload.py xnu/out/kernel.development.sweet "$d/q.adt" "$d/q.bin" \
		--cmdline "debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress fajita_dpu=0 sweettouch=0$extra" >/dev/null
	say "booting the kernel alone in QEMU for 60 s"
	python3 tools/qemu-sweet.py "$d/q.bin" "$d/out" --secs 60 --accel "$(pick_accel)" || true
	say "screen: build/qemu-smoke/out/fb.png (expect the console ending at 'Waiting on ... Apple_HFS'; on macOS use QEMU_ACCEL=tcg, hvf stops at arm_vm_init)"
}

# A copy of the root just big enough to hold its files: it becomes a RAM disk.
qemu_root() {
	local full=$B/ios16-root.img small=$B/ios16-qemu.img mnt used size
	[[ -f $small ]] && { echo "$small"; return; }
	[[ -f $full ]] || cmd_root >&2
	mnt=$(mktemp -d)
	sudo mount -t hfsplus -o loop,ro,force "$full" "$mnt"
	used=$(df --output=used -B1 "$mnt" | tail -1)
	sudo umount "$mnt"; rmdir "$mnt"
	size=$(( (used * 115 / 100 + (512 << 20) + (256 << 20) - 1) / (256 << 20) * (256 << 20) ))
	say "building a $((size >> 20)) MiB RAM-disk copy of the root (sudo)" >&2
	tools/mkiosroot.sh "$(ipsw_path os)" "$(ipsw_path systemos)" "$(ipsw_path appos)" "$small" "$size" >&2
	tweak_image "$small" >&2
	echo "$small"
}

cmd_qemu_ios() {
	[[ $(uname -s) == Linux ]] || die "qemu-ios builds its RAM disk with Linux tools"
	need "QEMU" qemu-system-aarch64 python3 || exit 1
	[[ -f xnu/out/kernel.development.sweet ]] || cmd_kernel
	local rd rdsize mem guest_gib avail_gib d=$B/qemu-ios accel
	rd=$(qemu_root)
	rdsize=$(stat -c %s "$rd")
	# Kernel RAM = RAM disk + 3 GiB (the iPhone X's RAM) in the upper bank
	mem=$(( (rdsize + (3 << 30) + (1 << 30) - 1) / (1 << 30) * (1 << 30) ))
	guest_gib=$(( (0x100000000 + mem - 0x40000000 + (1 << 30) - 1) >> 30 ))
	avail_gib=$(( $(awk '/MemAvailable/{print $2}' /proc/meminfo) >> 20 ))
	# The payload tools hold the RAM disk in memory while building it.
	if (( avail_gib < guest_gib + (rdsize >> 30) + 1 )); then
		warn "this needs ~$((guest_gib + (rdsize >> 30) + 1)) GiB free RAM; only $avail_gib GiB available"
	fi
	mkdir -p "$d"
	python3 tools/mkadt.py "$d/q.adt" --qemu --highmem --mem-size "$mem" 2>/dev/null
	say "packing kernel + $((rdsize >> 20)) MiB RAM disk"
	python3 tools/mkpayload.py xnu/out/kernel.development.sweet "$d/q.adt" "$d/q.bin" --ramdisk "$rd" \
		--cmdline 'debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress rd=md0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin launchd_unsecure_cache=1 wdt=-1 sweetfb_noconsole=1 sweetfb_hz=10 fajita_dpu=0 sweettouch=0 sweetkeys=0' >/dev/null
	accel=$(pick_accel)
	[[ $accel == tcg ]] && warn "no KVM/HVF: pure emulation, expect hours to reach the lock screen"
	say "booting iOS in QEMU ($accel) for ${QEMU_SECS:-10800} s; a screenshot every 60 s goes to build/qemu-ios/out/snaps"
	python3 tools/qemu-sweet.py "$d/q.bin" "$d/out" --secs "${QEMU_SECS:-10800}" --snap-every 60 --accel "$accel" || true
	rm -f "$d/q.bin"
	say "last screen: build/qemu-ios/out/fb.png; the whole run: build/qemu-ios/out/snaps/"
}

# ---------------------------------------------------------------- gpu-fw
cmd_gpu_fw() {
	local src=${1:-$B/gpu-fw-src}
	if [[ ! -f $src/a630_sqe.fw ]]; then
		mkdir -p "$src"
		cat <<-EOF
		  Pull the GPU firmware from your phone (Android or a recovery with adb), then rerun:
		    adb pull /vendor/firmware/a630_sqe.fw $src/
		    adb pull /vendor/firmware/a630_gmu.bin $src/
		    adb shell ls /vendor/firmware/a630_zap.* /vendor/firmware_mnt/image/a630_zap.*
		    adb pull /vendor/firmware/a630_zap.mdt $src/    # and every a630_zap.b0N listed
		  (on some builds the zap files are under /vendor/firmware_mnt/image/)
		  then: ./fajita.sh gpu-fw $src
		EOF
		return 0
	fi
	python3 tools/fajita-gpu-fw.py "$src" "$B/gpu-fw"
}

# ---------------------------------------------------------------- phone-test
# A boot image for testing on a phone that still runs Android: the kernel,
# display, touch, USB and the UFS driver come up, but nothing is written to
# storage (no ufsrw/rootrw, sweetlog=0) and no iOS root is expected, so the
# boot ends at the root mount. `fastboot boot` runs it from RAM only.
cmd_phone_test() {
	need "phone image" aarch64-linux-gnu-gcc mkbootimg cpio gzip python3 || exit 1
	[[ -f dtb/fajita-appended.dtb ]] || die "dtb/fajita-appended.dtb missing: python3 tools/fajita-dtb.py extract YOUR_STOCK_boot.img dtb (FAJITA.md step 0)"
	[[ -f xnu/out/kernel.development.sweet ]] || cmd_kernel
	mkdir -p shim/out
	python3 tools/mkadt.py shim/out/fajita.adt --highmem
	python3 tools/mkpayload.py xnu/out/kernel.development.sweet shim/out/fajita.adt shim/out/payload-test.bin \
		--cmdline "debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 wdt=-1 sweetlog=0 sweetusb=2 sweetgpu=0" >/dev/null
	shim/build.sh ../shim/out/payload-test.bin >/dev/null
	cp shim/out/shim-boot.img shim/out/shim-boot-test.img
	say "read-only test image: shim/out/shim-boot-test.img"
	cat <<-EOF
	  Boots from RAM and writes nothing (storage read-only, no log written):
	    fastboot boot shim/out/shim-boot-test.img
	  Expected end: the kernel waits for its root (userdata still holds Android).
	  Hold Power + Volume Down to get back to fastboot, then 'fastboot reboot'.
	EOF
}

# ---------------------------------------------------------------- phone
cmd_phone() {
	need "phone image" aarch64-linux-gnu-gcc mkbootimg cpio gzip python3 || exit 1
	[[ -f dtb/fajita-appended.dtb ]] || die "dtb/fajita-appended.dtb missing: python3 tools/fajita-dtb.py extract YOUR_STOCK_boot.img dtb (FAJITA.md step 0)"
	[[ -f xnu/out/kernel.development.sweet ]] || cmd_kernel
	[[ -f research/ipsw-x/d22ap.adt ]] || warn "no research/ipsw-x/d22ap.adt (run ./fajita.sh ipsw): iOS will not know it is an iPhone X"
	mkdir -p shim/out
	python3 tools/mkadt.py shim/out/fajita.adt --highmem
	local gpu=${SWEETGPU:-0} blobs=() accelargs=""
	if [[ $gpu != 0 ]]; then
		[[ -f $B/gpu-fw/a630_zap.mbn ]] || die "SWEETGPU=$gpu needs the GPU firmware: ./fajita.sh gpu-fw"
		blobs=(--blob "GPU-SQE=$B/gpu-fw/a630_sqe.fw" --blob "GPU-GMU=$B/gpu-fw/a630_gmu.bin" --blob "GPU-ZAP=$B/gpu-fw/a630_zap.mbn")
		say "GPU bring-up level sweetgpu=$gpu (firmware from build/gpu-fw)"
	fi
	# The IOAcceleratorES / Metal hook is independent of the GPU hardware:
	# SWEETACCEL=1 (with SWEETGPU=0) publishes it so Metal loads tools/sweetmtl
	# with no Adreno bring-up, to check the load path in isolation.
	if [[ ${SWEETACCEL:-0} != 0 ]]; then
		accelargs=" sweetaccel=1 sweetmtl=${SWEETMTL:-../../../usr/lib/sweetmtl/SweetMTLDriver} sweetmtlclass=${SWEETMTLCLASS:-SweetMTLDevice}"
		say "IOAcceleratorES on, Metal plugin ${SWEETMTL:-../../../usr/lib/sweetmtl/SweetMTLDriver} (sweetgpu=$gpu)"
	fi
	python3 tools/mkpayload.py xnu/out/kernel.development.sweet shim/out/fajita.adt shim/out/payload.bin ${blobs[@]+"${blobs[@]}"} \
		--cmdline "debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 sweetusb=2 sweetgpu=$gpu cs_enforcement_disable=1 sweetenv=backboardd:DYLD_INSERT_LIBRARIES=/usr/lib/sweetgl.dylib$accelargs" >/dev/null
	shim/build.sh ../shim/out/payload.bin >/dev/null
	say "phone boot image: shim/out/shim-boot.img"
	cat <<-EOF
	  First, without iOS (writes nothing):   shim/build.sh && fastboot boot shim/out/shim-boot.img
	  With iOS (ERASES userdata; back up first, FAJITA.md step 0):
	    fastboot -S 512M flash userdata build/ios16-root.simg
	    fastboot boot shim/out/shim-boot.img
	EOF
}

case ${1:-help} in
deps)       cmd_deps ;;
kernel)     shift; cmd_kernel "$@" ;;
ipsw)       shift; cmd_ipsw "$@" ;;
root)       cmd_root ;;
qemu-smoke) cmd_qemu_smoke ;;
qemu-ios)   cmd_qemu_ios ;;
gpu-fw)     shift; cmd_gpu_fw "$@" ;;
phone)      cmd_phone ;;
phone-test) cmd_phone_test ;;
all)        cmd_deps || die "install the missing tools first"
            cmd_kernel; cmd_ipsw; cmd_root; cmd_qemu_smoke; cmd_qemu_ios ;;
*)          sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//' ;;
esac
