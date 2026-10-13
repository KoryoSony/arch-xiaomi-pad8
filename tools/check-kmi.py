#!/usr/bin/env python3
"""Check that a kernel build can load a set of prebuilt (stock) modules.

With CONFIG_MODVERSIONS the kernel refuses a module when the CRC recorded in
its __versions section differs from the CRC the kernel exports for that
symbol, and compares vermagic minus the release string. This reproduces both
checks offline so an incompatible kernel never reaches the device.

usage: check-kmi.py MODULE.SYMVERS OUR_MODULES_DIR STOCK_MODULES_DIR...
"""
import os
import struct
import sys

MODVERSION_ENTRY = 64          # struct modversion_info: unsigned long crc + 56-byte name


def sections(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        raise ValueError(f"{path}: not a little-endian ELF64 file")
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    hdrs = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize) for i in range(shnum)]
    strtab = hdrs[shstrndx]
    names = data[strtab[4]:strtab[4] + strtab[5]]
    out = {}
    for h in hdrs:
        name = names[h[0]:names.index(b"\0", h[0])].decode()
        out[name] = data[h[4]:h[4] + h[5]]
    return out


def versions(secs):
    raw = secs.get("__versions", b"")
    for off in range(0, len(raw), MODVERSION_ENTRY):
        crc, = struct.unpack_from("<Q", raw, off)
        name = raw[off + 8:off + MODVERSION_ENTRY].split(b"\0", 1)[0].decode()
        yield name, crc & 0xFFFFFFFF


def modinfo(secs, key):
    for item in secs.get(".modinfo", b"").split(b"\0"):
        if item.startswith(key.encode() + b"="):
            return item.split(b"=", 1)[1].decode()
    return None


def kos(root):
    for d, _, files in os.walk(root):
        for f in files:
            if f.endswith(".ko"):
                yield os.path.join(d, f)


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    symvers_path, ours, *stock_dirs = sys.argv[1:]

    exported = {}
    with open(symvers_path) as f:
        for line in f:
            crc, sym, *_ = line.split("\t")
            exported[sym] = int(crc, 16)

    # Symbols exported by stock vendor modules satisfy each other.
    stock = [p for d in stock_dirs for p in kos(d)]
    if not stock:
        sys.exit("no stock modules found")
    vendor_exports = set()
    for p in stock:
        strings = sections(p).get("__ksymtab_strings", b"")
        vendor_exports.update(s.decode() for s in strings.split(b"\0") if s)

    our_magic = next((modinfo(sections(p), "vermagic") for p in kos(ours)), None)
    if our_magic is None:
        sys.exit(f"no modules in {ours}")
    our_flags = our_magic.split(" ", 1)[1]

    bad = 0
    for p in sorted(stock):
        secs = sections(p)
        name = os.path.basename(p)
        magic = modinfo(secs, "vermagic") or ""
        if magic.split(" ", 1)[-1] != our_flags:
            print(f"VERMAGIC {name}: '{magic}' vs ours '{our_magic}'")
            bad += 1
        for sym, crc in versions(secs):
            if sym in exported:
                if exported[sym] != crc:
                    print(f"CRC      {name}: {sym} stock={crc:#010x} ours={exported[sym]:#010x}")
                    bad += 1
            elif sym not in vendor_exports and sym != "module_layout":
                print(f"MISSING  {name}: {sym}")
                bad += 1

    print(f"checked {len(stock)} stock modules against {len(exported)} exported symbols: "
          f"{'OK' if not bad else f'{bad} problem(s)'}")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
