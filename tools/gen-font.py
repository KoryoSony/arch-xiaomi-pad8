#!/usr/bin/env python3
"""Generate font8x16.h (256 glyphs x 16 rows) from the Linux kernel's
lib/fonts/font_8x16.c (GPL-2.0, the VGA 8x16 console font)."""
import re, sys
src = open(sys.argv[1]).read()
body = src[src.index("fontdata_8x16"):]
# Each glyph has a comment like /* 65 0x41 'A' */ whose hex must not count.
body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
data = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})\b", body)][:4096]
assert len(data) == 4096, len(data)
with open(sys.argv[2], "w") as f:
    f.write("/* Generated from linux/lib/fonts/font_8x16.c (GPL-2.0) by tools/gen-font.py */\n")
    f.write("static const unsigned char font8x16[4096] = {\n")
    for i in range(0, 4096, 16):
        f.write("\t" + ", ".join(f"0x{b:02x}" for b in data[i:i+16]) + ",\n")
    f.write("};\n")
