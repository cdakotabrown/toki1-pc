# SPDX-License-Identifier: GPL-3.0-or-later
"""Allocation budgets for developer asset inspectors, checked before decoding."""
from pathlib import Path

MAX_ASSET_BYTES = 16 * 1024 * 1024
MAX_PIXELS = 4 * 1024 * 1024


def read_asset(path, max_bytes=MAX_ASSET_BYTES):
    path = Path(path)
    if path.stat().st_size > max_bytes:
        raise ValueError(f'{path}: asset exceeds {max_bytes} bytes')
    with path.open('rb') as fh:
        data = fh.read(max_bytes + 1)
    if len(data) > max_bytes:
        raise ValueError(f'{path}: asset grew beyond size limit')
    return data


def check_pixels(width, height):
    if width <= 0 or height <= 0 or width * height > MAX_PIXELS:
        raise ValueError(f'invalid or excessive image dimensions: {width}x{height}')
