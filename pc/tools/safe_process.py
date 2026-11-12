#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded process runner for development tools (Linux; a Windows Python
runs the thread-based variant, _run_windows).

Output is merged in arrival order. A supplied log keeps the bounded prefix;
the returned stdout then contains only its last 64 KiB. Without a log, stdout
contains the bounded output. Limits are failures, never successful truncation.
"""
import argparse
from dataclasses import dataclass
import math
import os
from pathlib import Path
import selectors
import signal
import subprocess
import sys
import tempfile
import time

OUTPUT_LIMIT = 8 * 1024 * 1024
FILE_LIMIT = 64 * 1024 * 1024


@dataclass
class Result:
    returncode: int
    stdout: str
    reason: str = None
    stderr: str = ''

    @property
    def ok(self):
        return self.reason is None and self.returncode == 0

    def require_ok(self):
        if not self.ok:
            raise RuntimeError(f'process failed: {self.reason or self.returncode}\n'
                               + self.stdout[-2000:])
        return self


def run(cmd, *, cwd=None, env=None, timeout=60, max_output=OUTPUT_LIMIT,
        log_path=None, stop=None, watch_files=()):
    """watch_files: (path, byte_limit, failure_label), polled every 50 ms."""
    if not math.isfinite(timeout) or timeout <= 0 or max_output <= 0:
        raise ValueError('timeout and output limit must be positive and finite')
    if os.name == 'nt':
        return _run_windows(cmd, cwd=cwd, env=env, timeout=timeout, max_output=max_output,
                            log_path=log_path, stop=stop, watch_files=watch_files)
    # A Windows .exe (PC_TEST_PLATFORM=windows, or one named explicitly)
    # runs under Wine in a private prefix (game_platform.py).
    if os.path.dirname(os.path.abspath(__file__)) not in sys.path:
        sys.path.append(os.path.dirname(os.path.abspath(__file__)))
    import game_platform
    cmd, env, cleanup = game_platform.wrap(cmd, env)
    try:
        return _run_posix(cmd, cwd=cwd, env=env, timeout=timeout, max_output=max_output,
                          log_path=log_path, stop=stop, watch_files=watch_files)
    finally:
        cleanup()


def _run_posix(cmd, *, cwd, env, timeout, max_output, log_path, stop, watch_files):
    deadline = time.monotonic() + timeout
    total, reason = 0, None

    def stopped():
        if stop is not None and stop.is_set():
            return 'CANCELLED'
        if time.monotonic() >= deadline:
            return 'TIMEOUT'
        for path, limit, label in watch_files:
            if os.path.exists(path) and os.path.getsize(path) > limit:
                return label
        return None

    if log_path is not None:
        Path(log_path).parent.mkdir(parents=True, exist_ok=True)
    with (open(log_path, 'w+b') if log_path is not None
          else tempfile.TemporaryFile()) as log:
        with subprocess.Popen(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT,
                              start_new_session=True) as proc:
            try:
                with selectors.DefaultSelector() as selector:
                    selector.register(proc.stdout, selectors.EVENT_READ)
                    while selector.get_map() or proc.poll() is None:
                        reason = stopped()
                        if reason:
                            break
                        for key, _ in selector.select(timeout=.05):
                            chunk = os.read(key.fd, 65536)
                            if not chunk:
                                selector.unregister(key.fileobj)
                                continue
                            log.write(chunk[:max_output - total])
                            total += len(chunk)
                            if total > max_output:
                                reason = 'OUTPUT_LIMIT'
                                break
                        if reason:
                            break
                    # Fast writers may exit between the final poll and EOF.
                    reason = reason or stopped()
            finally:
                try:
                    os.killpg(proc.pid, signal.SIGTERM)
                    try:
                        proc.wait(timeout=.5)
                    except subprocess.TimeoutExpired:
                        pass
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                proc.wait()
        log.flush()
        log.seek(max(0, log.tell() - 65536) if log_path is not None else 0)
        output = log.read().decode(errors='replace')
    return Result(proc.returncode, output, reason)


def _run_windows(cmd, *, cwd, env, timeout, max_output, log_path, stop, watch_files):
    """run() for a Windows Python (the player launcher's --headless):
    select() cannot wait on a pipe there
    and there are no process groups, so a thread drains the merged output
    and the limits end the process itself (the game starts no children)."""
    import threading
    deadline = time.monotonic() + timeout
    state = {'total': 0, 'reason': None}
    if log_path is not None:
        Path(log_path).parent.mkdir(parents=True, exist_ok=True)
    with (open(log_path, 'w+b') if log_path is not None
          else tempfile.TemporaryFile()) as log:
        with subprocess.Popen(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT) as proc:
            def drain():
                while True:
                    chunk = proc.stdout.read1(65536)
                    if not chunk:
                        return
                    log.write(chunk[:max(0, max_output - state['total'])])
                    state['total'] += len(chunk)
                    if state['total'] > max_output and not state['reason']:
                        state['reason'] = 'OUTPUT_LIMIT'
            reader = threading.Thread(target=drain, daemon=True)
            reader.start()
            try:
                while proc.poll() is None and not state['reason']:
                    if stop is not None and stop.is_set():
                        state['reason'] = 'CANCELLED'
                    elif time.monotonic() >= deadline:
                        state['reason'] = 'TIMEOUT'
                    for path, limit, label in watch_files:
                        if os.path.exists(path) and os.path.getsize(path) > limit:
                            state['reason'] = state['reason'] or label
                    time.sleep(.05)
            finally:
                if proc.poll() is None:
                    proc.kill()
                proc.wait()
                reader.join(timeout=5)
        log.flush()
        log.seek(max(0, log.tell() - 65536) if log_path is not None else 0)
        output = log.read().decode(errors='replace')
    return Result(proc.returncode, output, state['reason'])


def lines(path, *, max_bytes=FILE_LIMIT, max_line=65536):
    """Stream finite records; reject oversized input rather than misparse it."""
    if os.path.getsize(path) > max_bytes:
        raise ValueError(f'{path}: input exceeds {max_bytes} bytes')
    used = 0
    with open(path, 'rb') as fh:
        while raw := fh.readline(max_line + 1):
            used += len(raw)
            if len(raw) > max_line or used > max_bytes:
                raise ValueError(f'{path}: record or file exceeds size limit')
            yield raw.decode(errors='replace')


def terminate(signum, frame):
    raise SystemExit(128 + signum)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--timeout', type=float, default=60)
    ap.add_argument('--max-output', type=int, default=OUTPUT_LIMIT)
    ap.add_argument('--log')
    ap.add_argument('command', nargs=argparse.REMAINDER)
    a = ap.parse_args()
    cmd = a.command[1:] if a.command[:1] == ['--'] else a.command
    if not cmd:
        ap.error('a command is required after --')
    r = run(cmd, timeout=a.timeout, max_output=a.max_output, log_path=a.log)
    if not a.log:
        print(r.stdout, end='')
    if r.reason:
        print(f'[harness] stopped: {r.reason}', file=sys.stderr)
        return 124 if r.reason == 'TIMEOUT' else 125
    return r.returncode if r.returncode >= 0 else 128 - r.returncode


if __name__ == '__main__':
    signal.signal(signal.SIGTERM, terminate)
    sys.exit(main())
