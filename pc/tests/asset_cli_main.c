/* SPDX-License-Identifier: GPL-3.0-or-later */
/* asset_cli_main.c -- the standalone build of the C asset setup's command
 * line (pc/hal/asset_setup.c), used by pc/tools/test_asset_setup_c.py so the
 * C-versus-Python equivalence test needs no SDL and no game build.  The game
 * executable runs the same as_cli for --prepare-assets / --verify-assets. */
#include "asset_setup.h"

int main(int argc, char **argv) { return as_cli(argc, argv); }
