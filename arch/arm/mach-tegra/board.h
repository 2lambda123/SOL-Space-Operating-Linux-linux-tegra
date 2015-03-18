/*
 * arch/arm/mach-tegra/board.h
 *
 * Copyright (C) 2010 Google, Inc.
 *
 * Author:
 *	Colin Cross <ccross@google.com>
 *	Erik Gilling <konkers@google.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef __MACH_TEGRA_BOARD_H
#define __MACH_TEGRA_BOARD_H

#include <linux/types.h>
#include <linux/reboot.h>

void tegra_assert_system_reset(enum reboot_mode mode, const char *cmd);

void __init tegra20_init_early(void);
void __init tegra30_init_early(void);
void __init tegra_map_common_io(void);
void __init tegra_init_irq(void);
void __init tegra_dt_init_irq(void);
void __init tegra_reserve(unsigned long carveout_size, unsigned long fb_size,
	unsigned long fb2_size);
int __init tegra_pcie_init(bool init_port0, bool init_port1);
void tegra_init_cache(u32 tag_latency, u32 data_latency);
void __init tegra_protected_aperture_init(unsigned long aperture);
void tegra_move_framebuffer(unsigned long to, unsigned long from,
	unsigned long size);

extern unsigned long tegra_bootloader_fb_start;
extern unsigned long tegra_bootloader_fb_size;
extern unsigned long tegra_fb_start;
extern unsigned long tegra_fb_size;
extern unsigned long tegra_fb2_start;
extern unsigned long tegra_fb2_size;
extern unsigned long tegra_carveout_start;
extern unsigned long tegra_carveout_size;
extern unsigned long tegra_lp0_vec_start;
extern unsigned long tegra_lp0_vec_size;
extern unsigned long tegra_grhost_aperture;

void tegra_init_late(void);

#ifdef CONFIG_DEBUG_FS
int tegra_clk_debugfs_init(void);
#else
static inline int tegra_clk_debugfs_init(void) { return 0; }
#endif

int __init tegra_powergate_init(void);
#if defined(CONFIG_ARCH_TEGRA_2x_SOC) && defined(CONFIG_DEBUG_FS)
int __init tegra_powergate_debugfs_init(void);
#else
static inline int tegra_powergate_debugfs_init(void) { return 0; }
#endif

int __init harmony_regulator_init(void);
#ifdef CONFIG_TEGRA_PCI
int __init harmony_pcie_init(void);
#else
static inline int harmony_pcie_init(void) { return 0; }
#endif

void __init tegra_paz00_wifikill_init(void);

extern void tegra_init_timer(void);

enum board_fab {
	BOARD_FAB_A = 0,
	BOARD_FAB_B,
	BOARD_FAB_C,
	BOARD_FAB_D,
};

struct board_info {
	u16 board_id;
	u16 sku;
	u8  fab;
	u8  major_revision;
	u8  minor_revision;
};

void tegra_get_board_info(struct board_info *);

#endif
