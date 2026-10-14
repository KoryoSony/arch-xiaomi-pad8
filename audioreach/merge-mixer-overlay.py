#!/usr/bin/env python3
# Merges Xiaomi's mixer_paths_overlay_static.xml into the PAL mixer paths, as
# Xiaomi's Android audio HAL does (rules from the overlay's header): its
# top-level <ctl> defaults override (or add to) the common file's, and its
# top-level <path>s replace same-named paths (or are added). The overlay holds
# the SIA91xx speaker amp controls: without it only one of the four speakers
# is unmuted.  Usage: merge-mixer-overlay.py <mixer_paths> <overlay> <out>
import sys
import xml.etree.ElementTree as ET

base = ET.parse(sys.argv[1])
root = base.getroot()
over = ET.parse(sys.argv[2]).getroot()

for el in over:
    if el.tag not in ("ctl", "path"):
        continue
    name = el.get("name")
    match = [i for i, e in enumerate(root) if e.tag == el.tag and e.get("name") == name]
    if match:
        root[match[0]] = el
    elif el.tag == "ctl":
        # defaults come before the paths
        first_path = next((i for i, e in enumerate(root) if e.tag == "path"), len(root))
        root.insert(first_path, el)
    else:
        root.append(el)

ET.indent(base, "    ")
base.write(sys.argv[3], encoding="ISO-8859-1", xml_declaration=True)
