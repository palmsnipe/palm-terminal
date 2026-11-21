"""Check that code0001 contains the relocations needed when Palm OS loads it."""
from pathlib import Path
import struct
import subprocess
import sys

readelf, elf, binary = sys.argv[1:]
code = Path(binary).read_bytes()
symbols = {}
needed_symbols = {"__Startup__", "__reloc_start_offset", "__reloc_end_offset"}
for line in subprocess.check_output([readelf, "-sW", elf], text=True).splitlines():
    fields = line.split()
    if len(fields) == 8 and fields[7] in needed_symbols:
        symbols[fields[7]] = int(fields[1], 16)

base = symbols["__Startup__"]
table_start = symbols["__reloc_start_offset"]
table_end = symbols["__reloc_end_offset"]
if not 0 <= table_start <= table_end <= len(code) or (table_end - table_start) % 4:
    raise SystemExit("Invalid 68K runtime relocation table bounds")
installed = {entry for entry, in struct.iter_unpack(">I", code[table_start:table_end])}

required = set()
in_code = False
for line in subprocess.check_output([readelf, "-rW", elf], text=True).splitlines():
    if line.startswith("Relocation section "):
        in_code = line.split("'")[1] in {".rel.vec", ".rela.vec", ".rel.text", ".rela.text"}
    fields = line.split()
    if in_code and len(fields) >= 3 and fields[2] == "R_68K_32":
        offset = int(fields[0], 16) - base
        target, = struct.unpack_from(">I", code, offset)
        if base <= target < base + len(code):
            required.add(offset)

missing = required - installed
if missing:
    offsets = ", ".join(hex(offset) for offset in sorted(missing))
    raise SystemExit(f"Missing 68K runtime relocations at code offsets {offsets}")
print(f"Verified {len(required)} 68K runtime relocations")
