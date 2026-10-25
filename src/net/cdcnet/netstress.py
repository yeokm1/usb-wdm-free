#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Bounded localhost-only TCP pattern source and UDP sink for NETLOAD.EXE."""
import socket
import threading
import time

done = threading.Event()

def udp_sink():
    packets = 0
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        udp.bind(('127.0.0.1', 44343))
        udp.settimeout(0.2)
        while not done.is_set():
            try:
                data, _ = udp.recvfrom(2048)
                if len(data) != 1400 or any(b != data[0] for b in data):
                    raise RuntimeError('UDP data mismatch')
                packets += 1
            except socket.timeout:
                continue
    print(f'UDP received {packets} complete 1400-byte datagrams', flush=True)

worker = threading.Thread(target=udp_sink)
worker.start()
try:
    with socket.socket() as server:
        server.bind(('127.0.0.1', 44344))
        server.listen(1)
        server.settimeout(180)
        print('READY TCP44344 UDP44343 localhost only', flush=True)
        connection, _ = server.accept()
        with connection:
            connection.settimeout(5)
            total, deadline = 0, time.monotonic() + 60
            try:
                while time.monotonic() < deadline:
                    data = bytes(((total + i) * 37 + 11) & 255 for i in range(1460))
                    connection.sendall(data)
                    total += len(data)
                    if total % (1460 * 16) == 0:
                        print(f'TCP sent {total}', flush=True)
                    time.sleep(0.01)
            except (ConnectionError, TimeoutError, socket.timeout) as error:
                print(f'TCP ended {type(error).__name__}', flush=True)
            print(f'TCP total sent {total}', flush=True)
finally:
    done.set()
    worker.join()
