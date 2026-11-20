"""Convert the public-domain 6x10 BDF into a small terminal bitmap table."""
from pathlib import Path
import re
import sys

source, output = map(Path, sys.argv[1:])
glyphs = {}
for block in source.read_text().split("STARTCHAR ")[1:]:
    code = int(re.search(r"^ENCODING (-?\d+)", block, re.M)[1])
    if not (32 <= code <= 255 or 0x2500 <= code <= 0x259f):
        continue
    bitmap = block.split("BITMAP\n")[1].split("ENDCHAR")[0].splitlines()
    if len(bitmap) != 10:
        raise SystemExit(f"Expected ten rows for {code}")
    glyphs[code] = ", ".join("0x" + row.lower() for row in bitmap)

data = ["/* Generated from the public-domain X.Org misc-fixed 6x10 BDF. */",
        "static const UInt8 latin_glyphs[224][10] = {"]
data += [f"    [{c - 32}] = {{{rows}}}," for c, rows in glyphs.items() if c < 256]
data += ["};", "static const UInt8 line_glyphs[160][10] = {"]
data += [f"    [{c - 0x2500}] = {{{rows}}}," for c, rows in glyphs.items() if c >= 0x2500]
output.write_text("\n".join(data + ["};", ""]))
