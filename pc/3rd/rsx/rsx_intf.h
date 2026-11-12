/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Shim stub — redirect to our beetle_psx_globals.h.
 *
 * upstream/gpu_polygon_sub.cpp and gpu_line.cpp #include
 * "../../rsx/rsx_intf.h" from their directory; relative to
 * pc/3rd/mednafen-gpu/upstream/ that points here. The real
 * Beetle-PSX rsx_intf.h is 800+ LOC of multi-backend glue we
 * don't need — we only need rsx_intf_is_type() and the RSX_*
 * enum, which our beetle_psx_globals.h already declares.
 *
 * This file is a shim artifact; it lives outside the
 * mednafen-gpu/ tree so the literal `../../rsx/` include path
 * from upstream .cpp files resolves correctly.
 */
#ifndef MDFN_SHIM_RSX_INTF_H
#define MDFN_SHIM_RSX_INTF_H

#include "../mednafen-gpu/shim/beetle_psx_globals.h"

#endif
