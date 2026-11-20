"""Package an ARMv4T image and its literal-address relocations."""
from pathlib import Path
import re
import struct
import subprocess
import sys

CHUNK = 60000
mode, *args = sys.argv[1:]
if mode == "pack":
    readelf, elf, binary, out = args
    def elf_info(option):
        return subprocess.check_output([readelf, option, elf], text=True)
    if not re.search(r"Entry point address:\s+0x0\s*$", elf_info("-h"), re.M):
        raise SystemExit("ARM entry must be at offset zero")
    if "Tag_CPU_arch: v4T" not in elf_info("-A"):
        raise SystemExit("ARM target must be ARMv4T")
    data = Path(binary).read_bytes()
    offsets = set()
    relative = {"REL32", "CALL", "JUMP24", "PC24", "V4BX", "PREL31"}
    relocs = re.search(r"Relocation section '\.rel\.image'.*?(?=\nRelocation section|\Z)",
                       elf_info("-rW"), re.S)
    if not relocs:
        raise SystemExit("ARM image lacks a relocation table")
    for offset, kind in re.findall(r"^([0-9a-f]+)\s+[0-9a-f]+\s+R_ARM_(\w+)", relocs[0], re.M):
        offset = int(offset, 16)
        if kind == "ABS32":
            if offset % 4 or offset + 4 > len(data):
                raise SystemExit(f"Invalid ARM relocation offset {offset:#x}")
            target, = struct.unpack_from("<I", data, offset)
            if (target & ~1) >= len(data):
                raise SystemExit(f"ARM relocation target outside image: {target:#x}")
            offsets.add(offset)
        elif kind not in relative:
            raise SystemExit(f"Unsupported ARM relocation {kind}")
    chunks = [data[i:i + CHUNK] for i in range(0, len(data), CHUNK)]
    meta = struct.pack(">III", len(data), len(chunks), len(offsets))
    meta += b"".join(struct.pack(">I", offset) for offset in sorted(offsets))
    if len(meta) > CHUNK:
        raise SystemExit("ARM relocation resource is too large")
    folder = Path(out)
    lines = []
    for number, chunk in enumerate([meta, *chunks]):
        name = f"arm-part-{number}.bin"
        (folder / name).write_bytes(chunk)
        lines.append(f'DATA "armc" ID {number} "{name}"')
    (folder / "arm-resources.rcp").write_text("\n".join(lines) + "\n")
elif mode == "verify":
    prc, binary = map(Path, args)
    data = prc.read_bytes()
    count, = struct.unpack_from(">H", data, 76)
    entries = [struct.unpack_from(">4sHI", data, 78 + i * 10) for i in range(count)]
    ends = sorted([entry[2] for entry in entries] + [len(data)])
    resources = {number: data[start:ends[ends.index(start) + 1]]
                 for kind, number, start in entries if kind == b"armc"}
    length, parts, relocations = struct.unpack_from(">III", resources[0])
    image = b"".join(resources[i] for i in range(1, parts + 1))
    if (len(image) != length or image != binary.read_bytes() or
            len(resources[0]) != 12 + 4 * relocations or
            any(len(part) > CHUNK for part in resources.values())):
        raise SystemExit("Packaged ARM resources failed verification")
    print(f"Verified {len(image)}-byte ARM image and {relocations} relocations")
else:
    raise SystemExit(f"Unknown mode: {mode}")
