#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Write the TFTP payload NETTEST.EXE verifies: CDCNET.BIN, 1 MiB.

Files only: this script starts nothing and opens no socket. QEMU's own
user-mode network serves the directory (-netdev user,tftp=<dir>).
"""
import sys
from pathlib import Path

out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
data = bytes(((i * 37 + 11 + (i >> 9)) & 255) for i in range(1024 * 1024))
(out / 'CDCNET.BIN').write_bytes(data)
print(out / 'CDCNET.BIN')
