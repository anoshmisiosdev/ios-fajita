#!/usr/bin/env python3
"""Raw image -> Android sparse image, for hosts without libsparse's img2simg.

  img2simg.py RAW.img OUT.simg

All-zero 4 KiB blocks become DONT_CARE chunks, everything else RAW chunks
(format: AOSP system/core/libsparse/sparse_format.h, v1.0). fastboot on
macOS refuses a raw userdata image this large ("Failed reading from
userdata"), so build/ios16-root.img goes through this before flashing.
"""
import struct, sys

BLK = 4096
READ = 256 * BLK          # 1 MiB per read
ZERO = bytes(BLK)
RAW, DONT_CARE = 0xCAC1, 0xCAC3
MAX_RAW = 16384           # blocks per RAW chunk (64 MiB)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    chunks = 0
    total = 0
    with open(src, 'rb') as f, open(dst, 'wb') as o:
        o.write(bytes(28))  # header, rewritten at the end

        run_kind, run = None, []    # current chunk: kind and its RAW blocks / DONT_CARE count

        def flush():
            nonlocal chunks, run_kind, run
            if run_kind == RAW:
                n = len(run)
                o.write(struct.pack('<HHII', RAW, 0, n, 12 + n * BLK))
                o.writelines(run)
            elif run_kind == DONT_CARE:
                o.write(struct.pack('<HHII', DONT_CARE, 0, run, 12))
            else:
                return
            chunks += 1
            run_kind, run = None, []

        while True:
            buf = f.read(READ)
            if not buf:
                break
            if len(buf) % BLK:
                buf += bytes(BLK - len(buf) % BLK)
            for i in range(0, len(buf), BLK):
                b = buf[i:i + BLK]
                total += 1
                if b == ZERO:
                    if run_kind != DONT_CARE:
                        flush()
                        run_kind, run = DONT_CARE, 0
                    run += 1
                else:
                    if run_kind != RAW or len(run) >= MAX_RAW:
                        flush()
                        run_kind, run = RAW, []
                    run.append(b)
        flush()
        o.seek(0)
        # magic, major, minor, file_hdr_sz, chunk_hdr_sz, blk_sz, total_blks, total_chunks, crc
        o.write(struct.pack('<IHHHHIIII', 0xED26FF3A, 1, 0, 28, 12, BLK, total, chunks, 0))
    print(f'{dst}: {total} blocks, {chunks} chunks')


if __name__ == '__main__':
    main()
