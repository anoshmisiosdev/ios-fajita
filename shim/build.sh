#!/bin/bash
# Build the shim for the OnePlus 6T (fajita) and wrap it like a stock 6T
# kernel: Image.gz with the device tree(s) appended (Image.gz-dtb) in a
# boot.img header v0. The SDM845 ABL picks the appended DTB whose
# qcom,msm-id / qcom,board-id match the phone and passes it in x0, so the
# DTBs must come from this phone's stock boot.img: see tools/fajita-dtb.py.
#
#   shim/build.sh [PAYLOAD]          (PAYLOAD from tools/mkpayload.py)
# Env: DTB=path (default ../dtb/fajita-appended.dtb), MKBOOTIMG=command.
set -euo pipefail
cd "$(dirname "$0")"
CC=${CC:-aarch64-linux-gnu-gcc}
OBJCOPY=${OBJCOPY:-aarch64-linux-gnu-objcopy}
NM=${NM:-aarch64-linux-gnu-nm}
READELF=${READELF:-aarch64-linux-gnu-readelf}
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
SRCS="start.S shim.c console.c mmu.c fdt.c loader.c ufs.c fajita_disp.c"
mkdir -p out
OBJS=""
for f in $SRCS; do
	$CC $CFLAGS -c $f -o out/${f%.*}.o
	OBJS="$OBJS out/${f%.*}.o"
done
# The image runs wherever ABL puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$($READELF -r ${OBJS/out\/start.o/} | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	$OBJS -o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/Image
# ABL copies the file; pad to image_size so .bss/stacks exist as zeroed bytes.
size=$((16#$($NM out/shim.elf | awk '$3=="_end"{print $1}')))
truncate -s "$size" out/Image
# Image.gz-dtb, exactly like the stock OnePlus kernel
DTB=${DTB:-../dtb/fajita-appended.dtb}
[ -f "$DTB" ] || { echo "error: $DTB missing; run tools/fajita-dtb.py on your stock boot.img" >&2; exit 1; }
gzip -9 -n -c out/Image > out/Image.gz
cat out/Image.gz "$DTB" > out/Image.gz-dtb
# Ramdisk: an xnu payload (tools/mkpayload.py) if given as $1, else an empty
# newc cpio (ABL dislikes a zero-size ramdisk on some builds) -> diagnostics.
if [ -n "${1:-}" ]; then
	cp "$1" out/ramdisk.bin
else
	(cd out && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
fi
# Offsets as the 6T stock / postmarketOS boot images (base 0, pagesize 4096).
${MKBOOTIMG:-mkbootimg} --header_version 0 --kernel out/Image.gz-dtb --ramdisk out/ramdisk.bin \
	--base 0 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 --second_offset 0xf00000 \
	--tags_offset 0x100 --pagesize 4096 \
	--os_version 11.0.0 --os_patch_level 2021-11 \
	--cmdline "console=null" -o out/shim-boot.img
# Host render of the console layout.
gcc -O1 -Wall -o out/host_test console.c host_test.c && ./out/host_test
ls -l out/Image out/Image.gz-dtb out/shim-boot.img
