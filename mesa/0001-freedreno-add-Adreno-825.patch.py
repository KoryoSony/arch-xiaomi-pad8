#!/usr/bin/env python3
"""Add the Adreno 825 (Snapdragon 8s Gen 4, KGSL chip id 0x44030000) to Mesa's
freedreno device table, sharing the Adreno 829 entry: same 0x4403 core.
Applied by mesa/build-mesa.sh (kept as a script so it survives upstream edits)."""
import sys
p = sys.argv[1]
s = open(p).read()
anchor = 'GPUId(chip_id=0x44030a20, name="Adreno (TM) 829"), # KGSL'
assert anchor in s, "Adreno 829 entry not found; update the patch"
if "0x44030000" not in s:
    s = s.replace(anchor, anchor + '\n        GPUId(chip_id=0x44030000, name="Adreno (TM) 825"), # KGSL, yupei')
open(p, "w").write(s)
print("Adreno 825 entry added")
