#!/usr/bin/env python3
"""Split an Android sparse image into several sparse images that can be
flashed one after another to the same partition (fastboot does the same
internally when an image exceeds its download size). Each part holds a run of
the original chunks; the blocks before and after it are DONT_CARE, so flashing
a part leaves the rest of the partition alone.

Usage: sparse-split.py userdata.img MAX_MB out-prefix
       -> out-prefix.part1.img, out-prefix.part2.img, ...
"""
import struct
import sys

HDR = struct.Struct('<IHHHHIIII')      # magic, major, minor, file_hdr_sz, chunk_hdr_sz, blk_sz, total_blks, total_chunks, csum
CHUNK = struct.Struct('<HHII')         # type, reserved, chunk_sz (blocks), total_sz (bytes incl. header)
MAGIC = 0xED26FF3A
RAW, FILL, DONT_CARE, CRC32 = 0xCAC1, 0xCAC2, 0xCAC3, 0xCAC4


def read_chunks(f, hdr):
    _, _, _, fhs, chs, blk, total_blks, total_chunks, _ = hdr
    f.seek(fhs)
    pos = 0
    for _ in range(total_chunks):
        ctype, _, nblk, tsz = CHUNK.unpack(f.read(CHUNK.size))
        f.seek(chs - CHUNK.size, 1)
        data_off = f.tell()
        dsz = tsz - chs
        f.seek(dsz, 1)
        yield ctype, pos, nblk, data_off, dsz
        pos += nblk


def write_part(path, src, hdr, chunks, total_blks):
    _, major, minor, _, _, blk, _, _, _ = hdr
    out = []   # (type, nblk, data_off, dsz)
    first = chunks[0][1]
    last = chunks[-1][1] + chunks[-1][2]
    if first:
        out.append((DONT_CARE, first, None, 0))
    out += [(t, n, o, d) for t, _, n, o, d in chunks]
    if last < total_blks:
        out.append((DONT_CARE, total_blks - last, None, 0))
    with open(path, 'wb') as w:
        w.write(HDR.pack(MAGIC, major, minor, HDR.size, CHUNK.size, blk, total_blks, len(out), 0))
        for t, n, o, d in out:
            w.write(CHUNK.pack(t, 0, n, CHUNK.size + d))
            if d:
                src.seek(o)
                left = d
                while left:
                    buf = src.read(min(left, 16 << 20))
                    w.write(buf)
                    left -= len(buf)


def main():
    img, max_mb, prefix = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    limit = max_mb << 20
    with open(img, 'rb') as f:
        hdr = HDR.unpack(f.read(HDR.size))
        if hdr[0] != MAGIC:
            sys.exit(f'{img}: not an Android sparse image')
        total_blks = hdr[6]
        parts, cur, size = [], [], 0
        for c in read_chunks(f, hdr):
            if cur and size + c[4] + CHUNK.size > limit:
                parts.append(cur)
                cur, size = [], 0
            cur.append(c)
            size += c[4] + CHUNK.size
        if cur:
            parts.append(cur)
        for i, p in enumerate(parts, 1):
            path = f'{prefix}.part{i}.img'
            write_part(path, f, hdr, p, total_blks)
            print(path)


if __name__ == '__main__':
    main()
