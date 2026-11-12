/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host watchdog: standalone scene loops can complete frames inside game_main. */
#ifndef PC_WATCHDOG_H
#define PC_WATCHDOG_H

void pc_watchdog_arm(void);
void pc_watchdog_disarm(void);
void pc_watchdog_presented(void);
void pc_watchdog_vblank(void);

#endif
