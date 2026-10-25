#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Send an explicit HMP command to a local QEMU test guest, never the host UI.

Example: monitor.py --port 44332 'sendkey ret'
Use one command per observation/action when controlling a wizard.
"""
import argparse
import socket
import time

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--port', type=int, required=True)
ap.add_argument('command')
ap.add_argument('--screenshot', help='optional PNG capture after this action')
ap.add_argument('--text', help='type ASCII text into an already-focused guest field')
ap.add_argument('--layout', choices=('qwerty', 'dvorak'), default='qwerty')
args = ap.parse_args()
if '\n' in args.command or '\r' in args.command:
    ap.error('one HMP command per call')
with socket.create_connection(('127.0.0.1', args.port), timeout=3) as monitor:
    monitor.settimeout(3)
    data = b''
    while b'(qemu)' not in data:
        data += monitor.recv(4096)
    def command(text):
        monitor.sendall(text.encode('ascii') + b'\n')
        data = b''
        try:
            while b'(qemu)' not in data:
                block = monitor.recv(4096)
                if not block:
                    break
                data += block
        except ConnectionResetError:
            if text != 'quit':
                raise
        # Omit HMP's line-editing echo, preserve command results/errors.
        return data.decode('ascii', errors='replace').split('\r\n', 1)[-1]

    print(command(args.command))
    if args.text:
        # HMP acknowledges sendkey before its hold timer releases the keys.
        # Let a preceding shortcut finish, then keep each typed key distinct.
        time.sleep(0.25)
        keys = {' ': 'spc', '\\': 'backslash', '/': 'slash', '.': 'dot',
                '-': 'minus', ',': 'comma', '=': 'equal', ';': 'semicolon',
                "'": 'apostrophe', '[': 'bracket_left', ']': 'bracket_right',
                '`': 'grave_accent'}
        shifted = dict(zip('~!@#$%^&*()_+{}:"<>?', '`1234567890-=[];\',./'))
        inverse = dict(zip("',.pyfgcrl/=aoeuidhtns-;qjkxbmwvz[]",
                           "qwertyuiop[]asdfghjkl;'zxcvbnm,./-="))
        sequence = []
        for char in args.text:
            shift = char.isupper() or char in shifted
            base = shifted.get(char, char.lower())
            if args.layout == 'dvorak':
                base = inverse.get(base, base)
            if base.isascii() and base.isalnum():
                key = base
            elif base in keys:
                key = keys[base]
            else:
                ap.error(f'unsupported guest text character: {char!r}')
            sequence.append(('shift-' if shift else '') + key)
        for key in sequence:
            command('sendkey ' + key + ' 10')
            time.sleep(0.03)
            time.sleep(0.025)
    if args.screenshot:
        path = args.screenshot.replace('\\', '/')
        if any(ch in path for ch in '\r\n"'):
            ap.error('unsupported screenshot path')
        time.sleep(0.8)  # legacy guest dialogs often need another paint cycle
        print(command(f'screendump "{path}" -f png'))
