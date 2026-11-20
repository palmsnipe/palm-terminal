#!/usr/bin/env python3
"""Create a fresh one-time provisioning file on a trusted desktop."""
import argparse
import os
from pathlib import Path
import secrets

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("file", type=Path)
args = parser.parse_args()
with os.fdopen(os.open(args.file, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "wb") as seed:
    seed.write(secrets.token_bytes(48))
