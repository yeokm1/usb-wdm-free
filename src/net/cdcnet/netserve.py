#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
"""One-shot localhost TCP peer for NETTEST.EXE; no external network access."""
import argparse
import socket

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--port', type=int, default=44342)
args = ap.parse_args()
payload = bytes((i * 37 + 11) & 255 for i in range(32768))
with socket.socket() as server:
    server.bind(('127.0.0.1', args.port))
    server.listen(1)
    server.settimeout(600)
    print(f'READY 127.0.0.1:{args.port}', flush=True)
    connection, _ = server.accept()
    with connection:
        connection.settimeout(10)
        request = b''
        while len(request) < 4:
            part = connection.recv(4 - len(request))
            if not part:
                raise RuntimeError('short request')
            request += part
        if request != b'UWF1':
            raise RuntimeError('unexpected request')
        connection.sendall(payload)
        print('SENT 32768 deterministic bytes', flush=True)
