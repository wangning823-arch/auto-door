#!/usr/bin/env python3
import os
import subprocess
import sys

key = os.path.expanduser("~/.ssh/id_ed25519")
host = "root@101.37.175.30"
remote = sys.argv[1] if len(sys.argv) > 1 else "echo hi"
cmd = [
    "ssh",
    "-i",
    key,
    "-o",
    "BatchMode=yes",
    "-o",
    "ConnectTimeout=8",
    "-o",
    "IdentitiesOnly=yes",
    host,
    remote,
]
print(subprocess.check_output(cmd, text=True, errors="replace"))
