"""Convert the public-domain X.Org misc-fixed BDFs into terminal glyph tables."""
from pathlib import Path
import re
import sys

folder, output = map(Path, sys.argv[1:])
fonts = [(6, 10), (5, 8), (8, 13)]
data = ["/* Generated from the public-domain X.Org misc-fixed BDFs. */"]
for width, height in fonts:
    name = f"{width}x{height}"
    glyphs = {}
    for block in (folder / f"{name}.bdf").read_text().split("STARTCHAR ")[1:]:
        code = int(re.search(r"^ENCODING (-?\d+)", block, re.M)[1])
        if not (32 <= code <= 255 or 0x2500 <= code <= 0x259f):
            continue
        bitmap = block.split("BITMAP\n")[1].split("ENDCHAR")[0].splitlines()
        if len(bitmap) != height or any(len(row) != 2 for row in bitmap):
            raise SystemExit(f"Unexpected glyph dimensions in {name}: {code}")
        glyphs[code] = ", ".join("0x" + row.lower() for row in bitmap)
    for table, first, last in (("latin", 32, 256), ("line", 0x2500, 0x25a0)):
        entries = (f"    [{c - first}] = {{{glyphs[c]}}},"
                   for c in range(first, last) if c in glyphs)
        data.extend([f"static const uint8_t {table}_{name}[{last - first}][{height}] = {{",
                     *entries, "};"])
output.write_text("\n".join(data) + "\n")
