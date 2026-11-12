#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compatibility entry point for the single verified native asset engine.

Usage: python3 pc/tools/extract_assets.py disc.cue /new/assets/directory
See docs/pc-port/ASSET-BOOTSTRAP.md for supported images and cache guarantees.
"""
from bootstrap_assets import main


if __name__ == '__main__':
    raise SystemExit(main())
