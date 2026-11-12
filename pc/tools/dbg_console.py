#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Client for the PC port's live debug console (pc/hal/dbg_console.c).

Start the port with a console, then send commands:

    PC_DBG_CONSOLE=7777 ./pc/build/tokimeki-pc ./pc/assets &
    python3 pc/tools/dbg_console.py --port 7777 info
    python3 pc/tools/dbg_console.py --port 7777 'pause' 'step 30' 'peek now_step' 'shot /tmp/a.ppm'
    python3 pc/tools/dbg_console.py --port 7777          # interactive (one command per line)
    python3 pc/tools/dbg_console.py --unix /tmp/toki.sock info

Each command gets one reply line (`ok ...` or `err ...`).  The game reads
the console at its frame boundary, so a reply can take up to a frame (or the
boot movie, which is one long frame).  Exit status: 1 if any reply was err.
Commands: send `help`.
"""
import argparse
import socket
import sys
import time


def connect(args):
    deadline = time.time() + args.wait
    while True:
        try:
            if args.unix:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(args.unix)
            else:
                s = socket.create_connection(('127.0.0.1', args.port))
            return s
        except OSError:
            if time.time() > deadline:
                raise
            time.sleep(0.2)


def ask(sock, f, line, timeout):
    sock.sendall(line.strip().encode() + b'\n')
    sock.settimeout(timeout)
    reply = f.readline()
    if not reply:
        raise ConnectionError('the game closed the console')
    return reply.decode('utf-8', 'replace').rstrip('\n')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('commands', nargs='*')
    ap.add_argument('--port', type=int, default=7777)
    ap.add_argument('--unix', help='unix socket path (PC_DBG_CONSOLE=unix:PATH)')
    ap.add_argument('--wait', type=float, default=30, help='seconds to wait for the game to listen')
    ap.add_argument('--timeout', type=float, default=120, help='seconds to wait for a reply')
    a = ap.parse_args()
    sock = connect(a)
    f = sock.makefile('rb')
    bad = 0
    lines = a.commands or (l for l in sys.stdin)
    for line in lines:
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        try:
            reply = ask(sock, f, line, a.timeout)
        except (ConnectionError, socket.timeout) as e:
            print(f'err {e}')
            return 1
        print(reply, flush=True)
        bad += not reply.startswith('ok')
        if line.split()[0] == 'quit':
            break
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
