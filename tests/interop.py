"""Verify OpenSSH compatibility and rejection of damaged key inputs."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

host = Path(sys.argv[1]).resolve()
run = subprocess.check_output
with tempfile.TemporaryDirectory() as scratch:
    temp = Path(scratch)
    for cipher, passphrase in (("aes256-ctr", "import-pass"),
                               ("aes256-gcm@openssh.com", "gcm-pass"),
                               ("aes256-ctr", "")):
        key = temp / f"input-{len(passphrase)}"
        run(["ssh-keygen", "-q", "-t", "ed25519", "-N", passphrase,
             "-Z", cipher, "-f", str(key)])
        saved = Path(str(key) + ".saved")
        public = run([host, "import", key, passphrase, saved, "private-test"]).split()
        assert public[:2] == Path(str(key) + ".pub").read_bytes().split()[:2]
        reopened = run(["ssh-keygen", "-y", "-P", "private-test", "-f", saved]).split()
        assert public[:2] == reopened[:2]
        if passphrase:
            bad = subprocess.run([host, "import", key, "wrong", saved, "private-test"],
                                 capture_output=True)
            assert bad.returncode != 0
            assert run(["ssh-keygen", "-y", "-P", "private-test", "-f", saved]).split() == reopened
        truncated = temp / "truncated"
        truncated.write_bytes(key.read_bytes()[:100])
        assert subprocess.run([host, "import", truncated, passphrase, saved, "private-test"],
                              capture_output=True).returncode != 0
    generated = temp / "generated"
    public = run([host, "generate", generated, "palm-generated"]).split()
    assert public[:2] == run(["ssh-keygen", "-y", "-P", "palm-generated",
                              "-f", generated]).split()[:2]
    assert os.stat(generated).st_mode & 0o777 == 0o600
print("Verified plaintext, CTR and GCM Ed25519 import, encrypted save, generation, and rejection")
