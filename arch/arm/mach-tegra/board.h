/*
 * arch/arm/mach-tegra/board.h
 *
 * Copyright (C) 2010 Google, Inc.
 * Copyright (C) 2011 NVIDIA Corporation.
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
#include <linux/power_supply.h>

#define NVMAP_HEAP_CARVEOUT_IRAM_INIT	\
	{	.name		= "iram",					\
		.usage_mask	= NVMAP_HEAP_CARVEOUT_IRAM,			\
		.base		= TEGRA_IRAM_BASE + TEGRA_RESET_HANDLER_SIZE,	\
		.size		= TEGRA_IRAM_SIZE - TEGRA_RESET_HANDLER_SIZE,	\
		.buddy_size	= 0, /* no buddy allocation for IRAM */		\
	}

void tegra_assert_system_reset(enum reboot_mode mode, const char *cmd);

void __init tegra20_init_early(void);
void __init tegra30_init_early(void);
void __init tegra_map_common_io(void);
void __init tegra_init_irq(void);
void __init tegra_dt_init_irq(void);
void __init tegra_reserve(unsigned long carveout_size, unsigned long fb_size,
	unsigned long fb2_size);
int __init tegra_pcie_init(bool init_port0, bool init_port1);
void tegra_init_cache();
void __init tegra_release_bootloader_fb(void);
void __init tegra_protected_aperture_init(unsigned long aperture);
void tegra_move_framebuffer(unsigned long to, unsigned long from,
	unsigned long size);
bool is_tegra_debug_uartport_hs(void);
int get_tegra_uart_debug_port_id(void);

extern unsigned long tegra_bootloader_fb_start;
extern unsigned long tegra_bootloader_fb_size;
extern unsigned long tegra_fb_start;
extern unsigned long tegra_fb_size;
extern unsigned long tegra_fb2_start;
extern unsigned long tegra_fb2_size;
extern unsigned long tegra_carveout_start;
extern unsigned long tegra_carveout_size;
extern unsigned long tegra_vpr_start;
extern unsigned long tegra_vpr_size;
extern unsigned long tegra_lp0_vec_start;
extern unsigned long tegra_lp0_vec_size;
extern bool tegra_lp0_vec_relocate;
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

enum panel_type {
	panel_type_lvds = 0,
	panel_type_dsi,
};

enum audio_codec_type {
	audio_codec_none,
	audio_codec_wm8903,
};

void tegra_get_board_info(struct board_info *);
void tegra_get_pmu_board_info(struct board_info *bi);
void tegra_get_display_board_info(struct board_info *bi);

int get_core_edp(void);
enum panel_type get_panel_type(void);
int tegra_get_modem_id(void);
enum power_supply_type get_power_supply_type(void);
enum audio_codec_type get_audio_codec_type(void);
int get_maximum_cpu_current_supported(void);

#endif
