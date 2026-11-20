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
    expansion = "\t\tBlowfish_expand0state(&state, sha2salt, shalen);\n\t\tBlowfish_expand0state(&state, sha2pass, shalen);"
    if original.count(expansion) != 1:
        raise SystemExit("The bcrypt expansion port needs updating")
    original = original.replace(expansion, """#ifdef PT_ARMLET
        pt_blowfish_expand64(&state, sha2salt);
        pt_blowfish_expand64(&state, sha2pass);
#else
        Blowfish_expand0state(&state, sha2salt, shalen);
        Blowfish_expand0state(&state, sha2pass, shalen);
#endif""")
    # The Palm scheduler runs one upstream PBKDF hash between event-loop turns.
    needle = "static void\nbcrypt_hash("
    replacement = """#ifdef PT_ARMLET
_Static_assert(offsetof(blf_ctx, P) == 4096 && sizeof(((blf_ctx *)0)->P) == 72,
               "ARM Blowfish context layout");
void pt_blowfish_expand64(blf_ctx *state, const uint8_t digest[64]);
#endif

void
bcrypt_hash("""
else:
    raise SystemExit(f"Unknown port: {kind}")

if original.count(needle) != 1:
    raise SystemExit(f"The {kind} port needs updating")
destination.write_text(original.replace(needle, replacement))
