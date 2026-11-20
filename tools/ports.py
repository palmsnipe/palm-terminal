"""Write the Palm adaptations of the pinned upstream sources."""
from pathlib import Path
import sys

kind, deps, destination = sys.argv[1:]
deps = Path(deps)
destination = Path(destination)
if kind == "ssh":
    original = (deps / "wolfssh/src/internal.c").read_text()
    needle = "        GetTerminalInfo(&w, &h, &pxW, &pxH, &term);"
    replacement = """        extern void pt_terminal_dimensions(word32*, word32*, word32*, word32*);
        pt_terminal_dimensions(&w, &h, &pxW, &pxH);
        term = "xterm";"""
elif kind == "bcrypt":
    original = (deps / "openssh/openbsd-compat/bcrypt_pbkdf.c").read_text()
    # The Palm scheduler runs one upstream PBKDF hash between event-loop turns.
    needle = "static void\nbcrypt_hash("
    replacement = "void\nbcrypt_hash("
else:
    raise SystemExit(f"Unknown port: {kind}")

if original.count(needle) != 1:
    raise SystemExit(f"The {kind} port needs updating")
destination.write_text(original.replace(needle, replacement))
