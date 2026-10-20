#!/usr/bin/env python3
"""OnePlus 6T (fajita) device-tree helper.

extract  BOOT.img [OUTDIR]
    Split the device trees appended to the kernel of a stock 6T boot.img
    (Image.gz-dtb, header v0/v1; a v2 dtb section is handled too) and write
    OUTDIR/fajita-appended.dtb, the blob shim/build.sh appends to the shim.
    The SDM845 ABL picks the DTB whose qcom,msm-id/board-id match the phone,
    so ship them all, unmodified.

check    LIVE.dtb
    Check the FDT the phone was really booted with against the constants of
    this port (memory banks, reserved-memory, splash). Get it from a booted
    Android or recovery with root:
        adb shell su -c 'cat /sys/firmware/fdt' > live.dtb      (or adb pull)

info     FILE.dtb
    Print model, compatible, qcom,msm-id/board-id of each DTB in a file.
"""
import os, struct, sys, zlib

FDT_MAGIC = 0xd00dfeed

# What the port assumes (shim/console.h, tools/mkadt.py)
SPLASH = (0x9d400000, 0x02400000)
FB_BYTES = 1080 * 2340 * 4
DEFAULT_HIGH = (0x100000000, 0xc0000000)


def split_dtbs(blob):
    out, i = [], 0
    while True:
        i = blob.find(struct.pack('>I', FDT_MAGIC), i)
        if i < 0 or i + 8 > len(blob):
            return out
        size = struct.unpack_from('>I', blob, i + 4)[0]
        if 0x40 <= size <= len(blob) - i:
            out.append(blob[i:i + size])
            i += size
        else:
            i += 4


def boot_kernel_and_dtb(img):
    if img[:8] != b'ANDROID!':
        raise SystemExit('not an Android boot image (no ANDROID! magic)')
    ksize, kaddr, rsize, raddr, ssize, saddr, tags, page = struct.unpack_from('<8I', img, 8)
    hver = struct.unpack_from('<I', img, 40)[0]
    if 3 <= hver < 16:
        raise SystemExit(f'boot.img header v{hver}: not a 6T stock image (v3+ keep the DTB in vendor_boot)')
    pages = lambda n: (n + page - 1) // page * page
    koff = page
    kernel = img[koff:koff + ksize]
    dtb = b''
    if hver == 2:
        # v2: recovery_dtbo (v1) fields then dtb_size at 1648
        dtb_size = struct.unpack_from('<I', img, 1648)[0]
        rdo_size = struct.unpack_from('<I', img, 1632)[0]
        off = page + pages(ksize) + pages(rsize) + pages(ssize) + pages(rdo_size)
        dtb = img[off:off + dtb_size]
    print(f'boot.img header v{hver if hver < 16 else 0}, page {page}, kernel {ksize} bytes, ramdisk {rsize}')
    return kernel, dtb


def appended(kernel):
    if kernel[:2] == b'\x1f\x8b':
        d = zlib.decompressobj(16 + zlib.MAX_WBITS)
        image = d.decompress(kernel)
        tail = d.unused_data
        print(f'kernel: gzip, Image {len(image)} bytes, {len(tail)} bytes after the stream')
        return tail
    if kernel[56:60] == b'ARM\x64':
        # image_size includes .bss, so it can point past the appended DTBs:
        # scan the whole kernel instead (split_dtbs checks every candidate).
        print('kernel: raw Image, scanning it for appended DTBs')
        return kernel
    raise SystemExit('kernel is neither gzip nor an arm64 Image')


# ---- minimal FDT reader ----
def fdt_parse(b):
    magic, total, off_struct, off_strings = struct.unpack_from('>4I', b, 0)
    if magic != FDT_MAGIC:
        raise ValueError('bad FDT magic')
    strings = b[off_strings:]
    name = lambda o: strings[o:strings.index(b'\0', o)].decode()
    root = {'name': '', 'props': {}, 'children': []}
    stack, i = [], off_struct
    node = None
    while True:
        tok = struct.unpack_from('>I', b, i)[0]
        i += 4
        if tok == 1:  # BEGIN_NODE
            end = b.index(b'\0', i)
            n = {'name': b[i:end].decode(), 'props': {}, 'children': []}
            i = (end + 4) & ~3
            if node is None:
                node = root
                root['name'] = n['name']
            else:
                node['children'].append(n)
                stack.append(node)
                node = n
        elif tok == 2:  # END_NODE
            node = stack.pop() if stack else None
        elif tok == 3:  # PROP
            ln, no = struct.unpack_from('>2I', b, i)
            i += 8
            node['props'][name(no)] = b[i:i + ln]
            i = (i + ln + 3) & ~3
        elif tok == 4:
            continue
        elif tok == 9:
            return root
        else:
            raise ValueError(f'bad token {tok:#x}')


def cells(v, n):
    return [int.from_bytes(v[k:k + 4 * n], 'big') for k in range(0, len(v) - len(v) % (4 * n), 4 * n)]


def regs(node, ac=2, sc=2):
    v = node['props'].get('reg', b'')
    vals = cells(v, 1)
    out = []
    step = ac + sc
    for k in range(0, len(vals) - len(vals) % step, step):
        a = 0
        for c in vals[k:k + ac]:
            a = a << 32 | c
        s = 0
        for c in vals[k + ac:k + step]:
            s = s << 32 | c
        out.append((a, s))
    return out


def cstrs(v):
    return [x.decode(errors='replace') for x in v.split(b'\0') if x]


def info(b):
    r = fdt_parse(b)
    p = r['props']
    print(f"  model={cstrs(p.get('model', b''))} compatible={cstrs(p.get('compatible', b''))}")
    for k in ('qcom,msm-id', 'qcom,board-id', 'qcom,pmic-id'):
        if k in p:
            print(f'  {k} = {[hex(x) for x in cells(p[k], 1)]}')


def check(b):
    r = fdt_parse(b)
    ok = True
    mem = [n for n in r['children'] if n['name'].startswith('memory')]
    banks = [rg for n in mem for rg in regs(n)]
    print('memory banks:')
    for a, s in banks:
        print(f'  {a:#012x} + {s:#x}  ({s / 2**30:.2f} GiB)')
    hb, hs = DEFAULT_HIGH
    fits = any(a <= hb and hb + hs <= a + s for a, s in banks)
    print(f"kernel RAM {hb:#x} + {hs:#x}: {'inside a bank' if fits else 'NOT inside one bank'}")
    ok &= fits
    rm = next((n for n in r['children'] if n['name'] == 'reserved-memory'), None)
    clash = []
    splash_found = False
    if rm:
        print('reserved-memory:')
        for n in rm['children']:
            for a, s in regs(n):
                tag = ''
                if a < hb + hs and hb < a + s:
                    clash.append(n['name'])
                    tag = '   <-- overlaps the kernel RAM'
                if 'splash' in n['name'] or 'cont_splash' in n['name']:
                    splash_found = True
                    tag += '   (splash)'
                    if (a, s) != SPLASH:
                        tag += f'  != expected {SPLASH[0]:#x}+{SPLASH[1]:#x}'
                        ok = False
                    elif s < FB_BYTES:
                        ok = False
                print(f'  {n["name"]:40s} {a:#012x} + {s:#x}{tag}')
    if not splash_found:
        print('warning: no splash region in reserved-memory')
        ok = False
    if clash:
        print(f'ERROR: kernel RAM overlaps {clash}; pass a smaller --mem-size to mkadt.py')
        ok = False
    fb = [n for n in r['children'] if n['name'] == 'chosen']
    print('RESULT:', 'consistent with this port' if ok else 'MISMATCH, see above')
    return 0 if ok else 1


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    cmd, path = sys.argv[1], sys.argv[2]
    data = open(path, 'rb').read()
    if cmd == 'extract':
        outdir = sys.argv[3] if len(sys.argv) > 3 else 'dtb'
        kernel, v2dtb = boot_kernel_and_dtb(data)
        dtbs = split_dtbs(v2dtb) if v2dtb else split_dtbs(appended(kernel))
        if not dtbs:
            raise SystemExit('no device trees found')
        os.makedirs(outdir, exist_ok=True)
        for k, d in enumerate(dtbs):
            open(os.path.join(outdir, f'fajita-{k:02d}.dtb'), 'wb').write(d)
            print(f'dtb {k}: {len(d)} bytes')
            try:
                info(d)
            except Exception as e:  # noqa: BLE001
                print(f'  (unparsable: {e})')
        out = os.path.join(outdir, 'fajita-appended.dtb')
        open(out, 'wb').write(b''.join(dtbs))
        print(f'{out}: {len(dtbs)} DTBs')
        return 0
    if cmd == 'check':
        return check(data)
    if cmd == 'info':
        for k, d in enumerate(split_dtbs(data)):
            print(f'dtb {k}:')
            info(d)
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main())
