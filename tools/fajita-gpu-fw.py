#!/usr/bin/env python3
"""Collect the 6T's Adreno 630 firmware into build/gpu-fw/ for mkpayload --blob.

  tools/fajita-gpu-fw.py SRC_DIR [OUT_DIR]

SRC_DIR holds files pulled off your own phone (./fajita.sh gpu-fw prints the
adb commands): a630_sqe.fw, a630_gmu.bin and the zap shader, either already
squashed (a630_zap.mbn) or split (a630_zap.mdt + a630_zap.b00, .b01, ...).
A split zap shader is squashed into one ELF: each segment file .bNN goes to
program header NN's p_offset, as linux-msm's pil-squasher does.

Output (OUT_DIR, default build/gpu-fw): a630_sqe.fw, a630_gmu.bin,
a630_zap.mbn. These are Qualcomm binaries: never commit them.
"""
import os, shutil, struct, sys


def die(msg):
    sys.exit(f'fajita-gpu-fw: {msg}')


def phdrs(elf):
    """(p_offset, p_filesz) of each program header of an ELF32/ELF64 file."""
    if elf[:4] != b'\x7fELF':
        die('zap .mdt/.mbn is not an ELF file')
    if elf[4] == 1:
        phoff, = struct.unpack_from('<I', elf, 0x1c)
        phentsize, phnum = struct.unpack_from('<HH', elf, 0x2a)
        return [(struct.unpack_from('<I', elf, phoff + i * phentsize + 4)[0],
                 struct.unpack_from('<I', elf, phoff + i * phentsize + 16)[0]) for i in range(phnum)]
    if elf[4] == 2:
        phoff, = struct.unpack_from('<Q', elf, 0x20)
        phentsize, phnum = struct.unpack_from('<HH', elf, 0x36)
        return [(struct.unpack_from('<Q', elf, phoff + i * phentsize + 8)[0],
                 struct.unpack_from('<Q', elf, phoff + i * phentsize + 32)[0]) for i in range(phnum)]
    die('unknown ELF class')


def squash(mdt_path):
    mdt = open(mdt_path, 'rb').read()
    base = mdt_path[:-4]
    out = bytearray(mdt)
    for i, (off, size) in enumerate(phdrs(mdt)):
        if size == 0:
            continue
        seg_path = f'{base}.b{i:02d}'
        if not os.path.exists(seg_path):
            # The header segment(s) can already be inside the .mdt itself.
            if off + size <= len(mdt):
                continue
            die(f'missing {os.path.basename(seg_path)}')
        seg = open(seg_path, 'rb').read()
        if len(seg) != size:
            die(f'{os.path.basename(seg_path)} is {len(seg)} bytes, program header {i} says {size}')
        if len(out) < off + size:
            out.extend(b'\0' * (off + size - len(out)))
        out[off:off + size] = seg
    return bytes(out)


def main():
    if len(sys.argv) not in (2, 3):
        die('usage: fajita-gpu-fw.py SRC_DIR [OUT_DIR]')
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) == 3 else os.path.join(os.path.dirname(__file__), '..', 'build', 'gpu-fw')
    os.makedirs(out, exist_ok=True)

    sqe = os.path.join(src, 'a630_sqe.fw')
    gmu = os.path.join(src, 'a630_gmu.bin')
    for p in (sqe, gmu):
        if not os.path.exists(p):
            die(f'missing {p}')
    sqe_data = open(sqe, 'rb').read()
    gmu_size = os.path.getsize(gmu)
    if gmu_size > 0x8000:
        die(f'a630_gmu.bin is {gmu_size} bytes; the legacy GMU ITCM holds 0x8000')
    # dword 0 is a header the kernel does not upload (mainline adreno_fw_create_bo);
    # the version is the low 12 bits of dword 1.
    ver = struct.unpack_from('<I', sqe_data, 4)[0] & 0xfff
    print(f'a630_sqe.fw: {len(sqe_data)} bytes, version {ver:#x}'
          f'{"" if ver >= 0x190 else " (older than 1.90: mainline Linux would refuse it)"}')
    print(f'a630_gmu.bin: {gmu_size} bytes')
    shutil.copyfile(sqe, os.path.join(out, 'a630_sqe.fw'))
    shutil.copyfile(gmu, os.path.join(out, 'a630_gmu.bin'))

    mbn = os.path.join(src, 'a630_zap.mbn')
    mdt = os.path.join(src, 'a630_zap.mdt')
    if os.path.exists(mbn):
        zap = open(mbn, 'rb').read()
        phdrs(zap)
    elif os.path.exists(mdt):
        zap = squash(mdt)
    else:
        die('missing a630_zap.mbn or a630_zap.mdt')
    open(os.path.join(out, 'a630_zap.mbn'), 'wb').write(zap)
    print(f'a630_zap.mbn: {len(zap)} bytes')
    print(f'-> {os.path.abspath(out)}')


if __name__ == '__main__':
    main()
