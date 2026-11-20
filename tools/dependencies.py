"""Fetch and verify the exact upstream sources used in the PRC."""
import hashlib
from pathlib import Path
import sys
import tarfile
import urllib.request

SOURCES = {
    "wolfssh": ("https://codeload.github.com/wolfSSL/wolfssh/tar.gz/8221e4ee48c93da6cea3325a91de2ecc00ae5f56", "865af5d84ab91fd5356d6c103e09f08934d6b7b7e89a4d46094ed2e1ba6a9807"),
    "wolfssl": ("https://codeload.github.com/wolfSSL/wolfssl/tar.gz/3c5eead44904df64e6a5a1f4ebdce377d35a849a", "20fceb75ecac0fdb241f6d683e8cd1b579db0297f6d506d13eca9656262d72a5"),
    "vterm": ("https://www.leonerd.org.uk/code/libvterm/libvterm-0.3.3.tar.gz", "09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0"),
    "openssh": ("https://codeload.github.com/openssh/openssh-portable/tar.gz/refs/tags/V_10_6_P2", "c93873149687f9c80226dced4a4588ad7d0ad1bcd0b3220b0bb155ac836e8719"),
    "font": ("https://www.x.org/releases/individual/font/font-misc-misc-1.1.3.tar.xz", "79abe361f58bb21ade9f565898e486300ce1cc621d5285bec26e14b6a8618fed"),
}
base = Path(sys.argv[1])
base.mkdir(parents=True, exist_ok=True)
for name, (url, digest) in SOURCES.items():
    stamp = base / name / ".source"
    if stamp.exists() and stamp.read_text() == digest:
        continue
    print(f"Fetching {name}", flush=True)
    archive = base / (name + ".tar")
    with urllib.request.urlopen(url, timeout=40) as remote:
        archive.write_bytes(remote.read())
    if hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
        raise SystemExit(f"Checksum mismatch: {name}")
    with tarfile.open(archive) as source:
        for item in source:
            _, _, relative = item.name.partition("/")
            if not relative or not item.isfile():
                continue
            path = Path(relative)
            if path.is_absolute() or ".." in path.parts:
                raise SystemExit(f"Invalid archive member: {item.name}")
            output = base / name / path
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(source.extractfile(item).read())
    stamp.write_text(digest)
