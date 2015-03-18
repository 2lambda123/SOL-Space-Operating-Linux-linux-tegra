/*
 * arch/arm/mach-tegra/tegra21_clocks.c
 *
 * Copyright (C) 2013-2014 NVIDIA Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/cpufreq.h>
#include <linux/syscore_ops.h>
#include <linux/platform_device.h>

#include <asm/clkdev.h>

#include <mach/edp.h>
#include <mach/hardware.h>
#include <mach/mc.h>

#include "clock.h"
#include "dvfs.h"
#include "pm.h"
#include "sleep.h"
#include "devices.h"
#include "tegra12_emc.h"
#include "tegra_cl_dvfs.h"

/* FIXME: Disable for initial Si bringup */
#undef USE_PLLE_SS
#define USE_PLLE_SS 0

#define RST_DEVICES_L			0x004
#define RST_DEVICES_H			0x008
#define RST_DEVICES_U			0x00C
#define RST_DEVICES_V			0x358
#define RST_DEVICES_W			0x35C
#define RST_DEVICES_X			0x28C
#define RST_DEVICES_Y			0x2A4
#define RST_DEVICES_SET_L		0x300
#define RST_DEVICES_CLR_L		0x304
#define RST_DEVICES_SET_V		0x430
#define RST_DEVICES_CLR_V		0x434
#define RST_DEVICES_SET_X		0x290
#define RST_DEVICES_CLR_X		0x294
#define RST_DEVICES_SET_Y		0x2A8
#define RST_DEVICES_CLR_Y		0x2AC
#define RST_DEVICES_NUM			7

#define CLK_OUT_ENB_L			0x010
#define CLK_OUT_ENB_H			0x014
#define CLK_OUT_ENB_U			0x018
#define CLK_OUT_ENB_V			0x360
#define CLK_OUT_ENB_W			0x364
#define CLK_OUT_ENB_X			0x280
#define CLK_OUT_ENB_Y			0x298
#define CLK_OUT_ENB_SET_L		0x320
#define CLK_OUT_ENB_CLR_L		0x324
#define CLK_OUT_ENB_SET_V		0x440
#define CLK_OUT_ENB_CLR_V		0x444
#define CLK_OUT_ENB_SET_X		0x284
#define CLK_OUT_ENB_CLR_X		0x288
#define CLK_OUT_ENB_SET_Y		0x29C
#define CLK_OUT_ENB_CLR_Y		0x2A0
#define CLK_OUT_ENB_NUM			7

#define CLK_OUT_ENB_L_RESET_MASK	0xdcd7dff9
#define CLK_OUT_ENB_H_RESET_MASK	0x87d1f3e7
#define CLK_OUT_ENB_U_RESET_MASK	0xf3fed3fa
#define CLK_OUT_ENB_V_RESET_MASK	0xffc18cfb
#define CLK_OUT_ENB_W_RESET_MASK	0x793fb7ff
#define CLK_OUT_ENB_X_RESET_MASK	0x3e66fff
#define CLK_OUT_ENB_Y_RESET_MASK	0xfc1fc7ff

#define RST_DEVICES_V_SWR_CPULP_RST_DIS	(0x1 << 1) /* Reserved on Tegra11 */
#define CLK_OUT_ENB_V_CLK_ENB_CPULP_EN	(0x1 << 1)

#define PERIPH_CLK_TO_BIT(c)		(1 << (c->u.periph.clk_num % 32))
#define PERIPH_CLK_TO_RST_REG(c)	\
	periph_clk_to_reg((c), RST_DEVICES_L, RST_DEVICES_V, \
		RST_DEVICES_X, RST_DEVICES_Y, 4)
#define PERIPH_CLK_TO_RST_SET_REG(c)	\
	periph_clk_to_reg((c), RST_DEVICES_SET_L, RST_DEVICES_SET_V, \
		RST_DEVICES_SET_X, RST_DEVICES_SET_Y, 8)
#define PERIPH_CLK_TO_RST_CLR_REG(c)	\
	periph_clk_to_reg((c), RST_DEVICES_CLR_L, RST_DEVICES_CLR_V, \
		RST_DEVICES_CLR_X, RST_DEVICES_CLR_Y, 8)

#define PERIPH_CLK_TO_ENB_REG(c)	\
	periph_clk_to_reg((c), CLK_OUT_ENB_L, CLK_OUT_ENB_V, \
		CLK_OUT_ENB_X, CLK_OUT_ENB_Y, 4)
#define PERIPH_CLK_TO_ENB_SET_REG(c)	\
	periph_clk_to_reg((c), CLK_OUT_ENB_SET_L, CLK_OUT_ENB_SET_V, \
		CLK_OUT_ENB_SET_X, CLK_OUT_ENB_SET_Y, 8)
#define PERIPH_CLK_TO_ENB_CLR_REG(c)	\
	periph_clk_to_reg((c), CLK_OUT_ENB_CLR_L, CLK_OUT_ENB_CLR_V, \
		CLK_OUT_ENB_CLR_X, CLK_OUT_ENB_CLR_Y, 8)

static u32 pll_reg_idx_to_addr(struct clk *c, int idx)
{
	switch (idx) {
	case PLL_BASE_IDX:
		return c->reg;
	case PLL_MISC0_IDX:
		return c->reg + c->u.pll.misc0;
	case PLL_MISC1_IDX:
		return c->reg + c->u.pll.misc1;
	case PLL_MISC2_IDX:
		return c->reg + c->u.pll.misc2;
	case PLL_MISC3_IDX:
		return c->reg + c->u.pll.misc3;
	case PLL_MISC4_IDX:
		return c->reg + c->u.pll.misc4;
	case PLL_MISC5_IDX:
		return c->reg + c->u.pll.misc5;
	}
	BUG();
	return 0;
}

static bool pll_is_dyn_ramp(struct clk *c, struct clk_pll_freq_table *old_cfg,
			    struct clk_pll_freq_table *new_cfg)
{
	return (c->state == ON) && c->u.pll.defaults_set && c->u.pll.dyn_ramp &&
		(new_cfg->m == old_cfg->m) && (new_cfg->p == old_cfg->p);
}

#define PLL_MISC_CHK_DEFAULT(c, misc_num, default_val, mask)		       \
do {									       \
	u32 boot_val = clk_readl((c)->reg + (c)->u.pll.misc##misc_num);	       \
	boot_val &= (mask);						       \
	default_val &= (mask);						       \
	if (boot_val != (default_val)) {				       \
		pr_warn("%s boot misc" #misc_num " 0x%x : expected 0x%x\n",    \
			(c)->name, boot_val, (default_val));		       \
		pr_warn(" (comparison mask = 0x%x)\n", mask);		       \
			(c)->u.pll.defaults_set = false;		       \
	}								       \
} while (0)


#define CLK_MASK_ARM			0x44
#define MISC_CLK_ENB			0x48

#define OSC_CTRL			0x50
#define OSC_CTRL_OSC_FREQ_MASK		(0xF<<28)
#define OSC_CTRL_OSC_FREQ_12MHZ		(0x8<<28)
#define OSC_CTRL_OSC_FREQ_13MHZ		(0x0<<28)
#define OSC_CTRL_OSC_FREQ_19_2MHZ	(0x4<<28)
#define OSC_CTRL_OSC_FREQ_38_4MHZ	(0x5<<28)
#define OSC_CTRL_MASK			(0x3f2 | OSC_CTRL_OSC_FREQ_MASK)

#define OSC_CTRL_PLL_REF_DIV_MASK	(3<<26)
#define OSC_CTRL_PLL_REF_DIV_1		(0<<26)
#define OSC_CTRL_PLL_REF_DIV_2		(1<<26)
#define OSC_CTRL_PLL_REF_DIV_4		(2<<26)

#define PERIPH_CLK_SOURCE_I2S1		0x100
#define PERIPH_CLK_SOURCE_EMC		0x19c
#define PERIPH_CLK_SOURCE_EMC_MC_SAME	(1<<16)
#define PERIPH_CLK_SOURCE_LA		0x1f8
#define PERIPH_CLK_SOURCE_NUM1 \
	((PERIPH_CLK_SOURCE_LA - PERIPH_CLK_SOURCE_I2S1) / 4)

#define PERIPH_CLK_SOURCE_MSELECT	0x3b4
#define PERIPH_CLK_SOURCE_SE		0x42c
#define PERIPH_CLK_SOURCE_NUM2 \
	((PERIPH_CLK_SOURCE_SE - PERIPH_CLK_SOURCE_MSELECT) / 4 + 1)

#define AUDIO_DLY_CLK			0x49c
#define AUDIO_SYNC_CLK_SPDIF		0x4b4
#define PERIPH_CLK_SOURCE_NUM3 \
	((AUDIO_SYNC_CLK_SPDIF - AUDIO_DLY_CLK) / 4 + 1)

#define SPARE_REG			0x55c
#define SPARE_REG_CLK_M_DIVISOR_SHIFT	2
#define SPARE_REG_CLK_M_DIVISOR_MASK	(3 << SPARE_REG_CLK_M_DIVISOR_SHIFT)

#define PERIPH_CLK_SOURCE_XUSB_HOST	0x600
#define PERIPH_CLK_SOURCE_VIC		0x678
#define PERIPH_CLK_SOURCE_NUM4 \
	((PERIPH_CLK_SOURCE_VIC - PERIPH_CLK_SOURCE_XUSB_HOST) / 4 + 1)

#define PERIPH_CLK_SOURCE_NUM		(PERIPH_CLK_SOURCE_NUM1 + \
					 PERIPH_CLK_SOURCE_NUM2 + \
					 PERIPH_CLK_SOURCE_NUM3 + \
					 PERIPH_CLK_SOURCE_NUM4)

#define CPU_SOFTRST_CTRL		0x380
#define CPU_SOFTRST_CTRL1		0x384
#define CPU_SOFTRST_CTRL2		0x388

#define PERIPH_CLK_SOURCE_DIVU71_MASK	0xFF
#define PERIPH_CLK_SOURCE_DIVU16_MASK	0xFFFF
#define PERIPH_CLK_SOURCE_DIV_SHIFT	0
#define PERIPH_CLK_SOURCE_DIVIDLE_SHIFT	8
#define PERIPH_CLK_SOURCE_DIVIDLE_VAL	50
#define PERIPH_CLK_UART_DIV_ENB		(1<<24)
#define PERIPH_CLK_VI_SEL_EX_SHIFT	24
#define PERIPH_CLK_VI_SEL_EX_MASK	(0x3<<PERIPH_CLK_VI_SEL_EX_SHIFT)
#define PERIPH_CLK_NAND_DIV_EX_ENB	(1<<8)
#define PERIPH_CLK_DTV_POLARITY_INV	(1<<25)

#define AUDIO_SYNC_SOURCE_MASK		0x0F
#define AUDIO_SYNC_DISABLE_BIT		0x10
#define AUDIO_SYNC_TAP_NIBBLE_SHIFT(c)	((c->reg_shift - 24) * 4)

#define PERIPH_CLK_SOR_CLK_SEL_SHIFT	14
#define PERIPH_CLK_SOR_CLK_SEL_MASK	(0x3<<PERIPH_CLK_SOR_CLK_SEL_SHIFT)

/* PLL common */
/* FIXME: check no direct usage */
#define PLL_BASE			0x0
#define PLL_BASE_BYPASS			(1<<31)
#define PLL_BASE_ENABLE			(1<<30)
#define PLL_BASE_REF_DISABLE		(1<<29)

/* Quasi-linear PLL divider */
#define PLL_QLIN_PDIV_MAX		16
/* exponential PLL divider */
#define PLL_EXPO_PDIV_MAX		7

/* PLL with SDM:  effective n value: ndiv + 1/2 + sdm_din/PLL_SDM_COEFF */
#define PLL_SDM_COEFF			(1 << 13)

/* FIXME: no longer common should be eventually removed */
#define PLL_BASE_OVERRIDE		(1<<28)
#define PLL_BASE_LOCK			(1<<27)
#define PLL_BASE_DIVP_MASK		(0x7<<20)
#define PLL_BASE_DIVP_SHIFT		20
#define PLL_BASE_DIVN_MASK		(0x3FF<<8)
#define PLL_BASE_DIVN_SHIFT		8
#define PLL_BASE_DIVM_MASK		(0x1F)
#define PLL_BASE_DIVM_SHIFT		0

/* FXME: to be removed */
#define PLL_BASE_PARSE(pll, cfg, b)					       \
	do {								       \
		(cfg).m = ((b) & pll##_BASE_DIVM_MASK) >> PLL_BASE_DIVM_SHIFT; \
		(cfg).n = ((b) & pll##_BASE_DIVN_MASK) >> PLL_BASE_DIVN_SHIFT; \
		(cfg).p = ((b) & pll##_BASE_DIVP_MASK) >> PLL_BASE_DIVP_SHIFT; \
	} while (0)

#define PLL_OUT_RATIO_MASK		(0xFF<<8)
#define PLL_OUT_RATIO_SHIFT		8
#define PLL_OUT_OVERRIDE		(1<<2)
#define PLL_OUT_CLKEN			(1<<1)
#define PLL_OUT_RESET_DISABLE		(1<<0)

/* FXME: to be removed */
#define PLL_MISC(c)			\
	(((c)->flags & PLL_ALT_MISC_REG) ? 0x4 : 0xc)
#define PLL_MISCN(c, n)		\
	((c)->u.pll.misc1 + ((n) - 1) * PLL_MISC(c))
#define PLL_MISC_LOCK_ENABLE(c)	\
	(((c)->flags & (PLLU | PLLD)) ? (1<<22) : (1<<18))

#define PLL_MISC_DCCON_SHIFT		20
#define PLL_MISC_CPCON_SHIFT		8
#define PLL_MISC_CPCON_MASK		(0xF<<PLL_MISC_CPCON_SHIFT)
#define PLL_MISC_LFCON_SHIFT		4
#define PLL_MISC_LFCON_MASK		(0xF<<PLL_MISC_LFCON_SHIFT)
#define PLL_MISC_VCOCON_SHIFT		0
#define PLL_MISC_VCOCON_MASK		(0xF<<PLL_MISC_VCOCON_SHIFT)

#define PLL_FIXED_MDIV(c, ref)		((ref) == 38400000 ? 2 : 1)

#define PLLU_BASE_POST_DIV		(1<<20)

#define PLLDU_LFCON			2

/* PLLC, PLLC2, PLLC3 and PLLA1 */
#define PLLCX_USE_DYN_RAMP		0
#define PLLCX_BASE_LOCK			(1 << 26)

#define PLLCX_MISC0_RESET		(1 << 30)
#define PLLCX_MISC0_LOOP_CTRL_SHIFT	0
#define PLLCX_MISC0_LOOP_CTRL_MASK	(0x3 << PLLCX_MISC0_LOOP_CTRL_SHIFT)

#define PLLCX_MISC1_IDDQ		(1 << 27)

#define PLLCX_MISC0_DEFAULT_VALUE	0x40080000
#define PLLCX_MISC0_WRITE_MASK		0x400ffffb
#define PLLCX_MISC1_DEFAULT_VALUE	0x08000000
#define PLLCX_MISC1_WRITE_MASK		0x08003cff
#define PLLCX_MISC2_DEFAULT_VALUE	0x1f720f05
#define PLLCX_MISC2_WRITE_MASK		0xffffff17
#define PLLCX_MISC3_DEFAULT_VALUE	0x000000c4
#define PLLCX_MISC3_WRITE_MASK		0x00ffffff

/* PLLA */
#define PLLA_BASE_LOCK			(1 << 27)
#define PLLA_BASE_IDDQ			(1 << 25)

#define PLLA_MISC0_LOCK_ENABLE		(1 << 28)
#define PLLA_MISC0_LOCK_OVERRIDE	(1 << 27)

#define PLLA_MISC2_EN_SDM		(1 << 26)
#define PLLA_MISC2_EN_DYNRAMP		(1 << 25)

#define PLLA_MISC0_DEFAULT_VALUE	0x12000000
#define PLLA_MISC0_WRITE_MASK		0x7fffffff
#define PLLA_MISC2_DEFAULT_VALUE	0x0
#define PLLA_MISC2_WRITE_MASK		0x06ffffff

/* PLLD */
#define PLLD_BASE_LOCK			(1 << 27)
#define PLLD_BASE_DSI_MUX_SHIFT		25
#define PLLD_BASE_DSI_MUX_MASK		(0x1 << PLLD_BASE_DSI_MUX_SHIFT)
#define PLLD_BASE_CSI_CLKSOURCE		(1 << 23)

#define PLLD_MISC0_DSI_CLKENABLE	(1 << 21)
#define PLLD_MISC0_IDDQ			(1 << 20)
#define PLLD_MISC0_LOCK_ENABLE		(1 << 18)
#define PLLD_MISC0_LOCK_OVERRIDE	(1 << 17)
#define PLLD_MISC0_EN_SDM		(1 << 16)

#define PLLD_MISC0_DEFAULT_VALUE	0x00140000
#define PLLD_MISC0_WRITE_MASK		0x3ff7ffff
#define PLLD_MISC1_DEFAULT_VALUE	0x0
#define PLLD_MISC1_WRITE_MASK		0x00ffffff

/* PLLD2 and PLLDP  and PLLC4 */
#define PLLDSS_BASE_LOCK		(1 << 27)
#define PLLDSS_BASE_LOCK_OVERRIDE	(1 << 24)
#define PLLDSS_BASE_IDDQ		(1 << 18)
#define PLLDSS_BASE_REF_SEL_SHIFT	25
#define PLLDSS_BASE_REF_SEL_MASK	(0x3 << PLLDSS_BASE_REF_SEL_SHIFT)

#define PLLDSS_MISC0_LOCK_ENABLE	(1 << 30)

#define PLLDSS_MISC1_CFG_EN_SDM		(1 << 31)
#define PLLDSS_MISC1_CFG_EN_SSC		(1 << 30)

#define PLLD2_MISC0_DEFAULT_VALUE	0x40000000
#define PLLD2_MISC1_CFG_DEFAULT_VALUE	0x10000000
#define PLLD2_MISC2_CTRL1_DEFAULT_VALUE	0x0
#define PLLD2_MISC3_CTRL2_DEFAULT_VALUE	0x0

#define PLLDP_MISC0_DEFAULT_VALUE	0x40000000
#define PLLDP_MISC1_CFG_DEFAULT_VALUE	0xc0000000
#define PLLDP_MISC2_CTRL1_DEFAULT_VALUE	0xf000e5ec
#define PLLDP_MISC3_CTRL2_DEFAULT_VALUE	0x101BF000

#define PLLDSS_MISC0_WRITE_MASK		0x47ffffff
#define PLLDSS_MISC1_CFG_WRITE_MASK	0xf8000000
#define PLLDSS_MISC2_CTRL1_WRITE_MASK	0xffffffff
#define PLLDSS_MISC3_CTRL2_WRITE_MASK	0xffffffff

#define PLLC4_MISC0_DEFAULT_VALUE	0x40000000

/* PLLRE */
#define PLLRE_MISC0_LOCK_ENABLE		(1 << 30)
#define PLLRE_MISC0_LOCK_OVERRIDE	(1 << 29)
#define PLLRE_MISC0_LOCK		(1 << 27)
#define PLLRE_MISC0_IDDQ		(1 << 24)

#define PLLRE_BASE_DEFAULT_VALUE	0x0
#define PLLRE_MISC0_DEFAULT_VALUE	0x41000000

#define PLLRE_BASE_DEFAULT_MASK		0x1c000000
#define PLLRE_MISC0_WRITE_MASK		0x67ffffff

/* PLLU */
#define PLLU_BASE_LOCK			(1 << 27)
#define PLLU_BASE_OVERRIDE		(1 << 24)

#define PLLU_MISC0_IDDQ			(1 << 31)
#define PLLU_MISC0_LOCK_ENABLE		(1 << 29)
#define PLLU_MISC1_LOCK_OVERRIDE	(1 << 0)

#define PLLU_MISC0_DEFAULT_VALUE	0xa0000000
#define PLLU_MISC1_DEFAULT_VALUE	0x0

#define PLLU_MISC0_WRITE_MASK		0xbfffffff
#define PLLU_MISC1_WRITE_MASK		0x00000007

/* PLLX */
#define PLLX_USE_DYN_RAMP		0
#define PLLX_BASE_LOCK			(1 << 27)

#define PLLX_MISC0_FO_G_DISABLE		(0x1 << 28)
#define PLLX_MISC0_LOCK_ENABLE		(0x1 << 18)

#define PLLX_MISC2_DYNRAMP_STEPB_SHIFT	24
#define PLLX_MISC2_DYNRAMP_STEPB_MASK	(0xFF << PLLX_MISC2_DYNRAMP_STEPB_SHIFT)
#define PLLX_MISC2_DYNRAMP_STEPA_SHIFT	16
#define PLLX_MISC2_DYNRAMP_STEPA_MASK	(0xFF << PLLX_MISC2_DYNRAMP_STEPA_SHIFT)
#define PLLX_MISC2_NDIV_NEW_SHIFT	8
#define PLLX_MISC2_NDIV_NEW_MASK	(0xFF << PLLX_MISC2_NDIV_NEW_SHIFT)
#define PLLX_MISC2_LOCK_OVERRIDE	(0x1 << 4)
#define PLLX_MISC2_DYNRAMP_DONE		(0x1 << 2)
#define PLLX_MISC2_EN_DYNRAMP		(0x1 << 0)

#define PLLX_MISC3_IDDQ			(0x1 << 3)

#define PLLX_MISC0_DEFAULT_VALUE	PLLX_MISC0_LOCK_ENABLE
#define PLLX_MISC0_WRITE_MASK		0x10c40000
#define PLLX_MISC1_DEFAULT_VALUE	0x0
#define PLLX_MISC1_WRITE_MASK		0x00ffffff
#define PLLX_MISC2_DEFAULT_VALUE	0x0
#define PLLX_MISC2_WRITE_MASK		0xffffff11
#define PLLX_MISC3_DEFAULT_VALUE	PLLX_MISC3_IDDQ
#define PLLX_MISC3_WRITE_MASK		0x01ff0f0f
#define PLLX_MISC4_DEFAULT_VALUE	0x0
#define PLLX_MISC4_WRITE_MASK		0x8000ffff
#define PLLX_MISC5_DEFAULT_VALUE	0x0
#define PLLX_MISC5_WRITE_MASK		0x0000ffff

#define PLLX_HW_CTRL_CFG		0x548
#define PLLX_HW_CTRL_CFG_SWCTRL		(0x1 << 0)

/* PLLM */
#define PLLM_BASE_DIVP_MASK		(0xF << PLL_BASE_DIVP_SHIFT)
#define PLLM_BASE_DIVN_MASK		(0xFF << PLL_BASE_DIVN_SHIFT)
#define PLLM_BASE_DIVM_MASK		(0xFF << PLL_BASE_DIVM_SHIFT)

/* PLLM has 4-bit PDIV, but entry 15 is not allowed in h/w,
   and s/w usage is limited to 5 */
#define PLLM_PDIV_MAX			14
#define PLLM_SW_PDIV_MAX		5

#define PLLM_MISC_FSM_SW_OVERRIDE	(0x1 << 10)
#define PLLM_MISC_IDDQ			(0x1 << 5)
#define PLLM_MISC_LOCK_DISABLE		(0x1 << 4)
#define PLLM_MISC_LOCK_OVERRIDE		(0x1 << 3)

#define PMC_PLLP_WB0_OVERRIDE			0xf8
#define PMC_PLLP_WB0_OVERRIDE_PLLM_ENABLE	(1 << 12)
#define PMC_PLLP_WB0_OVERRIDE_PLLM_OVERRIDE	(1 << 11)

/* M, N layout for PLLM override and base registers are the same */
#define PMC_PLLM_WB0_OVERRIDE			0x1dc

#define PMC_PLLM_WB0_OVERRIDE_2			0x2b0
#define PMC_PLLM_WB0_OVERRIDE_2_DIVP_SHIFT	27
#define PMC_PLLM_WB0_OVERRIDE_2_DIVP_MASK	(0xF << 27)

#define OUT_OF_TABLE_CPCON		0x8

#define SUPER_CLK_MUX			0x00
#define SUPER_STATE_SHIFT		28
#define SUPER_STATE_MASK		(0xF << SUPER_STATE_SHIFT)
#define SUPER_STATE_STANDBY		(0x0 << SUPER_STATE_SHIFT)
#define SUPER_STATE_IDLE		(0x1 << SUPER_STATE_SHIFT)
#define SUPER_STATE_RUN			(0x2 << SUPER_STATE_SHIFT)
#define SUPER_STATE_IRQ			(0x3 << SUPER_STATE_SHIFT)
#define SUPER_STATE_FIQ			(0x4 << SUPER_STATE_SHIFT)
#define SUPER_LP_DIV2_BYPASS		(0x1 << 16)
#define SUPER_SOURCE_MASK		0xF
#define	SUPER_FIQ_SOURCE_SHIFT		12
#define	SUPER_IRQ_SOURCE_SHIFT		8
#define	SUPER_RUN_SOURCE_SHIFT		4
#define	SUPER_IDLE_SOURCE_SHIFT		0

#define SUPER_CLK_DIVIDER		0x04
#define SUPER_CLOCK_DIV_U71_SHIFT	16
#define SUPER_CLOCK_DIV_U71_MASK	(0xff << SUPER_CLOCK_DIV_U71_SHIFT)

#define SUPER_SKIPPER_ENABLE		(1 << 31)
#define SUPER_SKIPPER_TERM_SIZE		8
#define SUPER_SKIPPER_MUL_SHIFT		8
#define SUPER_SKIPPER_MUL_MASK		(((1 << SUPER_SKIPPER_TERM_SIZE) - 1) \
					<< SUPER_SKIPPER_MUL_SHIFT)
#define SUPER_SKIPPER_DIV_SHIFT		0
#define SUPER_SKIPPER_DIV_MASK		(((1 << SUPER_SKIPPER_TERM_SIZE) - 1) \
					<< SUPER_SKIPPER_DIV_SHIFT)

#define BUS_CLK_DISABLE			(1<<3)
#define BUS_CLK_DIV_MASK		0x3

#define PMC_CTRL			0x0
 #define PMC_CTRL_BLINK_ENB		(1 << 7)

#define PMC_DPD_PADS_ORIDE		0x1c
 #define PMC_DPD_PADS_ORIDE_BLINK_ENB	(1 << 20)

#define PMC_BLINK_TIMER_DATA_ON_SHIFT	0
#define PMC_BLINK_TIMER_DATA_ON_MASK	0x7fff
#define PMC_BLINK_TIMER_ENB		(1 << 15)
#define PMC_BLINK_TIMER_DATA_OFF_SHIFT	16
#define PMC_BLINK_TIMER_DATA_OFF_MASK	0xffff

#define UTMIP_PLL_CFG2					0x488
#define UTMIP_PLL_CFG2_STABLE_COUNT(x)			(((x) & 0xfff) << 6)
#define UTMIP_PLL_CFG2_ACTIVE_DLY_COUNT(x)		(((x) & 0x3f) << 18)
#define UTMIP_PLL_CFG2_FORCE_PD_SAMP_A_POWERDOWN	(1 << 0)
#define UTMIP_PLL_CFG2_FORCE_PD_SAMP_A_POWERUP		(1 << 1)
#define UTMIP_PLL_CFG2_FORCE_PD_SAMP_B_POWERDOWN	(1 << 2)
#define UTMIP_PLL_CFG2_FORCE_PD_SAMP_B_POWERUP		(1 << 3)
#define UTMIP_PLL_CFG2_FORCE_PD_SAMP_C_POWERDOWN	(1 << 4)
#define UTMIP_PLL_CFG2_FORCE_PD_SAMP_C_POWERUP		(1 << 5)
#define UTMIP_PLL_CFG2_FORCE_PD_SAMP_D_POWERDOWN	(1 << 24)
#define UTMIP_PLL_CFG2_FORCE_PD_SAMP_D_POWERUP		(1 << 25)

#define UTMIP_PLL_CFG1					0x484
#define UTMIP_PLL_CFG1_ENABLE_DLY_COUNT(x)		(((x) & 0x1f) << 27)
#define UTMIP_PLL_CFG1_XTAL_FREQ_COUNT(x)		(((x) & 0xfff) << 0)
#define UTMIP_PLL_CFG1_FORCE_PLL_ENABLE_POWERUP	(1 << 15)
#define UTMIP_PLL_CFG1_FORCE_PLL_ENABLE_POWERDOWN	(1 << 14)
#define UTMIP_PLL_CFG1_FORCE_PLL_ACTIVE_POWERDOWN	(1 << 12)
#define UTMIP_PLL_CFG1_FORCE_PLLU_POWERUP		(1 << 17)
#define UTMIP_PLL_CFG1_FORCE_PLLU_POWERDOWN		(1 << 16)

/* PLLE */
#define PLLE_BASE_LOCK_OVERRIDE		(0x1 << 29)
#define PLLE_BASE_DIVCML_SHIFT		24
#define PLLE_BASE_DIVCML_MASK		(0xf<<PLLE_BASE_DIVCML_SHIFT)

#define PLLE_BASE_DIVN_MASK		(0xFF<<PLL_BASE_DIVN_SHIFT)
#define PLLE_BASE_DIVM_MASK		(0xFF<<PLL_BASE_DIVM_SHIFT)

/* PLLE has 4-bit CMLDIV, but entry 15 is not allowed in h/w */
#define PLLE_CMLDIV_MAX			14
#define PLLE_MISC_READY			(1<<15)
#define PLLE_MISC_IDDQ_SW_CTRL		(1<<14)
#define PLLE_MISC_IDDQ_SW_VALUE		(1<<13)
#define PLLE_MISC_LOCK			(1<<11)
#define PLLE_MISC_LOCK_ENABLE		(1<<9)
#define PLLE_MISC_PLLE_PTS		(1<<8)
#define PLLE_MISC_VREG_BG_CTRL_SHIFT	4
#define PLLE_MISC_VREG_BG_CTRL_MASK	(0x3<<PLLE_MISC_VREG_BG_CTRL_SHIFT)
#define PLLE_MISC_VREG_CTRL_SHIFT	2
#define PLLE_MISC_VREG_CTRL_MASK	(0x3<<PLLE_MISC_VREG_CTRL_SHIFT)

#define PLLE_SS_CTRL			0x68
#define	PLLE_SS_INCINTRV_SHIFT		24
#define	PLLE_SS_INCINTRV_MASK		(0x3f<<PLLE_SS_INCINTRV_SHIFT)
#define	PLLE_SS_INC_SHIFT		16
#define	PLLE_SS_INC_MASK		(0xff<<PLLE_SS_INC_SHIFT)
#define	PLLE_SS_CNTL_INVERT		(0x1 << 15)
#define	PLLE_SS_CNTL_CENTER		(0x1 << 14)
#define	PLLE_SS_CNTL_SSC_BYP		(0x1 << 12)
#define	PLLE_SS_CNTL_INTERP_RESET	(0x1 << 11)
#define	PLLE_SS_CNTL_BYPASS_SS		(0x1 << 10)
#define	PLLE_SS_MAX_SHIFT		0
#define	PLLE_SS_MAX_MASK		(0x1ff<<PLLE_SS_MAX_SHIFT)
#define PLLE_SS_COEFFICIENTS_MASK	\
	(PLLE_SS_INCINTRV_MASK | PLLE_SS_INC_MASK | PLLE_SS_MAX_MASK)
#define PLLE_SS_COEFFICIENTS_VAL	\
	((0x20<<PLLE_SS_INCINTRV_SHIFT) | (0x1<<PLLE_SS_INC_SHIFT) | \
	 (0x25<<PLLE_SS_MAX_SHIFT))
#define PLLE_SS_DISABLE			(PLLE_SS_CNTL_SSC_BYP |\
	PLLE_SS_CNTL_INTERP_RESET | PLLE_SS_CNTL_BYPASS_SS)

#define PLLE_AUX			0x48c
#define PLLE_AUX_PLLRE_SEL		(1<<28)
#define PLLE_AUX_SEQ_STATE_SHIFT	26
#define PLLE_AUX_SEQ_STATE_MASK		(0x3<<PLLE_AUX_SEQ_STATE_SHIFT)
#define PLLE_AUX_SEQ_START_STATE	(1<<25)
#define PLLE_AUX_SEQ_ENABLE		(1<<24)
#define PLLE_AUX_SS_SWCTL		(1<<6)
#define PLLE_AUX_ENABLE_SWCTL		(1<<4)
#define PLLE_AUX_USE_LOCKDET		(1<<3)
#define PLLE_AUX_PLLP_SEL		(1<<2)

#define PLLE_AUX_CML_SATA_ENABLE	(1<<1)
#define PLLE_AUX_CML_PCIE_ENABLE	(1<<0)

/* USB PLLs PD HW controls */
#define XUSBIO_PLL_CFG0				0x51c
#define XUSBIO_PLL_CFG0_SEQ_START_STATE		(1<<25)
#define XUSBIO_PLL_CFG0_SEQ_ENABLE		(1<<24)
#define XUSBIO_PLL_CFG0_PADPLL_USE_LOCKDET	(1<<6)
#define XUSBIO_PLL_CFG0_CLK_ENABLE_SWCTL	(1<<2)
#define XUSBIO_PLL_CFG0_PADPLL_RESET_SWCTL	(1<<0)

#define UTMIPLL_HW_PWRDN_CFG0			0x52c
#define UTMIPLL_HW_PWRDN_CFG0_SEQ_START_STATE	(1<<25)
#define UTMIPLL_HW_PWRDN_CFG0_SEQ_ENABLE	(1<<24)
#define UTMIPLL_HW_PWRDN_CFG0_USE_LOCKDET	(1<<6)
#define UTMIPLL_HW_PWRDN_CFG0_SEQ_RESET_INPUT_VALUE	(1<<5)
#define UTMIPLL_HW_PWRDN_CFG0_SEQ_IN_SWCTL	(1<<4)
#define UTMIPLL_HW_PWRDN_CFG0_CLK_ENABLE_SWCTL	(1<<2)
#define UTMIPLL_HW_PWRDN_CFG0_IDDQ_OVERRIDE	(1<<1)
#define UTMIPLL_HW_PWRDN_CFG0_IDDQ_SWCTL	(1<<0)

#define PLLU_HW_PWRDN_CFG0			0x530
#define PLLU_HW_PWRDN_CFG0_SEQ_START_STATE	(1<<25)
#define PLLU_HW_PWRDN_CFG0_SEQ_ENABLE		(1<<24)
#define PLLU_HW_PWRDN_CFG0_USE_LOCKDET		(1<<6)
#define PLLU_HW_PWRDN_CFG0_CLK_ENABLE_SWCTL	(1<<2)
#define PLLU_HW_PWRDN_CFG0_CLK_SWITCH_SWCTL	(1<<0)

#define USB_PLLS_SEQ_START_STATE		(1<<25)
#define USB_PLLS_SEQ_ENABLE			(1<<24)
#define USB_PLLS_USE_LOCKDET			(1<<6)
#define USB_PLLS_ENABLE_SWCTL			((1<<2) | (1<<0))

/* XUSB PLL PAD controls */
#define XUSB_PADCTL_IOPHY_PLL_P0_CTL1_0         0x40
#define XUSB_PADCTL_IOPHY_PLL_P0_CTL1_0_PLL_PWR_OVRD    (1<<3)
#define XUSB_PADCTL_IOPHY_PLL_P0_CTL1_0_PLL_IDDQ        (1<<0)

/* DFLL */
#define DFLL_BASE				0x2f4
#define DFLL_BASE_RESET				(1<<0)

/* ADSP */
#define ADSP_NEON		(1 << 26)
#define ADSP_SCU		(1 << 25)
#define ADSP_WDT		(1 << 24)
#define ADSP_DBG		(1 << 23)
#define ADSP_PERIPH		(1 << 22)
#define ADSP_INTF		(1 << 21)
#define ADSP_CORE		(1 << 7)

#define ROUND_DIVIDER_UP	0
#define ROUND_DIVIDER_DOWN	1
#define DIVIDER_1_5_ALLOWED	0

/* PLLP default fixed rate in h/w controlled mode */
#define PLLP_DEFAULT_FIXED_RATE		408000000

/* Use PLL_RE as PLLE input (default - OSC via pll reference divider) */
#define USE_PLLE_INPUT_PLLRE    1

static bool tegra21_is_dyn_ramp(struct clk *c,
				unsigned long rate, bool from_vco_min);
static void tegra21_pllp_init_dependencies(unsigned long pllp_rate);
static unsigned long tegra21_clk_shared_bus_update(struct clk *bus,
	struct clk **bus_top, struct clk **bus_slow, unsigned long *rate_cap);
static unsigned long tegra21_clk_cap_shared_bus(struct clk *bus,
	unsigned long rate, unsigned long ceiling);

static struct clk *pll_u;

static bool detach_shared_bus;
module_param(detach_shared_bus, bool, 0644);

/* Defines default range for dynamic frequency lock loop (DFLL)
   to be used as CPU clock source:
   "0" - DFLL is not used,
   "1" - DFLL is used as a source for all CPU rates
   "2" - DFLL is used only for high rates above crossover with PLL dvfs curve
*/
static int use_dfll;

/**
* Structure defining the fields for USB UTMI clocks Parameters.
*/
struct utmi_clk_param
{
	/* Oscillator Frequency in KHz */
	u32 osc_frequency;
	/* UTMIP PLL Enable Delay Count  */
	u8 enable_delay_count;
	/* UTMIP PLL Stable count */
	u8 stable_count;
	/*  UTMIP PLL Active delay count */
	u8 active_delay_count;
	/* UTMIP PLL Xtal frequency count */
	u8 xtal_freq_count;
};

static const struct utmi_clk_param utmi_parameters[] =
{
/*	OSC_FREQUENCY,	ENABLE_DLY,	STABLE_CNT,	ACTIVE_DLY,	XTAL_FREQ_CNT */
	{13000000,	0x02,		0x33,		0x05,		0x7F},
	{12000000,	0x02,		0x2F,		0x04,		0x76},

	{19200000,	0x03,		0x4B,		0x06,		0xBB},
	/* HACK!!! FIXME!!! following entry for 38.4MHz is a stub */
	{38400000,	0x03,		0x4B,		0x06,		0xBB},
};

static void __iomem *reg_clk_base = IO_ADDRESS(TEGRA_CLK_RESET_BASE);
static void __iomem *reg_pmc_base = IO_ADDRESS(TEGRA_PMC_BASE);
static void __iomem *misc_gp_base = IO_ADDRESS(TEGRA_APB_MISC_BASE);
static void __iomem *reg_xusb_padctl_base = IO_ADDRESS(TEGRA_XUSB_PADCTL_BASE);

#define MISC_GP_HIDREV				0x804
#define MISC_GP_TRANSACTOR_SCRATCH_0		0x864
#define MISC_GP_TRANSACTOR_SCRATCH_LA_ENABLE	(0x1 << 1)
#define MISC_GP_TRANSACTOR_SCRATCH_DDS_ENABLE	(0x1 << 2)
#define MISC_GP_TRANSACTOR_SCRATCH_DP2_ENABLE	(0x1 << 3)

/*
 * Some peripheral clocks share an enable bit, so refcount the enable bits
 * in registers CLK_ENABLE_L, ... CLK_ENABLE_W, and protect refcount updates
 * with lock
 */
static DEFINE_SPINLOCK(periph_refcount_lock);
static int tegra_periph_clk_enable_refcount[CLK_OUT_ENB_NUM * 32];

#define clk_writel(value, reg) \
	__raw_writel(value, (void *)((uintptr_t)reg_clk_base + (reg)))
#define clk_readl(reg) \
	__raw_readl((void *)((uintptr_t)reg_clk_base + (reg)))
#define pmc_writel(value, reg) \
	__raw_writel(value, (void *)((uintptr_t)reg_pmc_base + (reg)))
#define pmc_readl(reg) \
	__raw_readl((void *)((uintptr_t)reg_pmc_base + (reg)))
#define chipid_readl() \
	__raw_readl((void *)((uintptr_t)misc_gp_base + MISC_GP_HIDREV))
#define xusb_padctl_writel(value, reg) \
	 __raw_writel(value, reg_xusb_padctl_base + (reg))
#define xusb_padctl_readl(reg) \
	readl(reg_xusb_padctl_base + (reg))

#define clk_writel_delay(value, reg) 					\
	do {								\
		__raw_writel((value), (void *)((uintptr_t)reg_clk_base + (reg)));	\
		__raw_readl((reg_clk_base + (reg)));	\
		udelay(2);						\
	} while (0)

#define pll_writel_delay(value, reg)					\
	do {								\
		__raw_writel((value), (void *)((uintptr_t)reg_clk_base + (reg)));	\
		__raw_readl((reg_clk_base + (reg)));	\
		udelay(1);						\
	} while (0)

static inline int clk_set_div(struct clk *c, u32 n)
{
	return clk_set_rate(c, (clk_get_rate(c->parent) + n-1) / n);
}

static inline u32 periph_clk_to_reg(
	struct clk *c, u32 reg_L, u32 reg_V, u32 reg_X, u32 reg_Y, int offs)
{
	u32 reg = c->u.periph.clk_num / 32;
	BUG_ON(reg >= RST_DEVICES_NUM);
	if (reg < 3)
		reg = reg_L + (reg * offs);
	else if (reg < 5)
		reg = reg_V + ((reg - 3) * offs);
	else if (reg == 5)
		reg = reg_X;
	else
		reg = reg_Y;
	return reg;
}

static int clk_div_x1_get_divider(unsigned long parent_rate, unsigned long rate,
			u32 max_x,
				 u32 flags, u32 round_mode)
{
	s64 divider_ux1 = parent_rate;
	if (!rate)
		return -EINVAL;

	if (!(flags & DIV_U71_INT))
		divider_ux1 *= 2;
	if (round_mode == ROUND_DIVIDER_UP)
		divider_ux1 += rate - 1;
	do_div(divider_ux1, rate);
	if (flags & DIV_U71_INT)
		divider_ux1 *= 2;

	if (divider_ux1 - 2 < 0)
		return 0;

	if (divider_ux1 - 2 > max_x)
		return -EINVAL;

#if !DIVIDER_1_5_ALLOWED
	if (divider_ux1 == 3)
		divider_ux1 = (round_mode == ROUND_DIVIDER_UP) ? 4 : 2;
#endif
	return divider_ux1 - 2;
}

static int clk_div71_get_divider(unsigned long parent_rate, unsigned long rate,
				 u32 flags, u32 round_mode)
{
	return clk_div_x1_get_divider(parent_rate, rate, 0xFF,
			flags, round_mode);
}

static int clk_div151_get_divider(unsigned long parent_rate, unsigned long rate,
				 u32 flags, u32 round_mode)
{
	return clk_div_x1_get_divider(parent_rate, rate, 0xFFFF,
			flags, round_mode);
}

static int clk_div16_get_divider(unsigned long parent_rate, unsigned long rate)
{
	s64 divider_u16;

	divider_u16 = parent_rate;
	if (!rate)
		return -EINVAL;
	divider_u16 += rate - 1;
	do_div(divider_u16, rate);

	if (divider_u16 - 1 < 0)
		return 0;

	if (divider_u16 - 1 > 0xFFFF)
		return -EINVAL;

	return divider_u16 - 1;
}

static long fixed_src_bus_round_updown(struct clk *c, struct clk *src,
			u32 flags, unsigned long rate, bool up, u32 *div)
{
	int divider;
	unsigned long source_rate, round_rate;

	source_rate = clk_get_rate(src);

	divider = clk_div71_get_divider(source_rate, rate + (up ? -1 : 1),
		flags, up ? ROUND_DIVIDER_DOWN : ROUND_DIVIDER_UP);
	if (divider < 0) {
		divider = flags & DIV_U71_INT ? 0xFE : 0xFF;
		round_rate = source_rate * 2 / (divider + 2);
		goto _out;
	}

	round_rate = source_rate * 2 / (divider + 2);

	if (round_rate > c->max_rate) {
		divider += flags & DIV_U71_INT ? 2 : 1;
#if !DIVIDER_1_5_ALLOWED
		divider = max(2, divider);
#endif
		round_rate = source_rate * 2 / (divider + 2);
	}
_out:
	if (div)
		*div = divider + 2;
	return round_rate;
}

static inline bool bus_user_is_slower(struct clk *a, struct clk *b)
{
	return a->u.shared_bus_user.client->max_rate <
		b->u.shared_bus_user.client->max_rate;
}

static inline bool bus_user_request_is_lower(struct clk *a, struct clk *b)
{
	return a->u.shared_bus_user.rate <
		b->u.shared_bus_user.rate;
}

/* clk_m functions */
static unsigned long tegra21_osc_autodetect_rate(struct clk *c)
{
	u32 osc_ctrl = clk_readl(OSC_CTRL);
	u32 pll_ref_div = clk_readl(OSC_CTRL) & OSC_CTRL_PLL_REF_DIV_MASK;

	switch (osc_ctrl & OSC_CTRL_OSC_FREQ_MASK) {
	case OSC_CTRL_OSC_FREQ_12MHZ:
		c->rate = 12000000;
		break;
	case OSC_CTRL_OSC_FREQ_13MHZ:
		/* 13MHz for FPGA only, BUG_ON otherwise */
		BUG_ON(!tegra_platform_is_fpga());
		c->rate = 13000000;
		break;
	case OSC_CTRL_OSC_FREQ_38_4MHZ:
		c->rate = 38400000;
		break;
	default:
		pr_err("supported OSC freq: %08x\n", osc_ctrl);
		BUG();
	}

	BUG_ON(pll_ref_div != OSC_CTRL_PLL_REF_DIV_1);

	return c->rate;
}

static void tegra21_osc_init(struct clk *c)
{
	pr_debug("%s on clock %s\n", __func__, c->name);
	tegra21_osc_autodetect_rate(c);
}

static int tegra21_osc_enable(struct clk *c)
{
	pr_debug("%s on clock %s\n", __func__, c->name);
	return 0;
}

static void tegra21_osc_disable(struct clk *c)
{
	pr_debug("%s on clock %s\n", __func__, c->name);
	WARN(1, "Attempting to disable main SoC clock\n");
}

static struct clk_ops tegra_osc_ops = {
	.init		= tegra21_osc_init,
	.enable		= tegra21_osc_enable,
	.disable	= tegra21_osc_disable,
};

static void tegra21_clk_m_init(struct clk *c)
{
	u32 rate;
	u32 spare = clk_readl(SPARE_REG);

	pr_debug("%s on clock %s\n", __func__, c->name);

	spare &= ~SPARE_REG_CLK_M_DIVISOR_MASK;

	rate = clk_get_rate(c->parent); /* the rate of osc clock */

	/* on QT platform, do not divide clk-m since it affects uart */
	if (!tegra_platform_is_silicon()) {
		if (rate == 38400000) {
			/* Set divider to (2 + 1) to still maintain
			clk_m to 13MHz instead of reporting clk_m as
			19.2 MHz when it is actually set to 13MHz */
			spare |= (2 << SPARE_REG_CLK_M_DIVISOR_SHIFT);
			if (!tegra_platform_is_qt())
				clk_writel(spare, SPARE_REG);
		}
	}

	c->div = ((spare & SPARE_REG_CLK_M_DIVISOR_MASK)
		>> SPARE_REG_CLK_M_DIVISOR_SHIFT) + 1;
	c->mul = 1;
	c->state = ON;
}

static int tegra21_clk_m_enable(struct clk *c)
{
	pr_debug("%s on clock %s\n", __func__, c->name);
	return 0;
}

static void tegra21_clk_m_disable(struct clk *c)
{
	pr_debug("%s on clock %s\n", __func__, c->name);
	WARN(1, "Attempting to disable main SoC clock\n");
}

static struct clk_ops tegra_clk_m_ops = {
	.init		= tegra21_clk_m_init,
	.enable		= tegra21_clk_m_enable,
	.disable	= tegra21_clk_m_disable,
};

static struct clk_ops tegra_clk_m_div_ops = {
	.enable		= tegra21_clk_m_enable,
};

/* PLL reference divider functions */
static void tegra21_pll_ref_init(struct clk *c)
{
	u32 pll_ref_div = clk_readl(OSC_CTRL) & OSC_CTRL_PLL_REF_DIV_MASK;
	pr_debug("%s on clock %s\n", __func__, c->name);

	switch (pll_ref_div) {
	case OSC_CTRL_PLL_REF_DIV_1:
		c->div = 1;
		break;
	case OSC_CTRL_PLL_REF_DIV_2:
		c->div = 2;
		break;
	case OSC_CTRL_PLL_REF_DIV_4:
		c->div = 4;
		break;
	default:
		pr_err("%s: Invalid pll ref divider %d", __func__, pll_ref_div);
		BUG();
	}
	c->mul = 1;
	c->state = ON;
}

static struct clk_ops tegra_pll_ref_ops = {
	.init		= tegra21_pll_ref_init,
	.enable		= tegra21_clk_m_enable,
	.disable	= tegra21_clk_m_disable,
};

/* super clock functions */
/* "super clocks" on tegra21x have two-stage muxes, fractional 7.1 divider and
 * clock skipping super divider.  We will ignore the clock skipping divider,
 * since we can't lower the voltage when using the clock skip, but we can if
 * we lower the PLL frequency. Note that skipping divider can and will be used
 * by thermal control h/w for automatic throttling. There is also a 7.1 divider
 * that most CPU super-clock inputs can be routed through. We will not use it
 * as well (keep default 1:1 state), to avoid high jitter on PLLX and DFLL path
 * and possible concurrency access issues with thermal h/w (7.1 divider setting
 * share register with clock skipping divider)
 */
static void tegra21_super_clk_init(struct clk *c)
{
	u32 val;
	int source;
	int shift;
	const struct clk_mux_sel *sel;
	val = clk_readl(c->reg + SUPER_CLK_MUX);
	c->state = ON;
#ifndef CONFIG_ARCH_TEGRA_21x_SOC
	BUG_ON(((val & SUPER_STATE_MASK) != SUPER_STATE_RUN) &&
		((val & SUPER_STATE_MASK) != SUPER_STATE_IDLE));
#endif
	shift = ((val & SUPER_STATE_MASK) == SUPER_STATE_IDLE) ?
		SUPER_IDLE_SOURCE_SHIFT : SUPER_RUN_SOURCE_SHIFT;
	source = (val >> shift) & SUPER_SOURCE_MASK;

	/*
	 * Enforce PLLX DIV2 bypass setting as early as possible. It is always
	 * safe to do for both cclk_lp and cclk_g when booting on G CPU. (In
	 * case of booting on LP CPU, cclk_lp will be updated during the cpu
	 * rate change after boot, and cclk_g after the cluster switch.)
	 */
	if (c->flags & DIV_U71) {
		val |= SUPER_LP_DIV2_BYPASS;
		clk_writel_delay(val, c->reg);
	}

	for (sel = c->inputs; sel->input != NULL; sel++) {
		if (sel->value == source)
			break;
	}
	BUG_ON(sel->input == NULL);
	c->parent = sel->input;

	/* Update parent in case when LP CPU PLLX DIV2 bypassed */
	if ((c->flags & DIV_2) && (c->parent->flags & PLLX) &&
	    (val & SUPER_LP_DIV2_BYPASS))
		c->parent = c->parent->parent;

	/* Update parent in case when LP CPU PLLX DIV2 bypassed */
	if ((c->flags & DIV_2) && (c->parent->flags & PLLX) &&
	    (val & SUPER_LP_DIV2_BYPASS))
		c->parent = c->parent->parent;

	if (c->flags & DIV_U71) {
		c->mul = 2;
		c->div = 2;

		/*
		 * Make sure 7.1 divider is 1:1, clear h/w skipper control -
		 * it will be enabled by soctherm later
		 */
		val = clk_readl(c->reg + SUPER_CLK_DIVIDER);
		BUG_ON(val & SUPER_CLOCK_DIV_U71_MASK);
		val = 0;
		clk_writel(val, c->reg + SUPER_CLK_DIVIDER);
	}
	else
		clk_writel(0, c->reg + SUPER_CLK_DIVIDER);
}

static int tegra21_super_clk_enable(struct clk *c)
{
	return 0;
}

static void tegra21_super_clk_disable(struct clk *c)
{
	/* since tegra 3 has 2 CPU super clocks - low power lp-mode clock and
	   geared up g-mode super clock - mode switch may request to disable
	   either of them; accept request with no affect on h/w */
}

static int tegra21_super_clk_set_parent(struct clk *c, struct clk *p)
{
	u32 val;
	const struct clk_mux_sel *sel;
	int shift;

	val = clk_readl(c->reg + SUPER_CLK_MUX);
	BUG_ON(((val & SUPER_STATE_MASK) != SUPER_STATE_RUN) &&
		((val & SUPER_STATE_MASK) != SUPER_STATE_IDLE));
	shift = ((val & SUPER_STATE_MASK) == SUPER_STATE_IDLE) ?
		SUPER_IDLE_SOURCE_SHIFT : SUPER_RUN_SOURCE_SHIFT;
	for (sel = c->inputs; sel->input != NULL; sel++) {
		if (sel->input == p) {
			/* For LP mode super-clock switch between PLLX direct
			   and divided-by-2 outputs is allowed only when other
			   than PLLX clock source is current parent */
			if ((c->flags & DIV_2) && (p->flags & PLLX) &&
			    ((sel->value ^ val) & SUPER_LP_DIV2_BYPASS)) {
				if (c->parent->flags & PLLX)
					return -EINVAL;
				val ^= SUPER_LP_DIV2_BYPASS;
				clk_writel_delay(val, c->reg);
			}
			val &= ~(SUPER_SOURCE_MASK << shift);
			val |= (sel->value & SUPER_SOURCE_MASK) << shift;

			if (c->flags & DIV_U71) {
				/* Make sure 7.1 divider is 1:1 */
				u32 div = clk_readl(c->reg + SUPER_CLK_DIVIDER);
				BUG_ON(div & SUPER_CLOCK_DIV_U71_MASK);
			}

			if (c->refcnt)
				clk_enable(p);

			clk_writel_delay(val, c->reg);

			if (c->refcnt && c->parent)
				clk_disable(c->parent);

			clk_reparent(c, p);
			return 0;
		}
	}
	return -EINVAL;
}

/*
 * Do not use super clocks "skippers", since dividing using a clock skipper
 * does not allow the voltage to be scaled down. Instead adjust the rate of
 * the parent clock. This requires that the parent of a super clock have no
 * other children, otherwise the rate will change underneath the other
 * children.
 */
static int tegra21_super_clk_set_rate(struct clk *c, unsigned long rate)
{
	/* In tegra21_cpu_clk_set_plls() and  tegra21_sbus_cmplx_set_rate()
	 * this call is skipped by directly setting rate of source plls. If we
	 * ever use 7.1 divider at other than 1:1 setting, or exercise s/w
	 * skipper control, not only this function, but cpu and sbus set_rate
	 * APIs should be changed accordingly.
	 */
	return clk_set_rate(c->parent, rate);
}

#ifdef CONFIG_PM_SLEEP
static void tegra21_super_clk_resume(struct clk *c, struct clk *backup,
				     u32 setting)
{
	u32 val;
	const struct clk_mux_sel *sel;
	int shift;

	/* For sclk and cclk_g super clock just restore saved value */
	if (!(c->flags & DIV_2)) {
		clk_writel_delay(setting, c->reg);
		return;
	}

	/*
	 * For cclk_lp supper clock: switch to backup (= not PLLX) source,
	 * safely restore PLLX DIV2 bypass, and only then restore full
	 * setting
	 */
	val = clk_readl(c->reg);
	BUG_ON(((val & SUPER_STATE_MASK) != SUPER_STATE_RUN) &&
		((val & SUPER_STATE_MASK) != SUPER_STATE_IDLE));
	shift = ((val & SUPER_STATE_MASK) == SUPER_STATE_IDLE) ?
		SUPER_IDLE_SOURCE_SHIFT : SUPER_RUN_SOURCE_SHIFT;
	for (sel = c->inputs; sel->input != NULL; sel++) {
		if (sel->input == backup) {
			val &= ~(SUPER_SOURCE_MASK << shift);
			val |= (sel->value & SUPER_SOURCE_MASK) << shift;

			BUG_ON(backup->flags & PLLX);
			clk_writel_delay(val, c->reg);

			val &= ~SUPER_LP_DIV2_BYPASS;
			val |= (setting & SUPER_LP_DIV2_BYPASS);
			clk_writel_delay(val, c->reg);
			clk_writel_delay(setting, c->reg);
			return;
		}
	}
	BUG();
}
#endif

static struct clk_ops tegra_super_ops = {
	.init			= tegra21_super_clk_init,
	.enable			= tegra21_super_clk_enable,
	.disable		= tegra21_super_clk_disable,
	.set_parent		= tegra21_super_clk_set_parent,
	.set_rate		= tegra21_super_clk_set_rate,
};

/* virtual cpu clock functions */
/* some clocks can not be stopped (cpu, memory bus) while the SoC is running.
   To change the frequency of these clocks, the parent pll may need to be
   reprogrammed, so the clock must be moved off the pll, the pll reprogrammed,
   and then the clock moved back to the pll.  To hide this sequence, a virtual
   clock handles it.
 */
static void tegra21_cpu_clk_init(struct clk *c)
{
	c->state = ON;
}

static int tegra21_cpu_clk_enable(struct clk *c)
{
	return 0;
}

static void tegra21_cpu_clk_disable(struct clk *c)
{
	/* since tegra 3 has 2 virtual CPU clocks - low power lp-mode clock
	   and geared up g-mode clock - mode switch may request to disable
	   either of them; accept request with no affect on h/w */
}

static int tegra21_cpu_clk_set_plls(struct clk *c, unsigned long rate,
				    unsigned long old_rate)
{
	int ret = 0;
	bool on_main = false;
	unsigned long backup_rate, main_rate;
	unsigned long vco_min = c->u.cpu.main->u.pll.vco_min;

	/*
	 * Take an extra reference to the main pll so it doesn't turn off when
	 * we move the cpu off of it. If possible, use main pll dynamic ramp
	 * to reach target rate in one shot. Otherwise, use dynamic ramp to
	 * lower current rate to pll VCO minimum level before switching to
	 * backup source.
	 */
	if (c->parent->parent == c->u.cpu.main) {
		bool dramp = (rate > c->u.cpu.backup_rate) &&
			tegra21_is_dyn_ramp(c->u.cpu.main, rate, false);
		clk_enable(c->u.cpu.main);
		on_main = true;

		if (dramp ||
		    ((old_rate > vco_min) &&
		     tegra21_is_dyn_ramp(c->u.cpu.main, vco_min, false))) {
			main_rate = dramp ? rate : vco_min;
			ret = clk_set_rate(c->u.cpu.main, main_rate);
			if (ret) {
				pr_err("Failed to set cpu rate %lu on source"
				       " %s\n", main_rate, c->u.cpu.main->name);
				goto out;
			}
			if (dramp)
				goto out;
		} else if (old_rate > vco_min) {
#if PLLX_USE_DYN_RAMP
			pr_warn("No dynamic ramp down: %s: %lu to %lu\n",
				c->u.cpu.main->name, old_rate, vco_min);
#endif
		}
	}

	/* Switch to back-up source, and stay on it if target rate is below
	   backup rate */
	if (c->parent->parent != c->u.cpu.backup) {
		ret = clk_set_parent(c->parent, c->u.cpu.backup);
		if (ret) {
			pr_err("Failed to switch cpu to %s\n",
			       c->u.cpu.backup->name);
			goto out;
		}
	}

	backup_rate = min(rate, c->u.cpu.backup_rate);
	if (backup_rate != clk_get_rate_locked(c)) {
		ret = clk_set_rate(c->u.cpu.backup, backup_rate);
		if (ret) {
			pr_err("Failed to set cpu rate %lu on backup source\n",
			       backup_rate);
			goto out;
		}
	}
	if (rate == backup_rate)
		goto out;

	/* Switch from backup source to main at rate not exceeding pll VCO
	   minimum. Use dynamic ramp to reach target rate if it is above VCO
	   minimum. */
	main_rate = rate;
	if (rate > vco_min) {
		if (tegra21_is_dyn_ramp(c->u.cpu.main, rate, true))
			main_rate = vco_min;
#if PLLX_USE_DYN_RAMP
		else
			pr_warn("No dynamic ramp up: %s: %lu to %lu\n",
				c->u.cpu.main->name, vco_min, rate);
#endif
	}

	ret = clk_set_rate(c->u.cpu.main, main_rate);
	if (ret) {
		pr_err("Failed to set cpu rate %lu on source"
		       " %s\n", main_rate, c->u.cpu.main->name);
		goto out;
	}
	ret = clk_set_parent(c->parent, c->u.cpu.main);
	if (ret) {
		pr_err("Failed to switch cpu to %s\n", c->u.cpu.main->name);
		goto out;
	}
	if (rate != main_rate) {
		ret = clk_set_rate(c->u.cpu.main, rate);
		if (ret) {
			pr_err("Failed to set cpu rate %lu on source"
			       " %s\n", rate, c->u.cpu.main->name);
			goto out;
		}
	}

out:
	if (on_main)
		clk_disable(c->u.cpu.main);

	return ret;
}

static int tegra21_cpu_clk_dfll_on(struct clk *c, unsigned long rate,
				   unsigned long old_rate)
{
	int ret;
	struct clk *dfll = c->u.cpu.dynamic;
	unsigned long dfll_rate_min = c->dvfs->dfll_data.use_dfll_rate_min;

	/* dfll rate request */
	ret = clk_set_rate(dfll, rate);
	if (ret) {
		pr_err("Failed to set cpu rate %lu on source"
		       " %s\n", rate, dfll->name);
		return ret;
	}

	/* 1st time - switch to dfll */
	if (c->parent->parent != dfll) {
		if (max(old_rate, rate) < dfll_rate_min) {
			/* set interim cpu dvfs rate at dfll_rate_min to
			   prevent voltage drop below dfll Vmin */
			ret = tegra_dvfs_set_rate(c, dfll_rate_min);
			if (ret) {
				pr_err("Failed to set cpu dvfs rate %lu\n",
				       dfll_rate_min);
				return ret;
			}
		}

		tegra_dvfs_rail_mode_updating(tegra_cpu_rail, true);
		ret = clk_set_parent(c->parent, dfll);
		if (ret) {
			tegra_dvfs_rail_mode_updating(tegra_cpu_rail, false);
			pr_err("Failed to switch cpu to %s\n", dfll->name);
			return ret;
		}
		ret = tegra_clk_cfg_ex(dfll, TEGRA_CLK_DFLL_LOCK, 1);
		WARN(ret, "Failed to lock %s at rate %lu\n", dfll->name, rate);

		/* prevent legacy dvfs voltage scaling */
		tegra_dvfs_dfll_mode_set(c->dvfs, rate);
		tegra_dvfs_rail_mode_updating(tegra_cpu_rail, false);
	}
	return 0;
}

static int tegra21_cpu_clk_dfll_off(struct clk *c, unsigned long rate,
				    unsigned long old_rate)
{
	int ret;
	struct clk *pll;
	struct clk *dfll = c->u.cpu.dynamic;
	unsigned long dfll_rate_min = c->dvfs->dfll_data.use_dfll_rate_min;

	rate = min(rate, c->max_rate - c->dvfs->dfll_data.max_rate_boost);
	pll = (rate <= c->u.cpu.backup_rate) ? c->u.cpu.backup : c->u.cpu.main;
	dfll_rate_min = max(rate, dfll_rate_min);

	/* set target rate last time in dfll mode */
	if (old_rate != dfll_rate_min) {
		ret = tegra_dvfs_set_rate(c, dfll_rate_min);
		if (!ret)
			ret = clk_set_rate(dfll, dfll_rate_min);

		if (ret) {
			pr_err("Failed to set cpu rate %lu on source %s\n",
			       dfll_rate_min, dfll->name);
			return ret;
		}
	}

	/* unlock dfll - release volatge rail control */
	tegra_dvfs_rail_mode_updating(tegra_cpu_rail, true);
	ret = tegra_clk_cfg_ex(dfll, TEGRA_CLK_DFLL_LOCK, 0);
	if (ret) {
		pr_err("Failed to unlock %s\n", dfll->name);
		goto back_to_dfll;
	}

	/* restore legacy dvfs operations and set appropriate voltage */
	ret = tegra_dvfs_dfll_mode_clear(c->dvfs, dfll_rate_min);
	if (ret) {
		pr_err("Failed to set cpu rail for rate %lu\n", rate);
		goto back_to_dfll;
	}

	/* set pll to target rate and return to pll source */
	ret = clk_set_rate(pll, rate);
	if (ret) {
		pr_err("Failed to set cpu rate %lu on source"
		       " %s\n", rate, pll->name);
		goto back_to_dfll;
	}
	ret = clk_set_parent(c->parent, pll);
	if (ret) {
		pr_err("Failed to switch cpu to %s\n", pll->name);
		goto back_to_dfll;
	}

	/* If going up, adjust voltage here (down path is taken care of by the
	   framework after set rate exit) */
	if (old_rate <= rate)
		tegra_dvfs_set_rate(c, rate);

	tegra_dvfs_rail_mode_updating(tegra_cpu_rail, false);
	return 0;

back_to_dfll:
	tegra_clk_cfg_ex(dfll, TEGRA_CLK_DFLL_LOCK, 1);
	tegra_dvfs_dfll_mode_set(c->dvfs, old_rate);
	tegra_dvfs_rail_mode_updating(tegra_cpu_rail, false);
	return ret;
}

static int tegra21_cpu_clk_set_rate(struct clk *c, unsigned long rate)
{
	unsigned long old_rate = clk_get_rate_locked(c);
	bool has_dfll = c->u.cpu.dynamic &&
		(c->u.cpu.dynamic->state != UNINITIALIZED);
	bool is_dfll = c->parent->parent == c->u.cpu.dynamic;

	/* On SILICON allow CPU rate change only if cpu regulator is connected.
	   Ignore regulator connection on FPGA and SIMULATION platforms. */
	if (c->dvfs && tegra_platform_is_silicon()) {
		if (!c->dvfs->dvfs_rail)
			return -ENOSYS;
		else if ((!c->dvfs->dvfs_rail->reg) && (old_rate < rate) &&
			 (c->boot_rate < rate)) {
			WARN(1, "Increasing CPU rate while regulator is not"
				" ready is not allowed\n");
			return -ENOSYS;
		}
	} else { /* for non silicon platform, return zero for now */
		return 0;
	}
	if (has_dfll && c->dvfs && c->dvfs->dvfs_rail) {
		if (tegra_dvfs_is_dfll_range(c->dvfs, rate))
			return tegra21_cpu_clk_dfll_on(c, rate, old_rate);
		else if (is_dfll)
			return tegra21_cpu_clk_dfll_off(c, rate, old_rate);
	}
	return tegra21_cpu_clk_set_plls(c, rate, old_rate);
}

static long tegra21_cpu_clk_round_rate(struct clk *c, unsigned long rate)
{
	unsigned long max_rate = c->max_rate;

	/* Remove dfll boost to maximum rate when running on PLL */
	if (c->dvfs && !tegra_dvfs_is_dfll_scale(c->dvfs, rate))
		max_rate -= c->dvfs->dfll_data.max_rate_boost;

	if (rate > max_rate)
		rate = max_rate;
	else if (rate < c->min_rate)
		rate = c->min_rate;
	return rate;
}

static struct clk_ops tegra_cpu_ops = {
	.init     = tegra21_cpu_clk_init,
	.enable   = tegra21_cpu_clk_enable,
	.disable  = tegra21_cpu_clk_disable,
	.set_rate = tegra21_cpu_clk_set_rate,
	.round_rate = tegra21_cpu_clk_round_rate,
};


static void tegra21_cpu_cmplx_clk_init(struct clk *c)
{
	c->parent = c->inputs[0].input;
}

/* cpu complex clock provides second level vitualization (on top of
   cpu virtual cpu rate control) in order to hide the CPU mode switch
   sequence */
#if PARAMETERIZE_CLUSTER_SWITCH
static unsigned int switch_delay;
static unsigned int switch_flags;
static DEFINE_SPINLOCK(parameters_lock);

void tegra_cluster_switch_set_parameters(unsigned int us, unsigned int flags)
{
	spin_lock(&parameters_lock);
	switch_delay = us;
	switch_flags = flags;
	spin_unlock(&parameters_lock);
}
#endif

static int tegra21_cpu_cmplx_clk_enable(struct clk *c)
{
	return 0;
}

static void tegra21_cpu_cmplx_clk_disable(struct clk *c)
{
	pr_debug("%s on clock %s\n", __func__, c->name);

	/* oops - don't disable the CPU complex clock! */
	BUG();
}

static int tegra21_cpu_cmplx_clk_set_rate(struct clk *c, unsigned long rate)
{
	unsigned long flags;
	int ret;
	struct clk *parent = c->parent;

	if (!parent->ops || !parent->ops->set_rate)
		return -ENOSYS;

	clk_lock_save(parent, &flags);

	ret = clk_set_rate_locked(parent, rate);

	clk_unlock_restore(parent, &flags);

	return ret;
}

static int tegra21_cpu_cmplx_clk_set_parent(struct clk *c, struct clk *p)
{
	int ret;
	unsigned int flags, delay;
	const struct clk_mux_sel *sel;
	unsigned long rate = clk_get_rate(c->parent);
	struct clk *dfll = c->parent->u.cpu.dynamic ? : p->u.cpu.dynamic;
	struct clk *p_source_old = NULL;
	struct clk *p_source;

	pr_debug("%s: %s %s\n", __func__, c->name, p->name);

	for (sel = c->inputs; sel->input != NULL; sel++) {
		if (sel->input == p)
			break;
	}
	if (!sel->input)
		return -EINVAL;

#if PARAMETERIZE_CLUSTER_SWITCH
	spin_lock(&parameters_lock);
	flags = switch_flags;
	delay = switch_delay;
	switch_flags = 0;
	spin_unlock(&parameters_lock);

	if (flags) {
		/* over/under-clocking after switch - allow, but update rate */
		if ((rate > p->max_rate) || (rate < p->min_rate)) {
			rate = rate > p->max_rate ? p->max_rate : p->min_rate;
			ret = clk_set_rate(c->parent, rate);
			if (ret) {
				pr_err("%s: Failed to set rate %lu for %s\n",
				        __func__, rate, p->name);
				return ret;
			}
		}
	} else
#endif
	{
		if (rate > p->max_rate) {	/* over-clocking - no switch */
			pr_warn("%s: No %s mode switch to %s at rate %lu\n",
				 __func__, c->name, p->name, rate);
			return -ECANCELED;
		}
		flags = TEGRA_POWER_CLUSTER_IMMEDIATE;
		flags |= TEGRA_POWER_CLUSTER_PART_DEFAULT;
		delay = 0;
	}
	flags |= TEGRA_POWER_CLUSTER_G;

	if (p == c->parent) {
		if (flags & TEGRA_POWER_CLUSTER_FORCE) {
			/* Allow parameterized switch to the same mode */
			ret = tegra_cluster_control(delay, flags);
			if (ret)
				pr_err("%s: Failed to force %s mode to %s\n",
				       __func__, c->name, p->name);
			return ret;
		}
		return 0;	/* already switched - exit */
	}

	tegra_dvfs_rail_mode_updating(tegra_cpu_rail, true);
	if (c->parent->parent->parent == dfll) {
		/* G (DFLL selected as clock source) => LP switch:
		 * turn DFLL into open loop mode ("release" VDD_CPU rail)
		 * select target p_source for LP, and get its rate ready
		 */
		ret = tegra_clk_cfg_ex(dfll, TEGRA_CLK_DFLL_LOCK, 0);
		if (ret)
			goto abort;

		p_source = rate <= p->u.cpu.backup_rate ?
			p->u.cpu.backup : p->u.cpu.main;
		ret = clk_set_rate(p_source, rate);
		if (ret)
			goto abort;
	} else if ((p->parent->parent == dfll) ||
		   (p->dvfs && tegra_dvfs_is_dfll_range(p->dvfs, rate))) {
		/* LP => G (DFLL selected as clock source) switch:
		 * set DFLL rate ready (DFLL is still disabled)
		 * (set target p_source as dfll, G source is already selected)
		 */
		p_source = dfll;
		ret = clk_set_rate(dfll,
			tegra_dvfs_rail_is_dfll_mode(tegra_cpu_rail) ? rate :
			max(rate, p->dvfs->dfll_data.use_dfll_rate_min));
		if (ret)
			goto abort;

		ret = tegra_dvfs_rail_dfll_mode_set_cold(tegra_cpu_rail, dfll);
		if (ret)
			goto abort;
	} else
		/* DFLL is not selected on either side of the switch:
		 * set target p_source equal to current clock source
		 */
		p_source = c->parent->parent->parent;

	/* Switch new parent to target clock source if necessary */
	if (p->parent->parent != p_source) {
		clk_enable(p->parent->parent);
		clk_enable(p->parent);
		p_source_old = p->parent->parent;
		ret = clk_set_parent(p->parent, p_source);
		if (ret) {
			pr_err("%s: Failed to set parent %s for %s\n",
			       __func__, p_source->name, p->name);
			goto abort;
		}
	}

	/* Enabling new parent scales new mode voltage rail in advanvce
	   before the switch happens (if p_source is DFLL: open loop mode) */
	if (c->refcnt)
		clk_enable(p);

	/* switch CPU mode */
	ret = tegra_cluster_control(delay, flags);
	if (ret) {
		if (c->refcnt)
			clk_disable(p);
		pr_err("%s: Failed to switch %s mode to %s\n",
		       __func__, c->name, p->name);
		goto abort;
	}

	/* Disabling old parent scales old mode voltage rail */
	if (c->refcnt)
		clk_disable(c->parent);
	if (p_source_old) {
		clk_disable(p->parent);
		clk_disable(p_source_old);
	}

	clk_reparent(c, p);

	/*
	 * Lock DFLL now (resume closed loop VDD_CPU control).
	 * G CPU operations are resumed on DFLL if it was the last G CPU
	 * clock source, or if resume rate is in DFLL usage range in case
	 * when auto-switch between PLL and DFLL is enabled.
	 */
	if (p_source == dfll) {
		if (tegra_dvfs_rail_is_dfll_mode(tegra_cpu_rail)) {
			tegra_clk_cfg_ex(dfll, TEGRA_CLK_DFLL_LOCK, 1);
		} else {
			clk_set_rate(dfll, rate);
			tegra_clk_cfg_ex(dfll, TEGRA_CLK_DFLL_LOCK, 1);
			tegra_dvfs_dfll_mode_set(p->dvfs, rate);
		}
	}

	tegra_dvfs_rail_mode_updating(tegra_cpu_rail, false);
	return 0;

abort:
	/* Re-lock DFLL if necessary after aborted switch */
	if (c->parent->parent->parent == dfll) {
		clk_set_rate(dfll, rate);
		tegra_clk_cfg_ex(dfll, TEGRA_CLK_DFLL_LOCK, 1);
	}
	if (p_source_old) {
		clk_disable(p->parent);
		clk_disable(p_source_old);
	}
	tegra_dvfs_rail_mode_updating(tegra_cpu_rail, false);

	pr_err("%s: aborted switch from %s to %s\n",
	       __func__, c->parent->name, p->name);
	return ret;
}

static long tegra21_cpu_cmplx_round_rate(struct clk *c,
	unsigned long rate)
{
	return clk_round_rate(c->parent, rate);
}

static struct clk_ops tegra_cpu_cmplx_ops = {
	.init     = tegra21_cpu_cmplx_clk_init,
	.enable   = tegra21_cpu_cmplx_clk_enable,
	.disable  = tegra21_cpu_cmplx_clk_disable,
	.set_rate = tegra21_cpu_cmplx_clk_set_rate,
	.set_parent = tegra21_cpu_cmplx_clk_set_parent,
	.round_rate = tegra21_cpu_cmplx_round_rate,
};

/* virtual cop clock functions. Used to acquire the fake 'cop' clock to
 * reset the COP block (i.e. AVP) */
static void tegra21_cop_clk_reset(struct clk *c, bool assert)
{
	unsigned long reg = assert ? RST_DEVICES_SET_L : RST_DEVICES_CLR_L;

	pr_debug("%s %s\n", __func__, assert ? "assert" : "deassert");
	clk_writel(1 << 1, reg);
}

static struct clk_ops tegra_cop_ops = {
	.reset    = tegra21_cop_clk_reset,
};

/* bus clock functions */
static DEFINE_SPINLOCK(bus_clk_lock);

static int bus_set_div(struct clk *c, int div)
{
	u32 val;
	unsigned long flags;

	if (!div || (div > (BUS_CLK_DIV_MASK + 1)))
		return -EINVAL;

	spin_lock_irqsave(&bus_clk_lock, flags);
	val = clk_readl(c->reg);
	val &= ~(BUS_CLK_DIV_MASK << c->reg_shift);
	val |= (div - 1) << c->reg_shift;
	clk_writel(val, c->reg);
	c->div = div;
	spin_unlock_irqrestore(&bus_clk_lock, flags);

	return 0;
}

static void tegra21_bus_clk_init(struct clk *c)
{
	u32 val = clk_readl(c->reg);
	c->state = ((val >> c->reg_shift) & BUS_CLK_DISABLE) ? OFF : ON;
	c->div = ((val >> c->reg_shift) & BUS_CLK_DIV_MASK) + 1;
	c->mul = 1;
}

static int tegra21_bus_clk_enable(struct clk *c)
{
	u32 val = clk_readl(c->reg);
	val &= ~(BUS_CLK_DISABLE << c->reg_shift);
	clk_writel(val, c->reg);
	return 0;
}

static void tegra21_bus_clk_disable(struct clk *c)
{
	u32 val = clk_readl(c->reg);
	val |= BUS_CLK_DISABLE << c->reg_shift;
	clk_writel(val, c->reg);
}

static int tegra21_bus_clk_set_rate(struct clk *c, unsigned long rate)
{
	unsigned long parent_rate = clk_get_rate(c->parent);
	int i;

	for (i = 1; i <= 4; i++) {
		if (rate >= parent_rate / i)
			return bus_set_div(c, i);
	}
	return -EINVAL;
}

static struct clk_ops tegra_bus_ops = {
	.init			= tegra21_bus_clk_init,
	.enable			= tegra21_bus_clk_enable,
	.disable		= tegra21_bus_clk_disable,
	.set_rate		= tegra21_bus_clk_set_rate,
};

/*
 * Virtual system bus complex clock is used to hide the sequence of
 * changing sclk/hclk/pclk parents and dividers to configure requested
 * sclk target rate.
 */
#define BUS_AHB_DIV_MAX			(BUS_CLK_DIV_MASK + 1UL)
#define BUS_APB_DIV_MAX			(BUS_CLK_DIV_MASK + 1UL)

static unsigned long sclk_pclk_unity_ratio_rate_max = 136000000;

struct clk_div_sel {
	struct clk *src;
	u32 div;
	unsigned long rate;
};
static struct clk_div_sel sbus_round_table[MAX_DVFS_FREQS + 1];
static int sbus_round_table_size;

static int last_round_idx;
static int get_start_idx(unsigned long rate)
{
	int i = last_round_idx;
	if (rate == sbus_round_table[i].rate)
		return i;
	return 0;
}


static void tegra21_sbus_cmplx_init(struct clk *c)
{
	unsigned long rate;
	struct clk *sclk_div = c->parent->parent;

	c->max_rate = sclk_div->max_rate;
	c->min_rate = sclk_div->min_rate;

	rate = clk_get_rate(c->u.system.sclk_low);
	if (tegra_platform_is_qt())
		return;

	/* Unity threshold must be an exact proper factor of low range parent */
	BUG_ON((rate % sclk_pclk_unity_ratio_rate_max) != 0);
	BUG_ON(!(sclk_div->flags & DIV_U71));
}

/* This special sbus round function is implemented because:
 *
 * (a) sbus complex clock source is selected automatically based on rate
 *
 * (b) since sbus is a shared bus, and its frequency is set to the highest
 * enabled shared_bus_user clock, the target rate should be rounded up divider
 * ladder (if max limit allows it) - for pll_div and peripheral_div common is
 * rounding down - special case again.
 *
 * Note that final rate is trimmed (not rounded up) to avoid spiraling up in
 * recursive calls. Lost 1Hz is added in tegra21_sbus_cmplx_set_rate before
 * actually setting divider rate.
 */
static void sbus_build_round_table_one(struct clk *c, unsigned long rate, int j)
{
	struct clk_div_sel sel;
	struct clk *sclk_div = c->parent->parent;
	u32 flags = sclk_div->flags;

	sel.src = c->u.system.sclk_low;
	sel.rate = fixed_src_bus_round_updown(
		c, sel.src, flags, rate, false, &sel.div);
	sbus_round_table[j] = sel;

	sel.src = c->u.system.sclk_high;
	sel.rate = fixed_src_bus_round_updown(
		c, sel.src, flags, rate, false, &sel.div);
	if (sbus_round_table[j].rate < sel.rate)
		sbus_round_table[j] = sel;
}

/* Populate sbus (not Avalon) round table with dvfs entries (not knights) */
static void sbus_build_round_table(struct clk *c)
{
	int i, j = 0;
	unsigned long rate;
	bool inserted = false;

	/*
	 * Make sure unity ratio threshold always inserted into the table.
	 * If no dvfs specified, just add maximum rate entry. Othrwise, add
	 * entries for all dvfs rates.
	 */
	if (!c->dvfs || !c->dvfs->num_freqs) {
		sbus_build_round_table_one(
			c, sclk_pclk_unity_ratio_rate_max, j++);
		sbus_build_round_table_one(
			c, c->max_rate, j++);
		sbus_round_table_size = j;
		return;
	}

	for (i = 0; i < c->dvfs->num_freqs; i++) {
		rate = c->dvfs->freqs[i];
		if (rate <= 1 * c->dvfs->freqs_mult)
			continue; /* skip 1kHz place holders */

		if (!inserted && (rate >= sclk_pclk_unity_ratio_rate_max)) {
			inserted = true;
			if (rate > sclk_pclk_unity_ratio_rate_max)
				sbus_build_round_table_one(
					c, sclk_pclk_unity_ratio_rate_max, j++);
		}
		sbus_build_round_table_one(c, rate, j++);
	}
	sbus_round_table_size = j;
}

/* Clip requested rate to the entry in the round table. Allow +/-1Hz slack. */
static long tegra21_sbus_cmplx_round_updown(struct clk *c, unsigned long rate,
					    bool up)
{
	int i;

	if (!sbus_round_table_size) {
		sbus_build_round_table(c);
		if (!sbus_round_table_size) {
			WARN(1, "Invalid sbus round table\n");
			return -EINVAL;
		}
	}

	rate = max(rate, c->min_rate);

	i = get_start_idx(rate);
	for (; i < sbus_round_table_size - 1; i++) {
		unsigned long sel_rate = sbus_round_table[i].rate;
		if (abs(rate - sel_rate) <= 1) {
			break;
		} else if (rate < sel_rate) {
			if (!up && i)
				i--;
			break;
		}
	}
	last_round_idx = i;
	return sbus_round_table[i].rate;
}

static long tegra21_sbus_cmplx_round_rate(struct clk *c, unsigned long rate)
{
	return tegra21_sbus_cmplx_round_updown(c, rate, true);
}

/*
 * Select {source : divider} setting from pre-built round table, and actually
 * change the configuration. Since voltage during switch is at safe level for
 * current and new sbus rates (but not above) over-clocking during the switch
 * is not allowed. Hence, the order of switch: 1st change divider if its setting
 * increases, then switch source clock, and finally change divider if it goes
 * down. No over-clocking is guaranteed, but dip below both initial and final
 * rates is possible.
 */
static int tegra21_sbus_cmplx_set_rate(struct clk *c, unsigned long rate)
{
	int ret, i;
	struct clk *skipper = c->parent;
	struct clk *sclk_div = skipper->parent;
	struct clk *sclk_mux = sclk_div->parent;
	struct clk_div_sel *new_sel = NULL;
	unsigned long sclk_div_rate = clk_get_rate(sclk_div);

	/*
	 * Configure SCLK/HCLK/PCLK guranteed safe combination:
	 * - keep hclk at the same rate as sclk
	 * - set pclk at 1:2 rate of hclk
	 * - disable sclk skipper
	 */
	bus_set_div(c->u.system.pclk, 2);
	bus_set_div(c->u.system.hclk, 1);
	c->child_bus->child_bus->div = 2;
	c->child_bus->div = 1;
	clk_set_rate(skipper, c->max_rate);

	/* Select new source/divider */
	i = get_start_idx(rate);
	for (; i < sbus_round_table_size; i++) {
		if (rate == sbus_round_table[i].rate) {
			new_sel = &sbus_round_table[i];
			break;
		}
	}
	if (!new_sel)
		return -EINVAL;

	if (sclk_div_rate == rate) {
		pr_debug("sbus_set_rate: no change in rate %lu on parent %s\n",
			 clk_get_rate_locked(c), sclk_mux->parent->name);
		return 0;
	}

	/* Raise voltage on the way up */
	if (c->dvfs && (rate > sclk_div_rate)) {
		ret = tegra_dvfs_set_rate(c, rate);
		if (ret)
			return ret;
		pr_debug("sbus_set_rate: set %d mV\n", c->dvfs->cur_millivolts);
	}

	/* Do switch */
	if (sclk_div->div < new_sel->div) {
		unsigned long sdiv_rate = sclk_div_rate * sclk_div->div;
		sdiv_rate = DIV_ROUND_UP(sdiv_rate, new_sel->div);
		ret = clk_set_rate(sclk_div, sdiv_rate);
		if (ret) {
			pr_err("%s: Failed to set %s rate to %lu\n",
			       __func__, sclk_div->name, sdiv_rate);
			return ret;
		}
		pr_debug("sbus_set_rate: rate %lu on parent %s\n",
			 clk_get_rate_locked(c), sclk_mux->parent->name);

	}

	if (new_sel->src != sclk_mux->parent) {
		ret = clk_set_parent(sclk_mux, new_sel->src);
		if (ret) {
			pr_err("%s: Failed to switch sclk source to %s\n",
			       __func__, new_sel->src->name);
			return ret;
		}
		pr_debug("sbus_set_rate: rate %lu on parent %s\n",
			 clk_get_rate_locked(c), sclk_mux->parent->name);
	}

	if (sclk_div->div > new_sel->div) {
		ret = clk_set_rate(sclk_div, rate + 1);
		if (ret) {
			pr_err("%s: Failed to set %s rate to %lu\n",
			       __func__, sclk_div->name, rate);
			return ret;
		}
		pr_debug("sbus_set_rate: rate %lu on parent %s\n",
			 clk_get_rate_locked(c), sclk_mux->parent->name);
	}

	/* Lower voltage on the way down */
	if (c->dvfs && (rate < sclk_div_rate)) {
		ret = tegra_dvfs_set_rate(c, rate);
		if (ret)
			return ret;
		pr_debug("sbus_set_rate: set %d mV\n", c->dvfs->cur_millivolts);
	}

	return 0;
}

/*
 * Limitations on SCLK/HCLK/PCLK ratios:
 * (A) H/w limitation:
 *	if SCLK >= 136MHz, SCLK:PCLK >= 2
 * (B) S/w policy limitation, in addition to (A):
 *	if any APB bus shared user request is enabled, HCLK:PCLK >= 2
 *  Reason for (B): assuming APB bus shared user has requested X < 136MHz,
 *  HCLK = PCLK = X, and new AHB user is coming on-line requesting Y >= 136MHz,
 *  we can consider 2 paths depending on order of changing HCLK rate and
 *  HCLK:PCLK ratio
 *  (i)  HCLK:PCLK = X:X => Y:Y* => Y:Y/2,   (*) violates rule (A)
 *  (ii) HCLK:PCLK = X:X => X:X/2* => Y:Y/2, (*) under-clocks APB user
 *  In this case we can not guarantee safe transition from HCLK:PCLK = 1:1
 *  below 60MHz to HCLK rate above 60MHz without under-clocking APB user.
 *  Hence, policy (B).
 *
 *  When there are no request from APB users, path (ii) can be used to
 *  increase HCLK above 136MHz, and HCLK:PCLK = 1:1 is allowed.
 *
 *  Note: with common divider used in the path for all SCLK sources SCLK rate
 *  during switching may dip down, anyway. So, in general, policy (ii) does not
 *  prevent underclocking users during clock transition.
 */
static int tegra21_clk_sbus_update(struct clk *bus)
{
	int ret, div;
	bool p_requested;
	unsigned long s_rate, h_rate, p_rate, ceiling, s_rate_raw;
	struct clk *ahb, *apb;
	struct clk *skipper = bus->parent;

	if (detach_shared_bus)
		return 0;

	s_rate = tegra21_clk_shared_bus_update(bus, &ahb, &apb, &ceiling);
	if (bus->override_rate) {
		ret = clk_set_rate_locked(bus, s_rate);
		if (!ret)
			clk_set_rate(skipper, s_rate);
		return ret;
	}

	ahb = bus->child_bus;
	apb = ahb->child_bus;
	h_rate = ahb->u.shared_bus_user.rate;
	p_rate = apb->u.shared_bus_user.rate;
	p_requested = apb->refcnt > 1;

	/* Propagate ratio requirements up from PCLK to SCLK */
	if (p_requested)
		h_rate = max(h_rate, p_rate * 2);
	s_rate = max(s_rate, h_rate);
	if (s_rate >= sclk_pclk_unity_ratio_rate_max)
		s_rate = max(s_rate, p_rate * 2);

	/* Propagate cap requirements down from SCLK to PCLK */
	s_rate_raw = s_rate;
	s_rate = tegra21_clk_cap_shared_bus(bus, s_rate, ceiling);
	if (s_rate >= sclk_pclk_unity_ratio_rate_max)
		p_rate = min(p_rate, s_rate / 2);
	h_rate = min(h_rate, s_rate);
	if (p_requested)
		p_rate = min(p_rate, h_rate / 2);


	/* Set new sclk rate in safe 1:1:2, rounded "up" configuration */
	ret = clk_set_rate_locked(bus, s_rate);
	if (ret)
		return ret;
	clk_set_rate(skipper, s_rate_raw);

	/* Finally settle new bus divider values */
	s_rate = clk_get_rate_locked(bus);
	div = min(s_rate / h_rate, BUS_AHB_DIV_MAX);
	if (div != 1) {
		bus_set_div(bus->u.system.hclk, div);
		ahb->div = div;
	}

	h_rate = clk_get_rate(bus->u.system.hclk);
	div = min(h_rate / p_rate, BUS_APB_DIV_MAX);
	if (div != 2) {
		bus_set_div(bus->u.system.pclk, div);
		apb->div = div;
	}

	return 0;
}

static struct clk_ops tegra_sbus_cmplx_ops = {
	.init = tegra21_sbus_cmplx_init,
	.set_rate = tegra21_sbus_cmplx_set_rate,
	.round_rate = tegra21_sbus_cmplx_round_rate,
	.round_rate_updown = tegra21_sbus_cmplx_round_updown,
	.shared_bus_update = tegra21_clk_sbus_update,
};

/* Blink output functions */

static void tegra21_blink_clk_init(struct clk *c)
{
	u32 val;

	val = pmc_readl(PMC_CTRL);
	c->state = (val & PMC_CTRL_BLINK_ENB) ? ON : OFF;
	c->mul = 1;
	val = pmc_readl(c->reg);

	if (val & PMC_BLINK_TIMER_ENB) {
		unsigned int on_off;

		on_off = (val >> PMC_BLINK_TIMER_DATA_ON_SHIFT) &
			PMC_BLINK_TIMER_DATA_ON_MASK;
		val >>= PMC_BLINK_TIMER_DATA_OFF_SHIFT;
		val &= PMC_BLINK_TIMER_DATA_OFF_MASK;
		on_off += val;
		/* each tick in the blink timer is 4 32KHz clocks */
		c->div = on_off * 4;
	} else {
		c->div = 1;
	}
}

static int tegra21_blink_clk_enable(struct clk *c)
{
	u32 val;

	val = pmc_readl(PMC_DPD_PADS_ORIDE);
	pmc_writel(val | PMC_DPD_PADS_ORIDE_BLINK_ENB, PMC_DPD_PADS_ORIDE);

	val = pmc_readl(PMC_CTRL);
	pmc_writel(val | PMC_CTRL_BLINK_ENB, PMC_CTRL);

	return 0;
}

static void tegra21_blink_clk_disable(struct clk *c)
{
	u32 val;

	val = pmc_readl(PMC_CTRL);
	pmc_writel(val & ~PMC_CTRL_BLINK_ENB, PMC_CTRL);

	val = pmc_readl(PMC_DPD_PADS_ORIDE);
	pmc_writel(val & ~PMC_DPD_PADS_ORIDE_BLINK_ENB, PMC_DPD_PADS_ORIDE);
}

static int tegra21_blink_clk_set_rate(struct clk *c, unsigned long rate)
{
	unsigned long parent_rate = clk_get_rate(c->parent);
	if (rate >= parent_rate) {
		c->div = 1;
		pmc_writel(0, c->reg);
	} else {
		unsigned int on_off;
		u32 val;

		on_off = DIV_ROUND_UP(parent_rate / 8, rate);
		c->div = on_off * 8;

		val = (on_off & PMC_BLINK_TIMER_DATA_ON_MASK) <<
			PMC_BLINK_TIMER_DATA_ON_SHIFT;
		on_off &= PMC_BLINK_TIMER_DATA_OFF_MASK;
		on_off <<= PMC_BLINK_TIMER_DATA_OFF_SHIFT;
		val |= on_off;
		val |= PMC_BLINK_TIMER_ENB;
		pmc_writel(val, c->reg);
	}

	return 0;
}

static struct clk_ops tegra_blink_clk_ops = {
	.init			= &tegra21_blink_clk_init,
	.enable			= &tegra21_blink_clk_enable,
	.disable		= &tegra21_blink_clk_disable,
	.set_rate		= &tegra21_blink_clk_set_rate,
};

/* PLL Functions */
static int tegra21_pll_clk_wait_for_lock(
	struct clk *c, u32 lock_reg, u32 lock_bits)
{
#if USE_PLL_LOCK_BITS
	int i;
	u32 val = 0;

	for (i = 0; i < (c->u.pll.lock_delay / PLL_PRE_LOCK_DELAY + 1); i++) {
		udelay(PLL_PRE_LOCK_DELAY);
		val = clk_readl(lock_reg);
		if ((val & lock_bits) == lock_bits) {
			udelay(PLL_POST_LOCK_DELAY);
			return 0;
		}
	}

	if (tegra_platform_is_silicon())
		pr_err("Timed out waiting for %s lock bit ([0x%x] = 0x%x)\n",
		       c->name, lock_reg, val);
	return -ETIMEDOUT;
#endif
	udelay(c->u.pll.lock_delay);
	return 0;
}

static void usb_plls_hw_control_enable(u32 reg)
{
	u32 val = clk_readl(reg);
	val |= USB_PLLS_USE_LOCKDET | USB_PLLS_SEQ_START_STATE;
	val &= ~USB_PLLS_ENABLE_SWCTL;
	val |= USB_PLLS_SEQ_START_STATE;
	pll_writel_delay(val, reg);

	val |= USB_PLLS_SEQ_ENABLE;
	pll_writel_delay(val, reg);
}

static void tegra21_utmi_param_configure(struct clk *c)
{
	u32 reg;
	int i;
	unsigned long main_rate =
		clk_get_rate(c->parent->parent);

	for (i = 0; i < ARRAY_SIZE(utmi_parameters); i++) {
		if (main_rate == utmi_parameters[i].osc_frequency) {
			break;
		}
	}

	if (i >= ARRAY_SIZE(utmi_parameters)) {
		pr_err("%s: Unexpected main rate %lu\n", __func__, main_rate);
		return;
	}

	reg = clk_readl(UTMIP_PLL_CFG2);

	/* Program UTMIP PLL stable and active counts */
	/* [FIXME] arclk_rst.h says WRONG! This should be 1ms -> 0x50 Check! */
	reg &= ~UTMIP_PLL_CFG2_STABLE_COUNT(~0);
	reg |= UTMIP_PLL_CFG2_STABLE_COUNT(
			utmi_parameters[i].stable_count);

	reg &= ~UTMIP_PLL_CFG2_ACTIVE_DLY_COUNT(~0);

	reg |= UTMIP_PLL_CFG2_ACTIVE_DLY_COUNT(
			utmi_parameters[i].active_delay_count);

	/* Remove power downs from UTMIP PLL control bits */
	reg |= UTMIP_PLL_CFG2_FORCE_PD_SAMP_A_POWERUP;
	reg |= UTMIP_PLL_CFG2_FORCE_PD_SAMP_B_POWERUP;
	reg |= UTMIP_PLL_CFG2_FORCE_PD_SAMP_C_POWERUP;
	reg |= UTMIP_PLL_CFG2_FORCE_PD_SAMP_D_POWERUP;
	reg &= ~UTMIP_PLL_CFG2_FORCE_PD_SAMP_A_POWERDOWN;
	reg &= ~UTMIP_PLL_CFG2_FORCE_PD_SAMP_B_POWERDOWN;
	reg &= ~UTMIP_PLL_CFG2_FORCE_PD_SAMP_C_POWERDOWN;
	reg &= ~UTMIP_PLL_CFG2_FORCE_PD_SAMP_D_POWERDOWN;

	clk_writel(reg, UTMIP_PLL_CFG2);

	/* Program UTMIP PLL delay and oscillator frequency counts */
	reg = clk_readl(UTMIP_PLL_CFG1);
	reg &= ~UTMIP_PLL_CFG1_ENABLE_DLY_COUNT(~0);

	reg |= UTMIP_PLL_CFG1_ENABLE_DLY_COUNT(
		utmi_parameters[i].enable_delay_count);

	reg &= ~UTMIP_PLL_CFG1_XTAL_FREQ_COUNT(~0);
	reg |= UTMIP_PLL_CFG1_XTAL_FREQ_COUNT(
		utmi_parameters[i].xtal_freq_count);

	/* Remove power downs from UTMIP PLL control bits */
	reg &= ~UTMIP_PLL_CFG1_FORCE_PLL_ENABLE_POWERDOWN;
	reg &= ~UTMIP_PLL_CFG1_FORCE_PLL_ACTIVE_POWERDOWN;
	reg &= ~UTMIP_PLL_CFG1_FORCE_PLLU_POWERUP;
	reg &= ~UTMIP_PLL_CFG1_FORCE_PLLU_POWERDOWN;
	clk_writel(reg, UTMIP_PLL_CFG1);

	/* Setup HW control of UTMIPLL */
	reg = clk_readl(UTMIPLL_HW_PWRDN_CFG0);
	reg |= UTMIPLL_HW_PWRDN_CFG0_USE_LOCKDET;
	reg &= ~UTMIPLL_HW_PWRDN_CFG0_CLK_ENABLE_SWCTL;
	reg |= UTMIPLL_HW_PWRDN_CFG0_SEQ_START_STATE;
	clk_writel(reg, UTMIPLL_HW_PWRDN_CFG0);

	reg = clk_readl(UTMIP_PLL_CFG1);
	reg &= ~UTMIP_PLL_CFG1_FORCE_PLL_ENABLE_POWERUP;
	reg &= ~UTMIP_PLL_CFG1_FORCE_PLL_ENABLE_POWERDOWN;
	pll_writel_delay(reg, UTMIP_PLL_CFG1);

	/* Setup SW override of UTMIPLL assuming USB2.0
	   ports are assigned to USB2 */
	reg = clk_readl(UTMIPLL_HW_PWRDN_CFG0);
	reg |= UTMIPLL_HW_PWRDN_CFG0_IDDQ_SWCTL;
	reg |= UTMIPLL_HW_PWRDN_CFG0_IDDQ_OVERRIDE;
	pll_writel_delay(reg, UTMIPLL_HW_PWRDN_CFG0);

	/* Enable HW control UTMIPLL */
	reg = clk_readl(UTMIPLL_HW_PWRDN_CFG0);
	reg |= UTMIPLL_HW_PWRDN_CFG0_SEQ_ENABLE;
	pll_writel_delay(reg, UTMIPLL_HW_PWRDN_CFG0);
}

static void tegra21_pll_clk_init(struct clk *c)
{
	u32 val = clk_readl(c->reg + PLL_BASE);
	u32 divn_shift = PLL_BASE_DIVN_SHIFT;
	u32 divn_mask = PLL_BASE_DIVN_MASK;
	u32 divm_mask = PLL_BASE_DIVM_MASK;

	c->state = (val & PLL_BASE_ENABLE) ? ON : OFF;

	if (c->flags & PLL_FIXED && !(val & PLL_BASE_OVERRIDE)) {
		const struct clk_pll_freq_table *sel;
		unsigned long input_rate = clk_get_rate(c->parent);
		c->u.pll.fixed_rate = PLLP_DEFAULT_FIXED_RATE;

		for (sel = c->u.pll.freq_table; sel->input_rate != 0; sel++) {
			if (sel->input_rate == input_rate &&
				sel->output_rate == c->u.pll.fixed_rate) {
				c->mul = sel->n;
				c->div = sel->m * sel->p;
				return;
			}
		}
		pr_err("Clock %s has unknown fixed frequency\n", c->name);
		BUG();
	} else if (val & PLL_BASE_BYPASS) {
		c->mul = 1;
		c->div = 1;
	} else {
		if (c->flags & PLLD) {
			/* FIXME: no longer happens to be removed
			divn_shift = PLLD_BASE_DIVN_SHIFT;
			divn_mask = PLLD_BASE_DIVN_MASK;
			*/
		} else if (c->flags & PLLU) {
			/* FIXME: no longer happens to be removed
			divn_mask = PLLU_BASE_DIVN_MASK;
			divm_mask = PLLC_BASE_DIVM_MASK;
			*/
		}
		c->mul = (val & divn_mask) >> divn_shift;
		c->div = (val & divm_mask) >> PLL_BASE_DIVM_SHIFT;
		if (!(c->flags & PLLU))
			c->div *= (0x1 << ((val & PLL_BASE_DIVP_MASK) >>
					PLL_BASE_DIVP_SHIFT));
	}

	if (c->flags & PLL_FIXED) {
		c->u.pll.fixed_rate = clk_get_rate_locked(c);
	}

}

static int tegra21_pll_clk_enable(struct clk *c)
{
	u32 val;
	pr_debug("%s on clock %s\n", __func__, c->name);

#if USE_PLL_LOCK_BITS
	/* toggle lock enable bit to reset lock detection circuit (couple
	   register reads provide enough duration for reset pulse) */
	val = clk_readl(c->reg + PLL_MISC(c));
	val &= ~PLL_MISC_LOCK_ENABLE(c);
	clk_writel(val, c->reg + PLL_MISC(c));
	val = clk_readl(c->reg + PLL_MISC(c));
	val = clk_readl(c->reg + PLL_MISC(c));
	val |= PLL_MISC_LOCK_ENABLE(c);
	clk_writel(val, c->reg + PLL_MISC(c));
#endif
	val = clk_readl(c->reg + PLL_BASE);
	val &= ~PLL_BASE_BYPASS;
	val |= PLL_BASE_ENABLE;
	clk_writel(val, c->reg + PLL_BASE);

	tegra21_pll_clk_wait_for_lock(c, c->reg + PLL_BASE, PLL_BASE_LOCK);

	return 0;
}

static void tegra21_pll_clk_disable(struct clk *c)
{
	u32 val;
	pr_debug("%s on clock %s\n", __func__, c->name);

	val = clk_readl(c->reg);
	val &= ~(PLL_BASE_BYPASS | PLL_BASE_ENABLE);
	clk_writel(val, c->reg);
}

static int tegra21_pll_clk_set_rate(struct clk *c, unsigned long rate)
{
	u32 val, p_div, old_base;
	unsigned long input_rate;
	const struct clk_pll_freq_table *sel;
	struct clk_pll_freq_table cfg;

	pr_debug("%s: %s %lu\n", __func__, c->name, rate);

	if (c->flags & PLL_FIXED) {
		int ret = 0;
		if (rate != c->u.pll.fixed_rate) {
			pr_err("%s: Can not change %s fixed rate %lu to %lu\n",
			       __func__, c->name, c->u.pll.fixed_rate, rate);
			ret = -EINVAL;
		}
		return ret;
	}

	p_div = 0;
	input_rate = clk_get_rate(c->parent);

	/* Check if the target rate is tabulated */
	for (sel = c->u.pll.freq_table; sel->input_rate != 0; sel++) {
		if (sel->input_rate == input_rate && sel->output_rate == rate) {
			if (c->flags & PLLU) {
				BUG_ON(sel->p < 1 || sel->p > 2);
				if (sel->p == 1)
					p_div = PLLU_BASE_POST_DIV;
			} else {
				BUG_ON(sel->p < 1);
				for (val = sel->p; val > 1; val >>= 1, p_div++);
				p_div <<= PLL_BASE_DIVP_SHIFT;
			}
			break;
		}
	}

	/* Configure out-of-table rate */
	if (sel->input_rate == 0) {
		unsigned long cfreq, vco;
		BUG_ON(c->flags & PLLU);
		sel = &cfg;

		switch (input_rate) {
		case 12000000:
			cfreq = (rate <= 1000000 * 1000) ? 1000000 : 2000000;
			break;
		case 13000000:
			cfreq = (rate <= 1000000 * 1000) ? 1000000 : 2600000;
			break;
		case 19200000:
			cfreq = (rate <= 1200000 * 1000) ? 1200000 : 2400000;
			break;
		case 38400000:
			cfreq = 2400000;
			break;
		default:
			if (c->parent->flags & DIV_U71_FIXED) {
				/* PLLP_OUT1 rate is not in PLLA table */
				pr_warn("%s: failed %s ref/out rates %lu/%lu\n",
					__func__, c->name, input_rate, rate);
				cfreq = input_rate/(input_rate/1000000);
				break;
			}
			pr_err("%s: Unexpected reference rate %lu\n",
			       __func__, input_rate);
			BUG();
		}

		/* Raise VCO to guarantee 0.5% accuracy, and vco min boundary */
		vco = max(200 * cfreq, c->u.pll.vco_min);
		for (cfg.output_rate = rate; cfg.output_rate < vco; p_div++)
			cfg.output_rate <<= 1;

		cfg.p = 0x1 << p_div;
		cfg.m = input_rate / cfreq;
		cfg.n = cfg.output_rate / cfreq;
		cfg.cpcon = c->u.pll.cpcon_default ? : OUT_OF_TABLE_CPCON;

		if ((cfg.m > (PLL_BASE_DIVM_MASK >> PLL_BASE_DIVM_SHIFT)) ||
		    (cfg.n > (PLL_BASE_DIVN_MASK >> PLL_BASE_DIVN_SHIFT)) ||
		    (p_div > (PLL_BASE_DIVP_MASK >> PLL_BASE_DIVP_SHIFT)) ||
		    (cfg.output_rate > c->u.pll.vco_max)) {
			pr_err("%s: Failed to set %s out-of-table rate %lu\n",
			       __func__, c->name, rate);
			return -EINVAL;
		}
		p_div <<= PLL_BASE_DIVP_SHIFT;
	}

	c->mul = sel->n;
	c->div = sel->m * sel->p;

	old_base = val = clk_readl(c->reg + PLL_BASE);
	val &= ~(PLL_BASE_DIVM_MASK | PLL_BASE_DIVN_MASK |
		 ((c->flags & PLLU) ? PLLU_BASE_POST_DIV : PLL_BASE_DIVP_MASK));
	val |= (sel->m << PLL_BASE_DIVM_SHIFT) |
		(sel->n << PLL_BASE_DIVN_SHIFT) | p_div;
	if (val == old_base)
		return 0;

	if (c->state == ON) {
		tegra21_pll_clk_disable(c);
		val &= ~(PLL_BASE_BYPASS | PLL_BASE_ENABLE);
	}
	clk_writel(val, c->reg + PLL_BASE);

	if (c->flags & PLL_HAS_CPCON) {
		val = clk_readl(c->reg + PLL_MISC(c));
		val &= ~PLL_MISC_CPCON_MASK;
		val |= sel->cpcon << PLL_MISC_CPCON_SHIFT;
		if (c->flags & (PLLU | PLLD)) {
			val &= ~PLL_MISC_LFCON_MASK;
			val |= PLLDU_LFCON << PLL_MISC_LFCON_SHIFT;
		}
		clk_writel(val, c->reg + PLL_MISC(c));
	}

	if (c->state == ON)
		tegra21_pll_clk_enable(c);

	return 0;
}

static void tegra21_pllp_clk_init(struct clk *c)
{
	tegra21_pll_clk_init(c);
	tegra21_pllp_init_dependencies(c->u.pll.fixed_rate);
}

#ifdef CONFIG_PM_SLEEP
static void tegra21_pllp_clk_resume(struct clk *c)
{
	unsigned long rate = c->u.pll.fixed_rate;
	tegra21_pll_clk_init(c);
	BUG_ON(rate != c->u.pll.fixed_rate);
}
#endif

static struct clk_ops tegra_pllp_ops = {
	.init			= tegra21_pllp_clk_init,
	.enable			= tegra21_pll_clk_enable,
	.disable		= tegra21_pll_clk_disable,
	.set_rate		= tegra21_pll_clk_set_rate,
};

/*
 * Dynamic ramp PLLs:
 *  PLLC2 and PLLC3 (PLLCX)
 *  PLLX and PLLC (PLLXC)
 *
 * When scaling PLLC and PLLX, dynamic ramp is allowed for any transition that
 * changes NDIV only. As a matter of policy we will make sure that switching
 * between output rates above VCO minimum is always dynamic. The pre-requisite
 * for the above guarantee is the following configuration convention:
 * - pll configured with fixed MDIV
 * - when output rate is above VCO minimum PDIV = 0 (p-value = 1)
 * Switching between output rates below VCO minimum may or may not be dynamic,
 * and switching across VCO minimum is never dynamic.
 *
 * PLLC2 and PLLC3 in addition to dynamic ramp mechanism have also glitchless
 * output dividers. However dynamic ramp without overshoot is guaranteed only
 * when output divisor is less or equal 8.
 *
 * Of course, dynamic ramp is applied provided PLL is already enabled.
 */

static u8 pll_qlin_pdiv_to_p[PLL_QLIN_PDIV_MAX + 1] = {
/* PDIV: 0, 1, 2, 3, 4, 5, 6, 7,  8,  9, 10, 11, 12, 13, 14, 15, 16 */
/* p: */ 1, 2, 3, 4, 5, 6, 8, 9, 10, 12, 15, 16, 18, 20, 24, 30, 32 };

static u32 pll_qlin_p_to_pdiv(u32 p, u32 *pdiv)
{
	int i;

	if (p) {
		for (i = 0; i <= PLL_QLIN_PDIV_MAX; i++) {
			if (p <= pll_qlin_pdiv_to_p[i]) {
				if (pdiv)
					*pdiv = i;
				return pll_qlin_pdiv_to_p[i];
			}
		}
	}
	return -EINVAL;
}

static u8 pll_expo_pdiv_to_p[PLL_QLIN_PDIV_MAX + 1] = {
/* PDIV: 0, 1, 2, 3,  4,  5,  6,   7 */
/* p: */ 1, 2, 4, 8, 16, 32, 64, 128 };

static u32 pll_expo_p_to_pdiv(u32 p, u32 *pdiv)
{
	if (p) {
		u32 i = fls(p);
		if (i == ffs(p))
			i--;

		if (i <= PLL_EXPO_PDIV_MAX) {
			if (pdiv)
				*pdiv = i;
			return 1 << i;
		}
	}
	return -EINVAL;
}

static u32 pll_base_set_div(struct clk *c, u32 m, u32 n, u32 pdiv, u32 val)
{
	struct clk_pll_div_layout *divs = c->u.pll.div_layout;

	val &= ~(divs->mdiv_mask | divs->ndiv_mask);
	val |= m << divs->mdiv_shift | n << divs->ndiv_shift;
	if (!c->u.pll.vco_out) {
		val &= ~divs->pdiv_mask;
		val |= pdiv << divs->pdiv_shift;
	}
	pll_writel_delay(val, c->reg);
	return val;
}

static void pll_sdm_set_din(struct clk *c, struct clk_pll_freq_table *cfg)
{
	u32 reg, val;
	struct clk_pll_controls *ctrl = c->u.pll.controls;
	struct clk_pll_div_layout *divs = c->u.pll.div_layout;

	if (!ctrl->sdm_en_mask)
		return;

	if (cfg->sdm_din) {
		reg = pll_reg_idx_to_addr(c, divs->sdm_din_reg_idx);
		val = clk_readl(reg) & (~divs->sdm_din_mask);
		val |= cfg->sdm_din << divs->sdm_din_shift;
		pll_writel_delay(val, reg);
	}

	reg = pll_reg_idx_to_addr(c, ctrl->sdm_ctrl_reg_idx);
	val = clk_readl(reg);
	if (!cfg->sdm_din != !(val & ctrl->sdm_en_mask)) {
		val ^= ctrl->sdm_en_mask;
		pll_writel_delay(val, reg);
	}
}


static void pll_base_parse_cfg(struct clk *c, struct clk_pll_freq_table *cfg)
{
	struct clk_pll_controls *ctrl = c->u.pll.controls;
	struct clk_pll_div_layout *divs = c->u.pll.div_layout;
	u32 base = clk_readl(c->reg);

	cfg->m = (base & divs->mdiv_mask) >> divs->mdiv_shift;
	cfg->n = (base & divs->ndiv_mask) >> divs->ndiv_shift;
	cfg->p = (base & divs->pdiv_mask) >> divs->pdiv_shift;
	if (cfg->p > divs->pdiv_max) {
		WARN(1, "%s pdiv %u is above max %u\n",
		     c->name, cfg->p, divs->pdiv_max);
		cfg->p = divs->pdiv_max;
	}
	cfg->p = c->u.pll.vco_out ? 1 : divs->pdiv_to_p[cfg->p];

	cfg->sdm_din = 0;
	if (ctrl->sdm_en_mask) {
		u32 reg = pll_reg_idx_to_addr(c, ctrl->sdm_ctrl_reg_idx);
		if (ctrl->sdm_en_mask & clk_readl(reg)) {
			reg = pll_reg_idx_to_addr(c, divs->sdm_din_reg_idx);
			cfg->sdm_din = (clk_readl(reg) & divs->sdm_din_mask) >>
				divs->sdm_din_shift;
		}
	}
}

static void pll_clk_set_gain(struct clk *c, struct clk_pll_freq_table *cfg)
{
	c->mul = cfg->n;
	c->div = cfg->m * cfg->p;

	if (cfg->sdm_din) {
		c->mul = c->mul*PLL_SDM_COEFF + PLL_SDM_COEFF/2 + cfg->sdm_din;
		c->div *= PLL_SDM_COEFF;
	}
}

static void pll_clk_start_ss(struct clk *c)
{
	u32 reg, val;
	struct clk_pll_controls *ctrl = c->u.pll.controls;

	if (c->u.pll.defaults_set && ctrl->ssc_en_mask) {
		reg = pll_reg_idx_to_addr(c, ctrl->sdm_ctrl_reg_idx);
		val = clk_readl(reg) | ctrl->ssc_en_mask;
		pll_writel_delay(val, reg);

	}
}

static void pll_clk_stop_ss(struct clk *c)
{
	u32 reg, val;
	struct clk_pll_controls *ctrl = c->u.pll.controls;

	if (ctrl->ssc_en_mask) {
		reg = pll_reg_idx_to_addr(c, ctrl->sdm_ctrl_reg_idx);
		val = clk_readl(reg) & (~ctrl->ssc_en_mask);
		pll_writel_delay(val, reg);
	}
}

static void pll_clk_verify_fixed_rate(struct clk *c)
{
	unsigned long rate = clk_get_rate_locked(c);

	/*
	 * If boot rate is not equal to expected fixed rate, print
	 * warning, but accept boot rate as new fixed rate, assuming
	 * that warning will be fixed.
	 */
	if (rate != c->u.pll.fixed_rate) {
		WARN(1, "%s: boot rate %lu != fixed rate %lu\n",
		       c->name, rate, c->u.pll.fixed_rate);
		c->u.pll.fixed_rate = rate;
	}
}

/*
 * Common configuration for PLLs with fixed input divider policy:
 * - always set fixed M-value based on the reference rate
 * - always set P-value value 1:1 for output rates above VCO minimum, and
 *   choose minimum necessary P-value for output rates below VCO minimum
 * - calculate N-value based on selected M and P
 * - calculate SDM_DIN fractional part
 */
static int pll_dyn_ramp_cfg(struct clk *c, struct clk_pll_freq_table *cfg,
	unsigned long rate, unsigned long input_rate, u32 *pdiv)
{
	int p;
	unsigned long cf;

	if (!rate)
		return -EINVAL;

	if (!c->u.pll.vco_out) {
		p = DIV_ROUND_UP(c->u.pll.vco_min, rate);
		p = c->u.pll.round_p_to_pdiv(p, pdiv);
	} else {
		p = rate >= c->u.pll.vco_min ? 1 : -EINVAL;
	}
	if (IS_ERR_VALUE(p))
		return -EINVAL;

	cfg->m = PLL_FIXED_MDIV(c, input_rate);
	cfg->p = p;
	cfg->output_rate = rate * cfg->p;
	if (cfg->output_rate > c->u.pll.vco_max)
		cfg->output_rate = c->u.pll.vco_max;
	cf = input_rate / cfg->m;
	cfg->n = cfg->output_rate / cf;

	cfg->sdm_din = 0;
	if (c->u.pll.controls->sdm_en_mask) {
		unsigned long rem = cfg->output_rate - cf * cfg->n;
		if (rem) {
			u64 s = rem * PLL_SDM_COEFF;
			do_div(s, cf);
			cfg->sdm_din = s + PLL_SDM_COEFF / 2;
			cfg->n--;
		}
	}

	return 0;
}

static int pll_dyn_ramp_find_cfg(struct clk *c, struct clk_pll_freq_table *cfg,
	unsigned long rate, unsigned long input_rate, u32 *pdiv)
{
	const struct clk_pll_freq_table *sel;

	/* Check if the target rate is tabulated */
	for (sel = c->u.pll.freq_table; sel->input_rate != 0; sel++) {
		if (sel->input_rate == input_rate && sel->output_rate == rate) {
			if (!c->u.pll.vco_out) {
				int p = c->u.pll.round_p_to_pdiv(sel->p, pdiv);
				BUG_ON(IS_ERR_VALUE(p));
			} else {
				BUG_ON(sel->p != 1);
			}
			BUG_ON(sel->m != PLL_FIXED_MDIV(c, input_rate));
			BUG_ON(!c->u.pll.controls->sdm_en_mask &&
			       sel->sdm_din);
			*cfg = *sel;
			return 0;
		}
	}

	/* Configure out-of-table rate */
	if (pll_dyn_ramp_cfg(c, cfg, rate, input_rate, pdiv)) {
		pr_err("%s: Failed to set %s out-of-table rate %lu\n",
		       __func__, c->name, rate);
		return -EINVAL;
	}
	return 0;
}

static int tegra_pll_clk_set_rate(struct clk *c, unsigned long rate)
{
	u32 val, pdiv = 0;
	struct clk_pll_freq_table cfg = { };
	struct clk_pll_freq_table old_cfg = { };
	struct clk_pll_controls *ctrl = c->u.pll.controls;
	unsigned long input_rate = clk_get_rate(c->parent);

	pr_debug("%s: %s %lu\n", __func__, c->name, rate);

	if (c->flags & PLL_FIXED) {
		if (rate != c->u.pll.fixed_rate) {
			pr_err("%s: can not change fixed rate %lu to %lu\n",
			       c->name, c->u.pll.fixed_rate, rate);
			return -EINVAL;
		}
		return 0;
	}

	if (pll_dyn_ramp_find_cfg(c, &cfg, rate, input_rate, &pdiv))
		return -EINVAL;

	pll_base_parse_cfg(c, &old_cfg);

	if ((cfg.m == old_cfg.m) && (cfg.n == old_cfg.n) &&
	    (cfg.p == old_cfg.p) && (cfg.sdm_din == old_cfg.sdm_din) &&
	    c->u.pll.defaults_set)
		return 0;

	pll_clk_set_gain(c, &cfg);

	if (!c->u.pll.defaults_set && c->u.pll.set_defaults) {
		c->ops->disable(c);
		c->u.pll.set_defaults(c, input_rate);

		val = clk_readl(c->reg);
		val = pll_base_set_div(c, cfg.m, cfg.n, pdiv, val);
		pll_sdm_set_din(c, &cfg);

		if (c->state == ON)
			c->ops->enable(c);
		return 0;
	}

	if (pll_is_dyn_ramp(c, &old_cfg, &cfg)) {
		if (!c->u.pll.dyn_ramp(c, &cfg))
			return 0;
	}

	val = clk_readl(c->reg);
	if (c->state == ON) {
		pll_clk_stop_ss(c);
		val &= ~ctrl->enable_mask;
		pll_writel_delay(val, c->reg);
	}

	val = pll_base_set_div(c, cfg.m, cfg.n, pdiv, val);
	pll_sdm_set_din(c, &cfg);

	if (c->state == ON) {
		u32 reg = pll_reg_idx_to_addr(c, ctrl->lock_reg_idx);
		val |= ctrl->enable_mask;
		pll_writel_delay(val, c->reg);
		tegra21_pll_clk_wait_for_lock(c, reg, ctrl->lock_mask);
		pll_clk_start_ss(c);
	}

	return 0;
}

/* FIXME: to be removed */
static void pll_do_iddq(struct clk *c, u32 offs, u32 iddq_bit, bool set)
{
	u32 val = clk_readl(c->reg + offs);
	if (set)
		val |= iddq_bit;
	else
		val &= ~iddq_bit;
	pll_writel_delay(val, c->reg + offs);
	if (!set)
		udelay(2); /* increased out of IDDQ delay to 3us */
}

static int tegra_pll_clk_enable(struct clk *c)
{
	u32 val, reg;
	struct clk_pll_controls *ctrl = c->u.pll.controls;

	pr_debug("%s on clock %s\n", __func__, c->name);

	if (ctrl->iddq_mask) {
		reg = pll_reg_idx_to_addr(c, ctrl->iddq_reg_idx);
		val = clk_readl(reg);
		val &= ~ctrl->iddq_mask;
		pll_writel_delay(val, reg);
		udelay(4); /* increase out of IDDQ delay to 5us */
	}

	if (ctrl->reset_mask) {
		reg = pll_reg_idx_to_addr(c, ctrl->reset_reg_idx);
		val = clk_readl(reg);
		val &= ~ctrl->reset_mask;
		pll_writel_delay(val, reg);
	}

	val = clk_readl(c->reg);
	val |= ctrl->enable_mask;
	pll_writel_delay(val, c->reg);

	reg = pll_reg_idx_to_addr(c, ctrl->lock_reg_idx);
	tegra21_pll_clk_wait_for_lock(c, reg, ctrl->lock_mask);

	pll_clk_start_ss(c);

	if (val & ctrl->bypass_mask) {
		val &= ~ctrl->bypass_mask;
		pll_writel_delay(val, c->reg);
	}

	return 0;
}

static void tegra_pll_clk_disable(struct clk *c)
{
	u32 val, reg;
	struct clk_pll_controls *ctrl = c->u.pll.controls;

	pr_debug("%s on clock %s\n", __func__, c->name);

	pll_clk_stop_ss(c);

	val = clk_readl(c->reg);
	val &= ~ctrl->enable_mask;
	pll_writel_delay(val, c->reg);

	if (ctrl->reset_mask) {
		reg = pll_reg_idx_to_addr(c, ctrl->reset_reg_idx);
		val = clk_readl(reg);
		val |= ctrl->reset_mask;
		pll_writel_delay(val, reg);
	}

	if (ctrl->iddq_mask) {
		reg = pll_reg_idx_to_addr(c, ctrl->iddq_reg_idx);
		val = clk_readl(reg);
		val |= ctrl->iddq_mask;
		pll_writel_delay(val, reg);
	}
}

static void tegra_pll_clk_init(struct clk *c)
{
	u32 val;
	unsigned long vco_min;
	unsigned long input_rate = clk_get_rate(c->parent);
	unsigned long cf = input_rate / PLL_FIXED_MDIV(c, input_rate);

	struct clk_pll_freq_table cfg = { };
	struct clk_pll_controls *ctrl = c->u.pll.controls;
	struct clk_pll_div_layout *divs = c->u.pll.div_layout;
	BUG_ON(!ctrl || !divs);

	/*
	 * To avoid vco_min crossover by rounding:
	 * - clip vco_min to exact multiple of comparison frequency if PLL does
	 *   not support SDM fractional divider
	 * - limit increase in vco_min to SDM resolution if SDM is supported
	 */
	vco_min = DIV_ROUND_UP(c->u.pll.vco_min, cf) * cf;
	if (ctrl->sdm_en_mask) {
		c->u.pll.vco_min += DIV_ROUND_UP(cf, PLL_SDM_COEFF);
		vco_min = min(vco_min, c->u.pll.vco_min);
	}
	c->u.pll.vco_min = vco_min;

	if (!c->u.pll.vco_out)
		c->min_rate = DIV_ROUND_UP(c->u.pll.vco_min,
					   divs->pdiv_to_p[divs->pdiv_max]);
	else
		c->min_rate = vco_min;

	val = clk_readl(c->reg);
	c->state = (val & ctrl->enable_mask) ? ON : OFF;

	/*
	 * If PLL is enabled on boot, keep it as is, just check if boot-loader
	 * set correct PLL parameters - if not, parameters will be reset at the
	 * 1st opportunity of reconfiguring PLL.
	 */
	if (c->state == ON) {
		if (c->u.pll.set_defaults) /* check only since PLL is ON */
			c->u.pll.set_defaults(c, input_rate);
		pll_base_parse_cfg(c, &cfg);
		pll_clk_set_gain(c, &cfg);
		if (c->flags & PLL_FIXED)
			pll_clk_verify_fixed_rate(c);
		return;
	}

	/*
	 * Initialize PLL to default state: disabled, reference running;
	 * registers are loaded with default parameters; rate is preset
	 * (close) to 1/4 of minimum VCO rate for PLLs with internal only
	 * VCO, and to VCO minimum for PLLs with VCO exposed to the clock
	 * tree.
	 */
	if (c->u.pll.set_defaults)
		c->u.pll.set_defaults(c, input_rate);

	val = clk_readl(c->reg);
	val &= ~(PLL_BASE_BYPASS | PLL_BASE_ENABLE | PLL_BASE_REF_DISABLE);
	pll_writel_delay(val, c->reg);

	if (c->flags & PLL_FIXED) {
		c->flags &= ~PLL_FIXED;	/* temporarily to allow set rate once */
		c->ops->set_rate(c, c->u.pll.fixed_rate);
		c->flags |= PLL_FIXED;
		pll_clk_verify_fixed_rate(c);
	} else {
		vco_min = DIV_ROUND_UP(vco_min, cf) * cf;
		if (!c->u.pll.vco_out)
			c->ops->set_rate(c, vco_min / 4);
		else
			c->ops->set_rate(c, vco_min);
	}
}

#ifdef CONFIG_PM_SLEEP
static void tegra_pll_clk_resume_enable(struct clk *c)
{
	unsigned long rate = clk_get_rate_all_locked(c->parent);
	u32 val = clk_readl(c->reg);
	enum clk_state state = c->state;

	if (val & c->u.pll.controls->enable_mask)
		return;		/* already resumed */

	/* temporarily sync h/w and s/w states, final sync happens
	   in tegra_clk_resume later */
	c->state = OFF;
	if (c->u.pll.set_defaults)
		c->u.pll.set_defaults(c, rate);

	if (c->flags & PLL_FIXED) {
		c->flags &= ~PLL_FIXED;	/* temporarily to allow set rate once */
		c->ops->set_rate(c, c->u.pll.fixed_rate);
		c->flags |= PLL_FIXED;
	} else {
		rate = clk_get_rate_all_locked(c);
		c->ops->set_rate(c, rate);
	}
	c->ops->enable(c);
	c->state = state;
}
#endif

/*
 * PLLCX: PLLC, PLLC2, PLLC3, PLLA1
 * Hybrid PLLs with dynamic ramp. Dynamic ramp is allowed for any transition
 * that changes NDIV only, while PLL is already locked.
 */
static void pllcx_check_defaults(struct clk *c, unsigned long input_rate)
{
	u32 default_val;

	default_val = PLLCX_MISC0_DEFAULT_VALUE & (~PLLCX_MISC0_RESET);
	PLL_MISC_CHK_DEFAULT(c, 0, default_val, PLLCX_MISC0_WRITE_MASK);

	default_val = PLLCX_MISC1_DEFAULT_VALUE & (~PLLCX_MISC1_IDDQ);
	PLL_MISC_CHK_DEFAULT(c, 1, default_val, PLLCX_MISC1_WRITE_MASK);

	default_val = PLLCX_MISC2_DEFAULT_VALUE;
	PLL_MISC_CHK_DEFAULT(c, 2, default_val, PLLCX_MISC2_WRITE_MASK);

	default_val = PLLCX_MISC3_DEFAULT_VALUE;
	PLL_MISC_CHK_DEFAULT(c, 3, default_val, PLLCX_MISC3_WRITE_MASK);
}

static void pllcx_set_defaults(struct clk *c, unsigned long input_rate)
{
	c->u.pll.defaults_set = true;

	if (clk_readl(c->reg) & c->u.pll.controls->enable_mask) {
		/* PLL is ON: only check if defaults already set */
		pllcx_check_defaults(c, input_rate);
		return;
	}

	/* Defaults assert PLL reset, and set IDDQ */
	clk_writel(PLLCX_MISC0_DEFAULT_VALUE, c->reg + c->u.pll.misc0);
	clk_writel(PLLCX_MISC1_DEFAULT_VALUE, c->reg + c->u.pll.misc1);
	clk_writel(PLLCX_MISC2_DEFAULT_VALUE, c->reg + c->u.pll.misc2);
	pll_writel_delay(PLLCX_MISC3_DEFAULT_VALUE, c->reg + c->u.pll.misc3);
}

static int pllcx_dyn_ramp(struct clk *c, struct clk_pll_freq_table *cfg)
{
#if PLLCX_USE_DYN_RAMP
	u32 reg;
	struct clk_pll_controls *ctrl = c->u.pll.controls;
	struct clk_pll_div_layout *divs = c->u.pll.div_layout;

	u32 val = clk_readl(c->reg);
	val &= ~divs->ndiv_mask;
	val |= cfg->n << divs->ndiv_shift;
	pll_writel_delay(val, c->reg);

	reg = pll_reg_idx_to_addr(c, ctrl->lock_reg_idx);
	tegra21_pll_clk_wait_for_lock(c, reg, ctrl->lock_mask);

	return 0;
#else
	return -ENOSYS;
#endif
}

static void tegra21_pllcx_clk_init(struct clk *c)
{
	tegra_pll_clk_init(c);
}

static struct clk_ops tegra_pllcx_ops = {
	.init			= tegra21_pllcx_clk_init,
	.enable			= tegra_pll_clk_enable,
	.disable		= tegra_pll_clk_disable,
	.set_rate		= tegra_pll_clk_set_rate,
};

/*
 * PLLA
 * PLL with dynamic ramp and fractional SDM. Dynamic ramp is not used.
 * Fractional SDM is allowed to provide exact audio rates.
 */
static void plla_set_defaults(struct clk *c, unsigned long input_rate)
{
	u32 mask;
	u32 val = clk_readl(c->reg);
	c->u.pll.defaults_set = true;

	if (val & c->u.pll.controls->enable_mask) {
		/*
		 * PLL is ON: check if defaults already set, then set those
		 * that can be updated in flight.
		 */
		if (val & PLLA_BASE_IDDQ) {
			pr_warn("%s boot enabled with IDDQ set\n", c->name);
			c->u.pll.defaults_set = false;
		}

		val = PLLA_MISC0_DEFAULT_VALUE;	/* ignore lock enable */
		mask = PLLA_MISC0_LOCK_ENABLE | PLLA_MISC0_LOCK_OVERRIDE;
		PLL_MISC_CHK_DEFAULT(c, 0, val, ~mask & PLLA_MISC0_WRITE_MASK);

		val = PLLA_MISC2_DEFAULT_VALUE; /* ignore all but control bit */
		PLL_MISC_CHK_DEFAULT(c, 2, val, PLLA_MISC2_EN_DYNRAMP);

		/* Enable lock detect */
		val = clk_readl(c->reg + c->u.pll.misc0);
		val &= ~mask;
		val |= PLLA_MISC0_DEFAULT_VALUE & mask;
		pll_writel_delay(val, c->reg + c->u.pll.misc0);

		return;
	}

	/* set IDDQ, enable lock detect, disable dynamic ramp and SDM */
	val |= PLLA_BASE_IDDQ;
	clk_writel(val, c->reg);
	clk_writel(PLLA_MISC0_DEFAULT_VALUE, c->reg + c->u.pll.misc0);
	pll_writel_delay(PLLA_MISC2_DEFAULT_VALUE, c->reg + c->u.pll.misc2);
}

static void tegra21_plla_clk_init(struct clk *c)
{
	tegra_pll_clk_init(c);
}

static struct clk_ops tegra_plla_ops = {
	.init			= tegra21_plla_clk_init,
	.enable			= tegra_pll_clk_enable,
	.disable		= tegra_pll_clk_disable,
	.set_rate		= tegra_pll_clk_set_rate,
};

/*
 * PLLD
 * PLL with fractional SDM.
 */
static void plld_set_defaults(struct clk *c, unsigned long input_rate)
{
	u32 val = clk_readl(c->reg);
	u32 mask = c->u.pll.div_layout->sdm_din_mask;

	c->u.pll.defaults_set = true;

	if (val & c->u.pll.controls->enable_mask) {
		/*
		 * PLL is ON: check if defaults already set, then set those
		 * that can be updated in flight.
		 */
		val = PLLD_MISC1_DEFAULT_VALUE;
		PLL_MISC_CHK_DEFAULT(c, 1, val, PLLD_MISC1_WRITE_MASK);

		/* ignore lock, DSI and SDM controls, make sure IDDQ not set */
		val = PLLD_MISC0_DEFAULT_VALUE & (~PLLD_MISC0_IDDQ);
		mask |= PLLD_MISC0_DSI_CLKENABLE | PLLD_MISC0_LOCK_ENABLE |
			PLLD_MISC0_LOCK_OVERRIDE | PLLD_MISC0_EN_SDM;
		PLL_MISC_CHK_DEFAULT(c, 0, val, ~mask & PLLD_MISC0_WRITE_MASK);

		/* Enable lock detect */
		mask = PLLD_MISC0_LOCK_ENABLE | PLLD_MISC0_LOCK_OVERRIDE;
		val = clk_readl(c->reg + c->u.pll.misc0);
		val &= ~mask;
		val |= PLLD_MISC0_DEFAULT_VALUE & mask;
		pll_writel_delay(val, c->reg + c->u.pll.misc0);

		return;
	}

	/* set IDDQ, enable lock detect, disable SDM */
	clk_writel(PLLD_MISC0_DEFAULT_VALUE, c->reg + c->u.pll.misc0);
	pll_writel_delay(PLLD_MISC1_DEFAULT_VALUE, c->reg + c->u.pll.misc1);
}

static void tegra21_plld_clk_init(struct clk *c)
{
	tegra_pll_clk_init(c);
}

static int
tegra21_plld_clk_cfg_ex(struct clk *c, enum tegra_clk_ex_param p, u32 setting)
{
	u32 val, mask, reg;
	u32 clear = 0;

	switch (p) {
	case TEGRA_CLK_PLLD_CSI_OUT_ENB:
		mask = PLLD_BASE_CSI_CLKSOURCE;
		reg = c->reg + PLL_BASE;
		break;
	case TEGRA_CLK_MIPI_CSI_OUT_ENB:
		mask = 0;
		clear = PLLD_BASE_CSI_CLKSOURCE;
		reg = c->reg + PLL_BASE;
		break;
	case TEGRA_CLK_PLLD_DSI_OUT_ENB:
		mask = PLLD_MISC0_DSI_CLKENABLE;
		reg = c->reg + PLL_MISC(c);
		break;
	case TEGRA_CLK_PLLD_MIPI_MUX_SEL:
		mask = PLLD_BASE_DSI_MUX_MASK;
		reg = c->reg + PLL_BASE;
		break;
	default:
		return -EINVAL;
	}

	val = clk_readl(reg);
	if (setting) {
		val |= mask;
		val &= ~clear;
	} else
		val &= ~mask;
	clk_writel(val, reg);
	return 0;
}

static struct clk_ops tegra_plld_ops = {
	.init			= tegra21_plld_clk_init,
	.enable			= tegra_pll_clk_enable,
	.disable		= tegra_pll_clk_disable,
	.set_rate		= tegra_pll_clk_set_rate,
	.clk_cfg_ex		= tegra21_plld_clk_cfg_ex,
};

/*
 * PLLD2, PLLDP
 * PLL with fractional SDM and Spread Spectrum (mutually exclusive).
 */
static void plldss_defaults(struct clk *c, u32 misc0_val, u32 misc1_val,
			    u32 misc2_val, u32 misc3_val)
{
	u32 default_val;
	u32 val = clk_readl(c->reg);
	c->u.pll.defaults_set = true;

	if (val & c->u.pll.controls->enable_mask) {
		/*
		 * PLL is ON: check if defaults already set, then set those
		 * that can be updated in flight.
		 */
		if (val & PLLDSS_BASE_IDDQ) {
			pr_warn("%s boot enabled with IDDQ set\n", c->name);
			c->u.pll.defaults_set = false;
		}

		default_val = misc0_val;
		PLL_MISC_CHK_DEFAULT(c, 0, default_val,	/* ignore lock enable */
				     PLLDSS_MISC0_WRITE_MASK &
				     (~PLLDSS_MISC0_LOCK_ENABLE));

		/*
		 * If SSC is used, check all settings, otherwise just confirm
		 * that SSC is not used on boot as well. Do nothing when using
		 * this function for PLLC4 that has only MISC0.
		 */
		if (c->u.pll.controls->ssc_en_mask) {
			default_val = misc1_val;
			PLL_MISC_CHK_DEFAULT(c, 1, default_val,
					     PLLDSS_MISC1_CFG_WRITE_MASK);
			default_val = misc2_val;
			PLL_MISC_CHK_DEFAULT(c, 2, default_val,
					     PLLDSS_MISC2_CTRL1_WRITE_MASK);
			default_val = misc3_val;
			PLL_MISC_CHK_DEFAULT(c, 3, default_val,
					     PLLDSS_MISC3_CTRL2_WRITE_MASK);
		} else if (c->u.pll.misc1) {
			default_val = misc1_val;
			PLL_MISC_CHK_DEFAULT(c, 1, default_val,
					     PLLDSS_MISC1_CFG_WRITE_MASK &
					     (~PLLDSS_MISC1_CFG_EN_SDM));
		}

		/* Enable lock detect */
		if (val & PLLDSS_BASE_LOCK_OVERRIDE) {
			val &= ~PLLDSS_BASE_LOCK_OVERRIDE;
			clk_writel(val, c->reg);
		}

		val = clk_readl(c->reg + c->u.pll.misc0);
		val &= ~PLLDSS_MISC0_LOCK_ENABLE;
		val |= misc0_val & PLLDSS_MISC0_LOCK_ENABLE;
		pll_writel_delay(val, c->reg + c->u.pll.misc0);

		return;
	}

	/* set IDDQ, enable lock detect, configure SDM/SSC  */
	val |= PLLDSS_BASE_IDDQ;
	val &= ~PLLDSS_BASE_LOCK_OVERRIDE;
	clk_writel(val, c->reg);

	/* When using this function for PLLC4 exit here */
	if (!c->u.pll.misc1) {
		pll_writel_delay(misc0_val, c->reg + c->u.pll.misc0);
		return;
	}

	clk_writel(misc0_val, c->reg + c->u.pll.misc0);
	clk_writel(misc1_val & (~PLLDSS_MISC1_CFG_EN_SSC),
		   c->reg + c->u.pll.misc1); /* if SSC used set by 1st enable */
	clk_writel(misc2_val, c->reg + c->u.pll.misc2);
	pll_writel_delay(misc3_val, c->reg + c->u.pll.misc3);
}

static void plldss_select_ref(struct clk *c)
{
	u32 val = clk_readl(c->reg);
	u32 ref = (val & PLLDSS_BASE_REF_SEL_MASK) >> PLLDSS_BASE_REF_SEL_SHIFT;

	/*
	 * The only productized reference clock is tegra_pll_ref. Made sure
	 * it is selected on boot. If pll is enabled, shut it down before
	 * changing reference selection.
	 */
	if (ref) {
		if (val & c->u.pll.controls->enable_mask) {
			WARN(1, "%s boot enabled with not supported ref %u\n",
			     c->name, ref);
			val &= ~c->u.pll.controls->enable_mask;
			pll_writel_delay(val, c->reg);
		}
		val &= ~PLLDSS_BASE_REF_SEL_MASK;
		pll_writel_delay(val, c->reg);
	}
}

static void plld2_set_defaults(struct clk *c, unsigned long input_rate)
{
	plldss_defaults(c, PLLD2_MISC0_DEFAULT_VALUE,
			PLLD2_MISC1_CFG_DEFAULT_VALUE,
			PLLD2_MISC2_CTRL1_DEFAULT_VALUE,
			PLLD2_MISC3_CTRL2_DEFAULT_VALUE);
}

static void tegra21_plld2_clk_init(struct clk *c)
{
	if (PLLD2_MISC1_CFG_DEFAULT_VALUE & PLLDSS_MISC1_CFG_EN_SSC) {
		/* SSC requires SDM enabled, but prevent fractional div usage */
		BUILD_BUG_ON(!(PLLD2_MISC1_CFG_DEFAULT_VALUE &
			       PLLDSS_MISC1_CFG_EN_SDM));
		c->u.pll.controls->sdm_en_mask = 0;
	} else {
		/* SSC should be disabled */
		c->u.pll.controls->ssc_en_mask = 0;
	}
	plldss_select_ref(c);

	tegra_pll_clk_init(c);
}

static struct clk_ops tegra_plld2_ops = {
	.init			= tegra21_plld2_clk_init,
	.enable			= tegra_pll_clk_enable,
	.disable		= tegra_pll_clk_disable,
	.set_rate		= tegra_pll_clk_set_rate,
};

static void plldp_set_defaults(struct clk *c, unsigned long input_rate)
{
	plldss_defaults(c, PLLDP_MISC0_DEFAULT_VALUE,
			PLLDP_MISC1_CFG_DEFAULT_VALUE,
			PLLDP_MISC2_CTRL1_DEFAULT_VALUE,
			PLLDP_MISC3_CTRL2_DEFAULT_VALUE);
}

static void tegra21_plldp_clk_init(struct clk *c)
{

	if (PLLDP_MISC1_CFG_DEFAULT_VALUE & PLLDSS_MISC1_CFG_EN_SSC) {
		/* SSC requires SDM enabled, but prevent fractional div usage */
		BUILD_BUG_ON(!(PLLDP_MISC1_CFG_DEFAULT_VALUE &
			       PLLDSS_MISC1_CFG_EN_SDM));
		c->u.pll.controls->sdm_en_mask = 0;
	} else {
		/* SSC should be disabled */
		c->u.pll.controls->ssc_en_mask = 0;
	}
	plldss_select_ref(c);

	tegra_pll_clk_init(c);
}

static struct clk_ops tegra_plldp_ops = {
	.init			= tegra21_plldp_clk_init,
	.enable			= tegra_pll_clk_enable,
	.disable		= tegra_pll_clk_disable,
	.set_rate		= tegra_pll_clk_set_rate,
};

/*
 * PLLC4
 * Base and misc0 layout is the same as PLLD2/PLLDP, but no SDM/SSC support.
 * VCO is exposed to the clock tree via fixed 1/3 and 1/5 dividers.
 */
static void pllc4_set_defaults(struct clk *c, unsigned long input_rate)
{
	plldss_defaults(c, PLLC4_MISC0_DEFAULT_VALUE, 0, 0, 0);
}

static void tegra21_pllc4_vco_init(struct clk *c)
{
	plldss_select_ref(c);
	tegra_pll_clk_init(c);
}

static struct clk_ops tegra_pllc4_vco_ops = {
	.init			= tegra21_pllc4_vco_init,
	.enable			= tegra_pll_clk_enable,
	.disable		= tegra_pll_clk_disable,
	.set_rate		= tegra_pll_clk_set_rate,
};

/*
 * PLLRE
 * VCO is exposed to the clock tree directly along with post-divider output
 */
static void pllre_set_defaults(struct clk *c, unsigned long input_rate)
{
	u32 mask;
	u32 val = clk_readl(c->reg);
	c->u.pll.defaults_set = true;

	if (val & c->u.pll.controls->enable_mask) {
		/*
		 * PLL is ON: check if defaults already set, then set those
		 * that can be updated in flight.
		 */
		val &= PLLRE_BASE_DEFAULT_MASK;
		if (val != PLLRE_BASE_DEFAULT_VALUE) {
			pr_warn("%s boot base 0x%x : expected 0x%x\n",
				(c)->name, val, PLLRE_BASE_DEFAULT_VALUE);
			pr_warn("(comparison mask = 0x%x)\n",
				PLLRE_BASE_DEFAULT_MASK);
			c->u.pll.defaults_set = false;
		}

		/* Ignore lock enable */
		val = PLLRE_MISC0_DEFAULT_VALUE & (~PLLRE_MISC0_IDDQ);
		mask = PLLRE_MISC0_LOCK_ENABLE | PLLRE_MISC0_LOCK_OVERRIDE;
		PLL_MISC_CHK_DEFAULT(c, 0, val, ~mask & PLLRE_MISC0_WRITE_MASK);

		/* Enable lock detect */
		val = clk_readl(c->reg + c->u.pll.misc0);
		val &= ~mask;
		val |= PLLRE_MISC0_DEFAULT_VALUE & mask;
		pll_writel_delay(val, c->reg + c->u.pll.misc0);

		return;
	}

	/* set IDDQ, enable lock detect */
	val &= ~PLLRE_BASE_DEFAULT_MASK;
	val |= PLLRE_BASE_DEFAULT_VALUE & PLLRE_BASE_DEFAULT_MASK;
	clk_writel(val, c->reg);
	pll_writel_delay(PLLRE_MISC0_DEFAULT_VALUE, c->reg + c->u.pll.misc0);
}

static void tegra21_pllre_vco_init(struct clk *c)
{
	tegra_pll_clk_init(c);
}

static struct clk_ops tegra_pllre_vco_ops = {
	.init			= tegra21_pllre_vco_init,
	.enable			= tegra_pll_clk_enable,
	.disable		= tegra_pll_clk_disable,
	.set_rate		= tegra_pll_clk_set_rate,
};

/*
 * PLLU
 * VCO is exposed to the clock tree directly along with post-divider output.
 * Both VCO and post-divider output rates are fixed at 480MHz and 240MHz,
 * respectively.
 */
static void pllu_check_defaults(struct clk *c, bool hw_control)
{
	u32 val, mask;

	/* Ignore lock enable (will be set) and IDDQ if under h/w control */
	val = PLLU_MISC0_DEFAULT_VALUE & (~PLLU_MISC0_IDDQ);
	mask = PLLU_MISC0_LOCK_ENABLE | (hw_control ? PLLU_MISC0_IDDQ : 0);
	PLL_MISC_CHK_DEFAULT(c, 0, val, ~mask & PLLU_MISC0_WRITE_MASK);

	val = PLLU_MISC1_DEFAULT_VALUE;
	mask = PLLU_MISC1_LOCK_OVERRIDE;
	PLL_MISC_CHK_DEFAULT(c, 1, val, ~mask & PLLU_MISC1_WRITE_MASK);
}

static void pllu_set_defaults(struct clk *c, unsigned long input_rate)
{
	u32 val = clk_readl(c->reg);
	c->u.pll.defaults_set = true;

	if (val & c->u.pll.controls->enable_mask) {
		/*
		 * PLL is ON: check if defaults already set, then set those
		 * that can be updated in flight.
		 */
		pllu_check_defaults(c, false);

		/* Enable lock detect */
		val = clk_readl(c->reg + c->u.pll.misc0);
		val &= ~PLLU_MISC0_LOCK_ENABLE;
		val |= PLLU_MISC0_DEFAULT_VALUE & PLLU_MISC0_LOCK_ENABLE;
		pll_writel_delay(val, c->reg + c->u.pll.misc0);

		val = clk_readl(c->reg + c->u.pll.misc1);
		val &= ~PLLU_MISC1_LOCK_OVERRIDE;
		val |= PLLU_MISC1_DEFAULT_VALUE & PLLU_MISC1_LOCK_OVERRIDE;
		pll_writel_delay(val, c->reg + c->u.pll.misc1);

		return;
	}

	/* set IDDQ, enable lock detect */
	clk_writel(PLLU_MISC0_DEFAULT_VALUE, c->reg + c->u.pll.misc0);
	pll_writel_delay(PLLU_MISC1_DEFAULT_VALUE, c->reg + c->u.pll.misc1);
}

static void tegra21_pllu_vco_init(struct clk *c)
{
	u32 val = clk_readl(c->reg);
	pll_u = c;

	/*
	 * If PLLU state is already under h/w control just check defaults, and
	 * verify expected fixed VCO rate (pll dividers can still be read from
	 * the * base register).
	 */
	if (!(val & PLLU_BASE_OVERRIDE)) {
		struct clk_pll_freq_table cfg = { };
		c->state = (val & c->u.pll.controls->enable_mask) ? ON : OFF;

		pllu_check_defaults(c, true);
		pll_base_parse_cfg(c, &cfg);
		pll_clk_set_gain(c, &cfg);

		pll_clk_verify_fixed_rate(c);
		return;
	}

	/* S/w controlled initialization */
	tegra_pll_clk_init(c);
}

static struct clk_ops tegra_pllu_vco_ops = {
	.init			= tegra21_pllu_vco_init,
	.enable			= tegra_pll_clk_enable,
	.disable		= tegra_pll_clk_disable,
	.set_rate		= tegra_pll_clk_set_rate,
};

/*
 * PLLX
 * PLL with dynamic ramp and fractional SDM. Dynamic ramp is allowed for any
 * transition that changes NDIV only, while PLL is already locked. SDM is not
 * used and always disabled.
 */
static void pllx_get_dyn_steps(struct clk *c, unsigned long input_rate,
				u32 *step_a, u32 *step_b)
{
	input_rate /= PLL_FIXED_MDIV(c, input_rate); /* cf rate */
	switch (input_rate) {
	case 12000000:
	case 12800000:
	case 13000000:
		*step_a = 0x2B;
		*step_b = 0x0B;
		return;
	case 19200000:
		*step_a = 0x12;
		*step_b = 0x08;
		return;
	case 38400000:
		*step_a = 0x04;
		*step_b = 0x05;
		return;
	default:
		pr_err("%s: Unexpected reference rate %lu\n",
			__func__, input_rate);
		BUG();
	}
}

static void pllx_check_defaults(struct clk *c, unsigned long input_rate)
{
	u32 default_val;

	default_val = PLLX_MISC0_DEFAULT_VALUE;
	PLL_MISC_CHK_DEFAULT(c, 0, default_val,	/* ignore lock enable */
			     PLLX_MISC0_WRITE_MASK & (~PLLX_MISC0_LOCK_ENABLE));

	default_val = PLLX_MISC1_DEFAULT_VALUE;
	PLL_MISC_CHK_DEFAULT(c, 1, default_val, PLLX_MISC1_WRITE_MASK);

	default_val = PLLX_MISC2_DEFAULT_VALUE; /* ignore all but control bit */
	PLL_MISC_CHK_DEFAULT(c, 2, default_val, PLLX_MISC2_EN_DYNRAMP);

	default_val = PLLX_MISC3_DEFAULT_VALUE & (~PLLX_MISC3_IDDQ);
	PLL_MISC_CHK_DEFAULT(c, 3, default_val, PLLX_MISC3_WRITE_MASK);

	default_val = PLLX_MISC4_DEFAULT_VALUE;
	PLL_MISC_CHK_DEFAULT(c, 4, default_val, PLLX_MISC4_WRITE_MASK);

	default_val = PLLX_MISC5_DEFAULT_VALUE;
	PLL_MISC_CHK_DEFAULT(c, 5, default_val, PLLX_MISC5_WRITE_MASK);
}

static void pllx_set_defaults(struct clk *c, unsigned long input_rate)
{
	u32 val;
	u32 step_a, step_b;

	c->u.pll.defaults_set = true;

	/* Get ready dyn ramp state machine settings */
	pllx_get_dyn_steps(c, input_rate, &step_a, &step_b);
	val = PLLX_MISC2_DEFAULT_VALUE & (~PLLX_MISC2_DYNRAMP_STEPA_MASK) &
		(~PLLX_MISC2_DYNRAMP_STEPB_MASK);
	val |= step_a << PLLX_MISC2_DYNRAMP_STEPA_SHIFT;
	val |= step_b << PLLX_MISC2_DYNRAMP_STEPB_SHIFT;

	if (clk_readl(c->reg) & c->u.pll.controls->enable_mask) {
		/*
		 * PLL is ON: check if defaults already set, then set those
		 * that can be updated in flight.
		 */
		pllx_check_defaults(c, input_rate);

		/* Configure dyn ramp, disable lock override */
		clk_writel(val, c->reg + c->u.pll.misc2);

		/* Enable lock detect */
		val = clk_readl(c->reg + c->u.pll.misc0);
		val &= ~PLLX_MISC0_LOCK_ENABLE;
		val |= PLLX_MISC0_DEFAULT_VALUE & PLLX_MISC0_LOCK_ENABLE;
		pll_writel_delay(val, c->reg + c->u.pll.misc0);

		return;
	}

	/* Enable lock detect and CPU output */
	clk_writel(PLLX_MISC0_DEFAULT_VALUE, c->reg + c->u.pll.misc0);

	/* Setup */
	clk_writel(PLLX_MISC1_DEFAULT_VALUE, c->reg + c->u.pll.misc1);

	/* Configure dyn ramp state machine, disable lock override */
	clk_writel(val, c->reg + c->u.pll.misc2);

	/* Set IDDQ */
	clk_writel(PLLX_MISC3_DEFAULT_VALUE, c->reg + c->u.pll.misc3);

	/* Disable SDM */
	clk_writel(PLLX_MISC4_DEFAULT_VALUE, c->reg + c->u.pll.misc4);
	pll_writel_delay(PLLX_MISC5_DEFAULT_VALUE, c->reg + c->u.pll.misc5);
}

static int pllx_dyn_ramp(struct clk *c, struct clk_pll_freq_table *cfg)
{
#if PLLX_USE_DYN_RAMP
	u32 reg, val, base, ndiv_new_mask;
	struct clk_pll_controls *ctrl = c->u.pll.controls;
	struct clk_pll_div_layout *divs = c->u.pll.div_layout;

	reg = pll_reg_idx_to_addr(c, divs->ndiv_new_reg_idx);
	ndiv_new_mask = (divs->ndiv_mask >> divs->ndiv_shift) <<
		divs->ndiv_new_shift;
	val = clk_readl(reg) & (~ndiv_new_mask);
	val |= cfg->n << divs->ndiv_new_shift;
	pll_writel_delay(val, reg);

	reg = pll_reg_idx_to_addr(c, ctrl->dramp_ctrl_reg_idx);
	val = clk_readl(reg);
	val |= ctrl->dramp_en_mask;
	pll_writel_delay(val, reg);
	tegra21_pll_clk_wait_for_lock(c, reg, ctrl->dramp_done_mask);

	base = clk_readl(c->reg) & (~divs->ndiv_mask);
	base |= cfg->n << divs->ndiv_shift;
	pll_writel_delay(base, c->reg);

	val &= ~ctrl->dramp_en_mask;
	pll_writel_delay(val, reg);

	return 0;
#else
	return -ENOSYS;
#endif
}

static void tegra21_pllx_clk_init(struct clk *c)
{
	/* Only s/w dyn ramp control is supported */
	u32 val = clk_readl(PLLX_HW_CTRL_CFG);
	BUG_ON(!(val & PLLX_HW_CTRL_CFG_SWCTRL) && !tegra_platform_is_linsim());

	tegra_pll_clk_init(c);
}

static struct clk_ops tegra_pllx_ops = {
	.init			= tegra21_pllx_clk_init,
	.enable			= tegra_pll_clk_enable,
	.disable		= tegra_pll_clk_disable,
	.set_rate		= tegra_pll_clk_set_rate,
};

/* FIXME: pllm suspend/resume */

/* non-monotonic mapping below is not a typo */
static u8 pllm_p[PLLM_PDIV_MAX + 1] = {
/* PDIV: 0, 1, 2, 3, 4, 5, 6,  7,  8,  9, 10, 11, 12, 13, 14 */
/* p: */ 1, 2, 3, 4, 5, 6, 8, 10, 12, 16, 12, 16, 20, 24, 32 };

static u32 pllm_round_p_to_pdiv(u32 p, u32 *pdiv)
{
	if (!p || (p > PLLM_SW_PDIV_MAX + 1))
		return -EINVAL;

	if (pdiv)
		*pdiv = p - 1;
	return p;
}

static void pllm_set_defaults(struct clk *c, unsigned long input_rate)
{
	u32 val = clk_readl(c->reg + PLL_MISC(c));

	val &= ~PLLM_MISC_LOCK_OVERRIDE;
#if USE_PLL_LOCK_BITS
	val &= ~PLLM_MISC_LOCK_DISABLE;
#else
	val |= PLLM_MISC_LOCK_DISABLE;
#endif

	if (c->state != ON)
		val |= PLLM_MISC_IDDQ;
	else
		BUG_ON(val & PLLM_MISC_IDDQ && !tegra_platform_is_linsim());

	clk_writel(val, c->reg + PLL_MISC(c));
}

static void tegra21_pllm_clk_init(struct clk *c)
{
	unsigned long input_rate = clk_get_rate(c->parent);
	u32 m, p, val;

	/* clip vco_min to exact multiple of input rate to avoid crossover
	   by rounding */
	c->u.pll.vco_min =
		DIV_ROUND_UP(c->u.pll.vco_min, input_rate) * input_rate;
	c->min_rate =
		DIV_ROUND_UP(c->u.pll.vco_min, pllm_p[PLLM_SW_PDIV_MAX]);

	val = pmc_readl(PMC_PLLP_WB0_OVERRIDE);
	if (val & PMC_PLLP_WB0_OVERRIDE_PLLM_OVERRIDE) {
		c->state = (val & PMC_PLLP_WB0_OVERRIDE_PLLM_ENABLE) ? ON : OFF;

		/* Tegra12 has bad default value of PMC_PLLM_WB0_OVERRIDE.
		 * If bootloader does not initialize PLLM, kernel has to
		 * initialize the register with sane value. */
		if (c->state == OFF) {
			val = pmc_readl(PMC_PLLM_WB0_OVERRIDE);
			m = (val & PLLM_BASE_DIVM_MASK) >> PLL_BASE_DIVM_SHIFT;
			if (m != PLL_FIXED_MDIV(c, input_rate)) {
				/* Copy DIVM and DIVN from PLLM_BASE */
				pr_info("%s: Fixing DIVM and DIVN\n", __func__);
				val = clk_readl(c->reg + PLL_BASE);
				val &= (PLLM_BASE_DIVM_MASK
					| PLLM_BASE_DIVN_MASK);
				pmc_writel(val, PMC_PLLM_WB0_OVERRIDE);
			}
		}

		val = pmc_readl(PMC_PLLM_WB0_OVERRIDE_2);
		p = (val & PMC_PLLM_WB0_OVERRIDE_2_DIVP_MASK) >>
			PMC_PLLM_WB0_OVERRIDE_2_DIVP_SHIFT;

		val = pmc_readl(PMC_PLLM_WB0_OVERRIDE);
	} else {
		val = clk_readl(c->reg + PLL_BASE);
		c->state = (val & PLL_BASE_ENABLE) ? ON : OFF;
		p = (val & PLLM_BASE_DIVP_MASK) >> PLL_BASE_DIVP_SHIFT;
	}

	m = (val & PLLM_BASE_DIVM_MASK) >> PLL_BASE_DIVM_SHIFT;
	BUG_ON(m != PLL_FIXED_MDIV(c, input_rate)
			 && tegra_platform_is_silicon());
	c->div = m * pllm_p[p];
	c->mul = (val & PLLM_BASE_DIVN_MASK) >> PLL_BASE_DIVN_SHIFT;

	pllm_set_defaults(c, input_rate);
}

static int tegra21_pllm_clk_enable(struct clk *c)
{
	u32 val;
	pr_debug("%s on clock %s\n", __func__, c->name);

	pll_do_iddq(c, PLL_MISC(c), PLLM_MISC_IDDQ, false);

	/* Just enable both base and override - one would work */
	val = clk_readl(c->reg + PLL_BASE);
	val |= PLL_BASE_ENABLE;
	clk_writel(val, c->reg + PLL_BASE);

	val = pmc_readl(PMC_PLLP_WB0_OVERRIDE);
	val |= PMC_PLLP_WB0_OVERRIDE_PLLM_ENABLE;
	pmc_writel(val, PMC_PLLP_WB0_OVERRIDE);
	val = pmc_readl(PMC_PLLP_WB0_OVERRIDE);

	tegra21_pll_clk_wait_for_lock(c, c->reg + PLL_BASE, PLL_BASE_LOCK);
	return 0;
}

static void tegra21_pllm_clk_disable(struct clk *c)
{
	u32 val;
	pr_debug("%s on clock %s\n", __func__, c->name);

	/* Just disable both base and override - one would work */
	val = pmc_readl(PMC_PLLP_WB0_OVERRIDE);
	val &= ~PMC_PLLP_WB0_OVERRIDE_PLLM_ENABLE;
	pmc_writel(val, PMC_PLLP_WB0_OVERRIDE);
	val = pmc_readl(PMC_PLLP_WB0_OVERRIDE);

	val = clk_readl(c->reg + PLL_BASE);
	val &= ~PLL_BASE_ENABLE;
	clk_writel(val, c->reg + PLL_BASE);

	pll_do_iddq(c, PLL_MISC(c), PLLM_MISC_IDDQ, true);
}

static int tegra21_pllm_clk_set_rate(struct clk *c, unsigned long rate)
{
	u32 val, pdiv;
	unsigned long input_rate;
	struct clk_pll_freq_table cfg;
	const struct clk_pll_freq_table *sel = &cfg;

	pr_debug("%s: %s %lu\n", __func__, c->name, rate);

	if (c->state == ON) {
		if (rate != clk_get_rate_locked(c)) {
			pr_err("%s: Can not change memory %s rate in flight\n",
			       __func__, c->name);
			return -EINVAL;
		}
		return 0;
	}

	input_rate = clk_get_rate(c->parent);

	if (pll_dyn_ramp_find_cfg(c, &cfg, rate, input_rate, &pdiv))
		return -EINVAL;

	c->mul = sel->n;
	c->div = sel->m * sel->p;

	val = pmc_readl(PMC_PLLP_WB0_OVERRIDE);
	if (val & PMC_PLLP_WB0_OVERRIDE_PLLM_OVERRIDE) {
		val = pmc_readl(PMC_PLLM_WB0_OVERRIDE_2);
		val &= ~PMC_PLLM_WB0_OVERRIDE_2_DIVP_MASK;
		val |= pdiv << PMC_PLLM_WB0_OVERRIDE_2_DIVP_SHIFT;
		pmc_writel(val, PMC_PLLM_WB0_OVERRIDE_2);

		val = pmc_readl(PMC_PLLM_WB0_OVERRIDE);
		val &= ~(PLLM_BASE_DIVM_MASK | PLLM_BASE_DIVN_MASK);
		val |= (sel->m << PLL_BASE_DIVM_SHIFT) |
			(sel->n << PLL_BASE_DIVN_SHIFT);
		pmc_writel(val, PMC_PLLM_WB0_OVERRIDE);
	} else {
		val = clk_readl(c->reg + PLL_BASE);
		val &= ~(PLLM_BASE_DIVM_MASK | PLLM_BASE_DIVN_MASK |
			 PLLM_BASE_DIVP_MASK);
		val |= (sel->m << PLL_BASE_DIVM_SHIFT) |
			(sel->n << PLL_BASE_DIVN_SHIFT) |
			(pdiv << PLL_BASE_DIVP_SHIFT);
		clk_writel(val, c->reg + PLL_BASE);
	}

	return 0;
}

static struct clk_ops tegra_pllm_ops = {
	.init			= tegra21_pllm_clk_init,
	.enable			= tegra21_pllm_clk_enable,
	.disable		= tegra21_pllm_clk_disable,
	.set_rate		= tegra21_pllm_clk_set_rate,
};

/* non-monotonic mapping below is not a typo */
static u8 plle_p[PLLE_CMLDIV_MAX + 1] = {
/* CMLDIV: 0, 1, 2, 3, 4, 5, 6,  7,  8,  9, 10, 11, 12, 13, 14 */
/* p: */   1, 2, 3, 4, 5, 6, 8, 10, 12, 16, 12, 16, 20, 24, 32 };

static inline void select_pll_e_input(u32 aux_reg)
{
#if USE_PLLE_INPUT_PLLRE
	aux_reg |= PLLE_AUX_PLLRE_SEL;
#else
	aux_reg &= ~(PLLE_AUX_PLLRE_SEL | PLLE_AUX_PLLP_SEL);
#endif
	clk_writel(aux_reg, PLLE_AUX);
}

static void tegra21_plle_clk_init(struct clk *c)
{
	u32 val, p;
	struct clk *pll_ref = tegra_get_clock_by_name("pll_ref");
	struct clk *re_vco = tegra_get_clock_by_name("pll_re_vco");
	struct clk *pllp = tegra_get_clock_by_name("pllp");
#if USE_PLLE_INPUT_PLLRE
	struct clk *ref = re_vco;
#else
	struct clk *ref = pll_ref;
#endif

	val = clk_readl(c->reg + PLL_BASE);
	c->state = (val & PLL_BASE_ENABLE) ? ON : OFF;
	c->mul = (val & PLLE_BASE_DIVN_MASK) >> PLL_BASE_DIVN_SHIFT;
	c->div = (val & PLLE_BASE_DIVM_MASK) >> PLL_BASE_DIVM_SHIFT;
	p = (val & PLLE_BASE_DIVCML_MASK) >> PLLE_BASE_DIVCML_SHIFT;
	c->div *= plle_p[p];

	val = clk_readl(PLLE_AUX);
	c->parent = (val & PLLE_AUX_PLLRE_SEL) ? re_vco :
		(val & PLLE_AUX_PLLP_SEL) ? pllp : pll_ref;
	if (c->parent != ref) {
		if (c->state == ON) {
			WARN(1, "%s: pll_e is left enabled with %s input\n",
			     __func__, c->parent->name);
		} else {
			c->parent = ref;
			select_pll_e_input(val);
		}
	}
}

static void tegra21_plle_clk_disable(struct clk *c)
{
	u32 val;
	pr_debug("%s on clock %s\n", __func__, c->name);

	/* FIXME: do we need to restore other s/w controls ? */
	val = clk_readl(c->reg + PLL_BASE);
	val &= ~PLL_BASE_ENABLE;
	clk_writel(val, c->reg + PLL_BASE);

	val = clk_readl(c->reg + PLL_MISC(c));
	val |= PLLE_MISC_IDDQ_SW_CTRL | PLLE_MISC_IDDQ_SW_VALUE;
	pll_writel_delay(val, c->reg + PLL_MISC(c));

/* FIXME: Disable for initial Si bringup */
#if 0
	/* Set XUSB PLL pad pwr override and iddq */
	val = xusb_padctl_readl(XUSB_PADCTL_IOPHY_PLL_P0_CTL1_0);
	val |= XUSB_PADCTL_IOPHY_PLL_P0_CTL1_0_PLL_PWR_OVRD;
	val |= XUSB_PADCTL_IOPHY_PLL_P0_CTL1_0_PLL_IDDQ;
	xusb_padctl_writel(val, XUSB_PADCTL_IOPHY_PLL_P0_CTL1_0);
	xusb_padctl_readl(XUSB_PADCTL_IOPHY_PLL_P0_CTL1_0);
#endif
}

static int tegra21_plle_clk_enable(struct clk *c)
{
	u32 val;
	const struct clk_pll_freq_table *sel;
	unsigned long rate = c->u.pll.fixed_rate;
	unsigned long input_rate = clk_get_rate(c->parent);

	if (c->state == ON) {
		/* BL left plle enabled - don't change configuartion */
		pr_warn("%s: pll_e is already enabled\n", __func__);
		return 0;
	}

	for (sel = c->u.pll.freq_table; sel->input_rate != 0; sel++) {
		if (sel->input_rate == input_rate && sel->output_rate == rate)
			break;
	}

	if (sel->input_rate == 0) {
		pr_err("%s: %s input rate %lu is out-of-table\n",
		       __func__, c->name, input_rate);
		return -EINVAL;
	}

	/* setup locking configuration, s/w control of IDDQ and enable modes,
	   take pll out of IDDQ via s/w control, setup VREG */
	val = clk_readl(c->reg + PLL_BASE);
	val &= ~PLLE_BASE_LOCK_OVERRIDE;
	clk_writel(val, c->reg + PLL_BASE);

	val = clk_readl(c->reg + PLL_MISC(c));
	val |= PLLE_MISC_LOCK_ENABLE;
	val |= PLLE_MISC_IDDQ_SW_CTRL;
	val &= ~PLLE_MISC_IDDQ_SW_VALUE;
	val |= PLLE_MISC_PLLE_PTS;
	val |= PLLE_MISC_VREG_BG_CTRL_MASK | PLLE_MISC_VREG_CTRL_MASK;
	clk_writel(val, c->reg + PLL_MISC(c));
	udelay(5);

	/* configure dividers, disable SS */
	val = clk_readl(PLLE_SS_CTRL);
	val |= PLLE_SS_DISABLE;
	clk_writel(val, PLLE_SS_CTRL);

	val = clk_readl(c->reg + PLL_BASE);
	val &= ~(PLLE_BASE_DIVM_MASK | PLLE_BASE_DIVN_MASK |
		 PLLE_BASE_DIVCML_MASK);
	val |= (sel->m << PLL_BASE_DIVM_SHIFT) |
		(sel->n << PLL_BASE_DIVN_SHIFT) |
		(sel->cpcon << PLLE_BASE_DIVCML_SHIFT);
	pll_writel_delay(val, c->reg + PLL_BASE);
	c->mul = sel->n;
	c->div = sel->m * sel->p;

	/* enable and lock pll */
	val |= PLL_BASE_ENABLE;
	clk_writel(val, c->reg + PLL_BASE);
	tegra21_pll_clk_wait_for_lock(
		c, c->reg + PLL_MISC(c), PLLE_MISC_LOCK);
#if USE_PLLE_SS
	val = clk_readl(PLLE_SS_CTRL);
	val &= ~(PLLE_SS_CNTL_CENTER | PLLE_SS_CNTL_INVERT);
	val &= ~PLLE_SS_COEFFICIENTS_MASK;
	val |= PLLE_SS_COEFFICIENTS_VAL;
	clk_writel(val, PLLE_SS_CTRL);
	val &= ~(PLLE_SS_CNTL_SSC_BYP | PLLE_SS_CNTL_BYPASS_SS);
	pll_writel_delay(val, PLLE_SS_CTRL);
	val &= ~PLLE_SS_CNTL_INTERP_RESET;
	pll_writel_delay(val, PLLE_SS_CTRL);
#endif
#if !USE_PLLE_SWCTL
	/* switch pll under h/w control */
	val = clk_readl(c->reg + PLL_MISC(c));
	val &= ~PLLE_MISC_IDDQ_SW_CTRL;
	clk_writel(val, c->reg + PLL_MISC(c));

	val = clk_readl(PLLE_AUX);
	val |= PLLE_AUX_USE_LOCKDET;
	val &= ~PLLE_AUX_ENABLE_SWCTL;
	pll_writel_delay(val, PLLE_AUX);
	val |= PLLE_AUX_SEQ_ENABLE;
	pll_writel_delay(val, PLLE_AUX);
#endif
/* FIXME: Disable for initial Si bringup */
#if 0
	/* clear XUSB PLL pad pwr override and iddq */
	val = xusb_padctl_readl(XUSB_PADCTL_IOPHY_PLL_P0_CTL1_0);
	val &= ~XUSB_PADCTL_IOPHY_PLL_P0_CTL1_0_PLL_PWR_OVRD;
	val &= ~XUSB_PADCTL_IOPHY_PLL_P0_CTL1_0_PLL_IDDQ;
	xusb_padctl_writel(val, XUSB_PADCTL_IOPHY_PLL_P0_CTL1_0);
	xusb_padctl_readl(XUSB_PADCTL_IOPHY_PLL_P0_CTL1_0);
#endif

	/* enable hw control of xusb brick pll */
	usb_plls_hw_control_enable(XUSBIO_PLL_CFG0);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static void tegra21_plle_clk_resume(struct clk *c)
{
	u32 val = clk_readl(c->reg + PLL_BASE);
	if (val & PLL_BASE_ENABLE)
		return;		/* already resumed */

	/* Restore parent */
	val = clk_readl(PLLE_AUX);
	select_pll_e_input(val);
}
#endif

static struct clk_ops tegra_plle_ops = {
	.init			= tegra21_plle_clk_init,
	.enable			= tegra21_plle_clk_enable,
	.disable		= tegra21_plle_clk_disable,
};

/*
 * Tegra12 includes dynamic frequency lock loop (DFLL) with automatic voltage
 * control as possible CPU clock source. It is included in the Tegra12 clock
 * tree as "complex PLL" with standard Tegra clock framework APIs. However,
 * DFLL locking logic h/w access APIs are separated in the tegra_cl_dvfs.c
 * module. Hence, DFLL operations, with the exception of initialization, are
 * basically cl-dvfs wrappers.
 */

/* DFLL operations */
static void __init tegra21_dfll_cpu_late_init(struct clk *c)
{
#ifdef CONFIG_ARCH_TEGRA_HAS_CL_DVFS
	int ret;
	struct clk *cpu = tegra_get_clock_by_name("cpu_g");

	if (!cpu || !cpu->dvfs) {
		pr_err("%s: CPU dvfs is not present\n", __func__);
		return;
	}

	/* release dfll clock source reset, init cl_dvfs control logic, and
	   move dfll to initialized state, so it can be used as CPU source */
	tegra_periph_reset_deassert(c);
	ret = tegra_init_cl_dvfs();
	if (!ret) {
		c->state = OFF;
		c->u.dfll.cl_dvfs = platform_get_drvdata(&tegra_cl_dvfs_device);
		if (tegra_platform_is_silicon())
			use_dfll = CONFIG_TEGRA_USE_DFLL_RANGE;
		tegra_dvfs_set_dfll_range(cpu->dvfs, use_dfll);
		tegra_cl_dvfs_debug_init(c);
		pr_info("Tegra CPU DFLL is initialized with use_dfll = %d\n", use_dfll);
	}
#endif
}

static void tegra21_dfll_clk_init(struct clk *c)
{
	c->ops->init = tegra21_dfll_cpu_late_init;
}

static int tegra21_dfll_clk_enable(struct clk *c)
{
	return tegra_cl_dvfs_enable(c->u.dfll.cl_dvfs);
}

static void tegra21_dfll_clk_disable(struct clk *c)
{
	tegra_cl_dvfs_disable(c->u.dfll.cl_dvfs);
}

static int tegra21_dfll_clk_set_rate(struct clk *c, unsigned long rate)
{
	int ret = tegra_cl_dvfs_request_rate(c->u.dfll.cl_dvfs, rate);

	if (!ret)
		c->rate = tegra_cl_dvfs_request_get(c->u.dfll.cl_dvfs);

	return ret;
}

static void tegra21_dfll_clk_reset(struct clk *c, bool assert)
{
	u32 val = assert ? DFLL_BASE_RESET : 0;
	clk_writel_delay(val, c->reg);
}

static int
tegra21_dfll_clk_cfg_ex(struct clk *c, enum tegra_clk_ex_param p, u32 setting)
{
	if (p == TEGRA_CLK_DFLL_LOCK)
		return setting ? tegra_cl_dvfs_lock(c->u.dfll.cl_dvfs) :
				 tegra_cl_dvfs_unlock(c->u.dfll.cl_dvfs);
	return -EINVAL;
}

#ifdef CONFIG_PM_SLEEP
static void tegra21_dfll_clk_resume(struct clk *c)
{
	if (!(clk_readl(c->reg) & DFLL_BASE_RESET))
		return;		/* already resumed */

	if (c->state != UNINITIALIZED) {
		tegra_periph_reset_deassert(c);
		tegra_cl_dvfs_resume(c->u.dfll.cl_dvfs);
	}
}
#endif

static struct clk_ops tegra_dfll_ops = {
	.init			= tegra21_dfll_clk_init,
	.enable			= tegra21_dfll_clk_enable,
	.disable		= tegra21_dfll_clk_disable,
	.set_rate		= tegra21_dfll_clk_set_rate,
	.reset			= tegra21_dfll_clk_reset,
	.clk_cfg_ex		= tegra21_dfll_clk_cfg_ex,
};

/* DFLL sysfs interface */
static int tegra21_use_dfll_cb(const char *arg, const struct kernel_param *kp)
{
	int ret = 0;
	unsigned long c_flags, p_flags;
	unsigned int old_use_dfll;
	struct clk *c = tegra_get_clock_by_name("cpu");

	if (!c->parent || !c->parent->dvfs)
		return -ENOSYS;

	clk_lock_save(c, &c_flags);

	clk_lock_save(c->parent, &p_flags);
	old_use_dfll = use_dfll;
	param_set_int(arg, kp);

	if (use_dfll != old_use_dfll) {
		ret = tegra_dvfs_set_dfll_range(c->parent->dvfs, use_dfll);
		if (ret) {
			use_dfll = old_use_dfll;
		} else {
			ret = clk_set_rate_locked(c->parent,
				clk_get_rate_locked(c->parent));
			if (ret) {
				use_dfll = old_use_dfll;
				tegra_dvfs_set_dfll_range(
					c->parent->dvfs, use_dfll);
			}
		}
	}
	clk_unlock_restore(c->parent, &p_flags);
	clk_unlock_restore(c, &c_flags);
	tegra_recalculate_cpu_edp_limits();
	return ret;
}

static struct kernel_param_ops tegra21_use_dfll_ops = {
	.set = tegra21_use_dfll_cb,
	.get = param_get_int,
};
module_param_cb(use_dfll, &tegra21_use_dfll_ops, &use_dfll, 0644);

/*
 * PLL internal post divider ops for PLLs that have both VCO and post-divider
 * output separtely connected to the clock tree.
 */
static int tegra21_pll_out_clk_set_rate(struct clk *c, unsigned long rate)
{
	int p;
	u32 val, pdiv;
	unsigned long vco_rate, flags;
	struct clk *pll = c->parent;
	struct clk_pll_div_layout *divs = pll->u.pll.div_layout;

	pr_debug("%s: %s %lu\n", __func__, c->name, rate);

	if (c->flags & PLL_FIXED) {
		if (rate != c->u.pll.fixed_rate) {
			pr_err("%s: can not change fixed rate %lu to %lu\n",
			       c->name, c->u.pll.fixed_rate, rate);
			return -EINVAL;
		}
		return 0;
	}

	if (!rate)
		return -EINVAL;

	clk_lock_save(pll, &flags);

	vco_rate = clk_get_rate_locked(pll);
	p = DIV_ROUND_UP(vco_rate, rate);
	p = pll->u.pll.round_p_to_pdiv(p, &pdiv);
	if (IS_ERR_VALUE(p)) {
		pr_err("%s: Failed to set %s rate %lu\n",
		       __func__, c->name, rate);
		clk_unlock_restore(pll, &flags);
		return -EINVAL;
	}

	val = clk_readl(pll->reg);
	val &= ~divs->pdiv_mask;
	val |= pdiv << divs->pdiv_shift;
	pll_writel_delay(val, pll->reg);

	c->div = p;
	c->mul = 1;

	clk_unlock_restore(pll, &flags);
	return 0;
}

static int tegra21_pll_out_clk_enable(struct clk *c)
{
	return 0;
}

static void tegra21_pll_out_clk_init(struct clk *c)
{
	u32 p, val;
	unsigned long vco_rate;
	struct clk *pll = c->parent;
	struct clk_pll_div_layout *divs = pll->u.pll.div_layout;

	c->state = ON;
	c->max_rate = pll->u.pll.vco_max;
	c->min_rate = pll->u.pll.vco_min;

	if (!c->ops->set_rate) {
		/* fixed ratio output */
		if (c->mul && c->div) {
			c->max_rate = c->max_rate * c->mul / c->div;
			c->min_rate = c->min_rate * c->mul / c->div;
		}
		return;
	}
	c->min_rate = DIV_ROUND_UP(c->min_rate,
				   divs->pdiv_to_p[divs->pdiv_max]);

	/* PLL is enabled on boot - just record state */
	if (pll->state == ON) {
		val = clk_readl(pll->reg);
		p = (val & divs->pdiv_mask) >> divs->pdiv_shift;
		if (p > divs->pdiv_max) {
			WARN(1, "%s pdiv %u is above max %u\n",
			     c->name, p, divs->pdiv_max);
			p = divs->pdiv_max;
		}
		p = divs->pdiv_to_p[p];
		c->div = p;
		c->mul = 1;
		if (c->flags & PLL_FIXED)
			pll_clk_verify_fixed_rate(c);
		return;
	}

	if (c->flags & PLL_FIXED) {
		c->flags &= ~PLL_FIXED;	/* temporarily to allow set rate once */
		c->ops->set_rate(c, c->u.pll.fixed_rate);
		c->flags |= PLL_FIXED;
		pll_clk_verify_fixed_rate(c);
	} else {
		/* PLL is disabled - set 1/4 of VCO rate */
		vco_rate = clk_get_rate(pll);
		c->ops->set_rate(c, vco_rate / 4);
	}
}

static struct clk_ops tegra_pll_out_ops = {
	.init			= tegra21_pll_out_clk_init,
	.enable			= tegra21_pll_out_clk_enable,
	.set_rate		= tegra21_pll_out_clk_set_rate,
};

static struct clk_ops tegra_pll_out_fixed_ops = {
	.init			= tegra21_pll_out_clk_init,
	.enable			= tegra21_pll_out_clk_enable,
};

static void tegra21_pllu_hw_ctrl_set(struct clk *c)
{
	u32 val = clk_readl(c->reg);

	/* Put UTMI PLL under h/w control */
	tegra21_utmi_param_configure(c);

	/* Put PLLU under h/w control */
	usb_plls_hw_control_enable(PLLU_HW_PWRDN_CFG0);

	if (val & PLLU_BASE_OVERRIDE) {
		val &= ~PLLU_BASE_OVERRIDE;
		pll_writel_delay(val, c->reg);
	} else {
		/* FIXME: should it be WARN() ? */
		pr_info("%s: boot with h/w control already set\n", c->name);
	}

/* FIXME: Disable for initial Si bringup */
#if 0
	/* Set XUSB PLL pad pwr override and iddq */
	val = xusb_padctl_readl(XUSB_PADCTL_IOPHY_PLL_P0_CTL1_0);
	val |= XUSB_PADCTL_IOPHY_PLL_P0_CTL1_0_PLL_PWR_OVRD;
	val |= XUSB_PADCTL_IOPHY_PLL_P0_CTL1_0_PLL_IDDQ;
	xusb_padctl_writel(val, XUSB_PADCTL_IOPHY_PLL_P0_CTL1_0);
	xusb_padctl_readl(XUSB_PADCTL_IOPHY_PLL_P0_CTL1_0);
#endif
}

static void tegra21_pllu_out_clk_init(struct clk *c)
{
	u32 p, val;
	struct clk *pll = c->parent;
	val = clk_readl(pll->reg);

	/*
	 * If PLLU state is already under h/w control just record output ratio,
	 * and verify expected fixed output rate.
	 */
	if (!(val & PLLU_BASE_OVERRIDE)) {
		struct clk_pll_div_layout *divs = pll->u.pll.div_layout;

		c->state = ON;
		c->max_rate = pll->u.pll.vco_max;

		p = (val & divs->pdiv_mask) >> divs->pdiv_shift;
		if (p > divs->pdiv_max)
			p = divs->pdiv_max; /* defer invalid p WARN to verify */
		p = divs->pdiv_to_p[p];
		c->div = p;
		c->mul = 1;

		pll_clk_verify_fixed_rate(c);
	} else {
		/* Complete PLLU output s/w controlled initialization */
		tegra21_pll_out_clk_init(c);
	}

	/* Put USB plls under h/w control */
	tegra21_pllu_hw_ctrl_set(pll);
}

static struct clk_ops tegra_pllu_out_ops = {
	.init			= tegra21_pllu_out_clk_init,
	.enable			= tegra21_pll_out_clk_enable,
	.set_rate		= tegra21_pll_out_clk_set_rate,
};

#ifdef CONFIG_PM_SLEEP
static void tegra_pll_out_resume_enable(struct clk *c)
{
	struct clk *pll = c->parent;
	struct clk_pll_div_layout *divs = pll->u.pll.div_layout;
	u32 val = clk_readl(pll->reg);
	u32 pdiv;

	if (val & pll->u.pll.controls->enable_mask)
		return;		/* already resumed */

	/* Restore post divider */
	pll->u.pll.round_p_to_pdiv(c->div, &pdiv);
	val = clk_readl(pll->reg);
	val &= ~divs->pdiv_mask;
	val |= pdiv << divs->pdiv_shift;
	pll_writel_delay(val, pll->reg);

	/* Restore PLL feedback loop */
	tegra_pll_clk_resume_enable(pll);
}

static void tegra_pllu_out_resume_enable(struct clk *c)
{
	u32 val = clk_readl(c->parent->reg);
	if (!(val & PLLU_BASE_OVERRIDE) ||
	    val & c->parent->u.pll.controls->enable_mask)
		return;		/* already resumed */

	tegra_pll_out_resume_enable(c);
}
#endif

/* PLL external secondary divider ops (non-atomic shared register access) */
static DEFINE_SPINLOCK(pll_div_lock);

static int tegra21_pll_div_clk_set_rate(struct clk *c, unsigned long rate);
static void tegra21_pll_div_clk_init(struct clk *c)
{
	if (c->flags & DIV_U71) {
		u32 val, divu71;
		if (c->parent->state == OFF)
			c->ops->disable(c);

		val = clk_readl(c->reg);
		val >>= c->reg_shift;
		c->state = (val & PLL_OUT_CLKEN) ? ON : OFF;
		if (!(val & PLL_OUT_RESET_DISABLE))
			c->state = OFF;

		if (c->u.pll_div.default_rate) {
			int ret = tegra21_pll_div_clk_set_rate(
					c, c->u.pll_div.default_rate);
			if (!ret)
				return;
		}
		divu71 = (val & PLL_OUT_RATIO_MASK) >> PLL_OUT_RATIO_SHIFT;
		c->div = (divu71 + 2);
		c->mul = 2;
	} else if (c->flags & DIV_2) {
		c->state = ON;
		if (c->flags & (PLLD | PLLX)) {
			c->div = 2;
			c->mul = 1;
		}
		else
			BUG();
	} else if (c->flags & PLLU) {
		u32 val = clk_readl(c->reg);
		c->state = val & (0x1 << c->reg_shift) ? ON : OFF;
		c->max_rate = c->parent->max_rate;
	} else {
		c->state = ON;
		c->div = 1;
		c->mul = 1;
	}
}

static int tegra21_pll_div_clk_enable(struct clk *c)
{
	u32 val;
	u32 new_val;
	unsigned long flags;

	pr_debug("%s: %s\n", __func__, c->name);
	if (c->flags & DIV_U71) {
		spin_lock_irqsave(&pll_div_lock, flags);
		val = clk_readl(c->reg);
		new_val = val >> c->reg_shift;
		new_val &= 0xFFFF;

		new_val |= PLL_OUT_CLKEN | PLL_OUT_RESET_DISABLE;

		val &= ~(0xFFFF << c->reg_shift);
		val |= new_val << c->reg_shift;
		clk_writel_delay(val, c->reg);
		spin_unlock_irqrestore(&pll_div_lock, flags);
		return 0;
	} else if (c->flags & DIV_2) {
		return 0;
	} else if (c->flags & PLLU) {
		clk_lock_save(pll_u, &flags);
		val = clk_readl(c->reg) | (0x1 << c->reg_shift);
		clk_writel_delay(val, c->reg);
		clk_unlock_restore(pll_u, &flags);
		return 0;
	}
	return -EINVAL;
}

static void tegra21_pll_div_clk_disable(struct clk *c)
{
	u32 val;
	u32 new_val;
	unsigned long flags;

	pr_debug("%s: %s\n", __func__, c->name);
	if (c->flags & DIV_U71) {
		spin_lock_irqsave(&pll_div_lock, flags);
		val = clk_readl(c->reg);
		new_val = val >> c->reg_shift;
		new_val &= 0xFFFF;

		new_val &= ~(PLL_OUT_CLKEN | PLL_OUT_RESET_DISABLE);

		val &= ~(0xFFFF << c->reg_shift);
		val |= new_val << c->reg_shift;
		clk_writel_delay(val, c->reg);
		spin_unlock_irqrestore(&pll_div_lock, flags);
	} else if (c->flags & PLLU) {
		clk_lock_save(pll_u, &flags);
		val = clk_readl(c->reg) & (~(0x1 << c->reg_shift));
		clk_writel_delay(val, c->reg);
		clk_unlock_restore(pll_u, &flags);
	}
}

static int tegra21_pll_div_clk_set_rate(struct clk *c, unsigned long rate)
{
	u32 val;
	u32 new_val;
	int divider_u71;
	unsigned long parent_rate = clk_get_rate(c->parent);
	unsigned long flags;

	pr_debug("%s: %s %lu\n", __func__, c->name, rate);
	if (c->flags & DIV_U71) {
		divider_u71 = clk_div71_get_divider(
			parent_rate, rate, c->flags, ROUND_DIVIDER_UP);
		if (divider_u71 >= 0) {
			spin_lock_irqsave(&pll_div_lock, flags);
			val = clk_readl(c->reg);
			new_val = val >> c->reg_shift;
			new_val &= 0xFFFF;
			if (c->flags & DIV_U71_FIXED)
				new_val |= PLL_OUT_OVERRIDE;
			new_val &= ~PLL_OUT_RATIO_MASK;
			new_val |= divider_u71 << PLL_OUT_RATIO_SHIFT;

			val &= ~(0xFFFF << c->reg_shift);
			val |= new_val << c->reg_shift;
			clk_writel_delay(val, c->reg);
			c->div = divider_u71 + 2;
			c->mul = 2;
			spin_unlock_irqrestore(&pll_div_lock, flags);
			return 0;
		}
	} else if (c->flags & DIV_2)
		return clk_set_rate(c->parent, rate * 2);

	return -EINVAL;
}

static long tegra21_pll_div_clk_round_rate(struct clk *c, unsigned long rate)
{
	int divider;
	unsigned long parent_rate = clk_get_rate(c->parent);
	pr_debug("%s: %s %lu\n", __func__, c->name, rate);

	if (c->flags & DIV_U71) {
		divider = clk_div71_get_divider(
			parent_rate, rate, c->flags, ROUND_DIVIDER_UP);
		if (divider < 0)
			return divider;
		return DIV_ROUND_UP(parent_rate * 2, divider + 2);
	} else if (c->flags & DIV_2)
		/* no rounding - fixed DIV_2 dividers pass rate to parent PLL */
		return rate;

	return -EINVAL;
}

static struct clk_ops tegra_pll_div_ops = {
	.init			= tegra21_pll_div_clk_init,
	.enable			= tegra21_pll_div_clk_enable,
	.disable		= tegra21_pll_div_clk_disable,
	.set_rate		= tegra21_pll_div_clk_set_rate,
	.round_rate		= tegra21_pll_div_clk_round_rate,
};

/* Periph clk ops */
static inline u32 periph_clk_source_mask(struct clk *c)
{
	if (c->u.periph.src_mask)
		return c->u.periph.src_mask;
	else if (c->flags & MUX_PWM)
		return 3 << 28;
	else if (c->flags & MUX_CLK_OUT)
		return 3 << (c->u.periph.clk_num + 4);
	else if (c->flags & PLLD)
		return PLLD_BASE_DSI_MUX_MASK;
	else
		return 7 << 29;
}

static inline u32 periph_clk_source_shift(struct clk *c)
{
	if (c->u.periph.src_shift)
		return c->u.periph.src_shift;
	else if (c->flags & MUX_PWM)
		return 28;
	else if (c->flags & MUX_CLK_OUT)
		return c->u.periph.clk_num + 4;
	else if (c->flags & PLLD)
		return PLLD_BASE_DSI_MUX_SHIFT;
	else
		return 29;
}

static void tegra21_periph_clk_init(struct clk *c)
{
	u32 val = clk_readl(c->reg);
	const struct clk_mux_sel *mux = 0;
	const struct clk_mux_sel *sel;
	if (c->flags & MUX) {
		for (sel = c->inputs; sel->input != NULL; sel++) {
			if (((val & periph_clk_source_mask(c)) >>
			    periph_clk_source_shift(c)) == sel->value)
				mux = sel;
		}
		BUG_ON(!mux);

		c->parent = mux->input;
	} else {
		if (c->flags & PLLU) {
			/* for xusb_hs clock enforce SS div2 source */
			val &= ~periph_clk_source_mask(c);
			clk_writel_delay(val, c->reg);
		}
		c->parent = c->inputs[0].input;
	}

	/* if peripheral is left under reset - enforce safe rate */
	if (!(c->flags & PERIPH_NO_RESET) &&
	    (clk_readl(PERIPH_CLK_TO_RST_REG(c)) & PERIPH_CLK_TO_BIT(c))) {
		tegra_periph_clk_safe_rate_init(c);
		val = clk_readl(c->reg);
	}

	if (c->flags & DIV_U71) {
		u32 divu71 = val & PERIPH_CLK_SOURCE_DIVU71_MASK;
		if (c->flags & DIV_U71_IDLE) {
			val &= ~(PERIPH_CLK_SOURCE_DIVU71_MASK <<
				PERIPH_CLK_SOURCE_DIVIDLE_SHIFT);
			val |= (PERIPH_CLK_SOURCE_DIVIDLE_VAL <<
				PERIPH_CLK_SOURCE_DIVIDLE_SHIFT);
			clk_writel(val, c->reg);
		}
		c->div = divu71 + 2;
		c->mul = 2;
	} else if (c->flags & DIV_U151) {
		u32 divu151 = val & PERIPH_CLK_SOURCE_DIVU16_MASK;
		if ((c->flags & DIV_U151_UART) &&
		    (!(val & PERIPH_CLK_UART_DIV_ENB))) {
			divu151 = 0;
		}
		c->div = divu151 + 2;
		c->mul = 2;
	} else if (c->flags & DIV_U16) {
		u32 divu16 = val & PERIPH_CLK_SOURCE_DIVU16_MASK;
		c->div = divu16 + 1;
		c->mul = 1;
	} else {
		c->div = 1;
		c->mul = 1;
	}

	if (c->flags & PERIPH_NO_ENB) {
		c->state = c->parent->state;
		return;
	}

	c->state = ON;

	if (!(clk_readl(PERIPH_CLK_TO_ENB_REG(c)) & PERIPH_CLK_TO_BIT(c)))
		c->state = OFF;
	if (!(c->flags & PERIPH_NO_RESET))
		if (clk_readl(PERIPH_CLK_TO_RST_REG(c)) & PERIPH_CLK_TO_BIT(c))
			c->state = OFF;
}

static int tegra21_periph_clk_enable(struct clk *c)
{
	unsigned long flags;
	pr_debug("%s on clock %s\n", __func__, c->name);

	if (c->flags & PERIPH_NO_ENB)
		return 0;

	spin_lock_irqsave(&periph_refcount_lock, flags);

	tegra_periph_clk_enable_refcount[c->u.periph.clk_num]++;
	if (tegra_periph_clk_enable_refcount[c->u.periph.clk_num] > 1) {
		spin_unlock_irqrestore(&periph_refcount_lock, flags);
		return 0;
	}

	/* FIXME: WAR for HW bug 1438604 */
	if (!(tegra_platform_is_linsim() &&
		(!strcmp(c->name, "nvjpg") || !strcmp(c->name, "nvdec"))))
		clk_writel_delay(PERIPH_CLK_TO_BIT(c), PERIPH_CLK_TO_ENB_SET_REG(c));

	if (!(c->flags & PERIPH_NO_RESET) && !(c->flags & PERIPH_MANUAL_RESET)) {
		if (clk_readl(PERIPH_CLK_TO_RST_REG(c)) & PERIPH_CLK_TO_BIT(c)) {
			udelay(RESET_PROPAGATION_DELAY);
			clk_writel(PERIPH_CLK_TO_BIT(c), PERIPH_CLK_TO_RST_CLR_REG(c));
		}
	}
	spin_unlock_irqrestore(&periph_refcount_lock, flags);
	return 0;
}

static void tegra21_periph_clk_disable(struct clk *c)
{
	unsigned long val, flags;
	pr_debug("%s on clock %s\n", __func__, c->name);

	if (c->flags & PERIPH_NO_ENB)
		return;

	spin_lock_irqsave(&periph_refcount_lock, flags);

	if (c->refcnt)
		tegra_periph_clk_enable_refcount[c->u.periph.clk_num]--;

	if (tegra_periph_clk_enable_refcount[c->u.periph.clk_num] == 0) {
		/* If peripheral is in the APB bus then read the APB bus to
		 * flush the write operation in apb bus. This will avoid the
		 * peripheral access after disabling clock*/
		if (c->flags & PERIPH_ON_APB)
			val = chipid_readl();

		/* FIXME: WAR for HW bug 1438604 */
		if (!(tegra_platform_is_linsim() &&
			(!strcmp(c->name, "nvjpg") || !strcmp(c->name, "nvdec"))))
			clk_writel_delay(
				PERIPH_CLK_TO_BIT(c), PERIPH_CLK_TO_ENB_CLR_REG(c));
	}
	spin_unlock_irqrestore(&periph_refcount_lock, flags);
}

static void tegra21_periph_clk_reset(struct clk *c, bool assert)
{
	unsigned long val;
	pr_debug("%s %s on clock %s\n", __func__,
		 assert ? "assert" : "deassert", c->name);

	if (c->flags & PERIPH_NO_ENB)
		return;

	if (!(c->flags & PERIPH_NO_RESET)) {
		if (assert) {
			/* If peripheral is in the APB bus then read the APB
			 * bus to flush the write operation in apb bus. This
			 * will avoid the peripheral access after disabling
			 * clock */
			if (c->flags & PERIPH_ON_APB)
				val = chipid_readl();

			clk_writel(PERIPH_CLK_TO_BIT(c),
				   PERIPH_CLK_TO_RST_SET_REG(c));
		} else
			clk_writel(PERIPH_CLK_TO_BIT(c),
				   PERIPH_CLK_TO_RST_CLR_REG(c));
	}
}

static int tegra21_periph_clk_set_parent(struct clk *c, struct clk *p)
{
	u32 val;
	const struct clk_mux_sel *sel;
	pr_debug("%s: %s %s\n", __func__, c->name, p->name);

	if (!(c->flags & MUX))
		return (p == c->parent) ? 0 : (-EINVAL);

	for (sel = c->inputs; sel->input != NULL; sel++) {
		if (sel->input == p) {
			val = clk_readl(c->reg);
			val &= ~periph_clk_source_mask(c);
			val |= (sel->value << periph_clk_source_shift(c));

			if (c->refcnt)
				clk_enable(p);

			clk_writel_delay(val, c->reg);

			if (c->refcnt && c->parent)
				clk_disable(c->parent);

			clk_reparent(c, p);
			return 0;
		}
	}

	return -EINVAL;
}

static int tegra21_periph_clk_set_rate(struct clk *c, unsigned long rate)
{
	u32 val;
	int divider;
	unsigned long parent_rate = clk_get_rate(c->parent);

	if (c->flags & DIV_U71) {
		divider = clk_div71_get_divider(
			parent_rate, rate, c->flags, ROUND_DIVIDER_UP);
		if (divider >= 0) {
			val = clk_readl(c->reg);
			val &= ~PERIPH_CLK_SOURCE_DIVU71_MASK;
			val |= divider;
			clk_writel_delay(val, c->reg);
			c->div = divider + 2;
			c->mul = 2;
			return 0;
		}
	} else if (c->flags & DIV_U151) {
		divider = clk_div151_get_divider(
			parent_rate, rate, c->flags, ROUND_DIVIDER_UP);
		if (divider >= 0) {
			val = clk_readl(c->reg);
			val &= ~PERIPH_CLK_SOURCE_DIVU16_MASK;
			val |= divider;
			if (c->flags & DIV_U151_UART) {
				if (divider)
					val |= PERIPH_CLK_UART_DIV_ENB;
				else
					val &= ~PERIPH_CLK_UART_DIV_ENB;
			}
			clk_writel_delay(val, c->reg);
			c->div = divider + 2;
			c->mul = 2;
			return 0;
		}
	} else if (c->flags & DIV_U16) {
		divider = clk_div16_get_divider(parent_rate, rate);
		if (divider >= 0) {
			val = clk_readl(c->reg);
			val &= ~PERIPH_CLK_SOURCE_DIVU16_MASK;
			val |= divider;
			clk_writel_delay(val, c->reg);
			c->div = divider + 1;
			c->mul = 1;
			return 0;
		}
	} else if (parent_rate <= rate) {
		c->div = 1;
		c->mul = 1;
		return 0;
	}
	return -EINVAL;
}

static long tegra21_periph_clk_round_rate(struct clk *c,
	unsigned long rate)
{
	int divider;
	unsigned long parent_rate = clk_get_rate(c->parent);
	pr_debug("%s: %s %lu\n", __func__, c->name, rate);

	if (c->flags & DIV_U71) {
		divider = clk_div71_get_divider(
			parent_rate, rate, c->flags, ROUND_DIVIDER_UP);
		if (divider < 0)
			return divider;

		return DIV_ROUND_UP(parent_rate * 2, divider + 2);
	} else if (c->flags & DIV_U151) {
		divider = clk_div151_get_divider(
			parent_rate, rate, c->flags, ROUND_DIVIDER_UP);
		if (divider < 0)
			return divider;

		return DIV_ROUND_UP(parent_rate * 2, divider + 2);
	} else if (c->flags & DIV_U16) {
		divider = clk_div16_get_divider(parent_rate, rate);
		if (divider < 0)
			return divider;
		return DIV_ROUND_UP(parent_rate, divider + 1);
	}
	return -EINVAL;
}

static struct clk_ops tegra_periph_clk_ops = {
	.init			= &tegra21_periph_clk_init,
	.enable			= &tegra21_periph_clk_enable,
	.disable		= &tegra21_periph_clk_disable,
	.set_parent		= &tegra21_periph_clk_set_parent,
	.set_rate		= &tegra21_periph_clk_set_rate,
	.round_rate		= &tegra21_periph_clk_round_rate,
	.reset			= &tegra21_periph_clk_reset,
};

/* Supper skipper ops */
static void tegra21_clk_super_skip_init(struct clk *c)
{
	u32 val = clk_readl(c->reg);

	if (!c->parent)
		c->parent = c - 1;
	c->parent->skipper = c;

	/* Skipper is always ON (does not gate the clock) */
	c->state = ON;
	c->max_rate = c->parent->max_rate;

	if (val & SUPER_SKIPPER_ENABLE) {
		c->div = ((val & SUPER_SKIPPER_DIV_MASK) >>
			  SUPER_SKIPPER_DIV_SHIFT) + 1;
		c->mul = ((val & SUPER_SKIPPER_MUL_MASK) >>
			  SUPER_SKIPPER_MUL_SHIFT) + 1;
	} else {
		c->div = 1;
		c->mul = 1;
	}
}

static int tegra21_clk_super_skip_enable(struct clk *c)
{
	/* no clock gate in skipper, just pass thru to parent */
	return 0;
}

static int tegra21_clk_super_skip_set_rate(struct clk *c, unsigned long rate)
{
	u32 val, mul, div;
	u64 output_rate = rate;
	unsigned long input_rate, flags;

	pr_debug("%s: %s %lu\n", __func__, c->name, rate);

	/*
	 * Locking parent clock prevents parent rate change while super skipper
	 * is updated. It also takes care of super skippers that share h/w
	 * register with parent clock divider. Skipper output rate can be
	 * rounded up, since volatge is set based on source clock rate.
	 */
	clk_lock_save(c->parent, &flags);
	input_rate = clk_get_rate_locked(c->parent);

	div = 1 << SUPER_SKIPPER_TERM_SIZE;
	output_rate <<= SUPER_SKIPPER_TERM_SIZE;
	output_rate += input_rate - 1;
	do_div(output_rate, input_rate);
	mul = output_rate ? : 1;

	if (mul < div) {
		val = SUPER_SKIPPER_ENABLE |
			((mul - 1) << SUPER_SKIPPER_MUL_SHIFT) |
			((div - 1) << SUPER_SKIPPER_DIV_SHIFT);
		c->div = div;
		c->mul = mul;
	} else {
		val = 0;
		c->div = 1;
		c->mul = 1;
	}

	/* FIXME: for SCLK c->reg, this write to be replaced with IPC to BPMP */
	clk_writel(val, c->reg);

	clk_unlock_restore(c->parent, &flags);
	return 0;
}

static struct clk_ops tegra_clk_super_skip_ops = {
	.init			= &tegra21_clk_super_skip_init,
	.enable			= &tegra21_clk_super_skip_enable,
	.set_rate		= &tegra21_clk_super_skip_set_rate,
};

/* 1x shared bus ops */
static long _1x_round_updown(struct clk *c, struct clk *src,
				unsigned long rate, bool up)
{
	return fixed_src_bus_round_updown(c, src, c->flags, rate, up, NULL);
}

static long tegra21_1xbus_round_updown(struct clk *c, unsigned long rate,
					    bool up)
{
	unsigned long pll_low_rate, pll_high_rate;

	rate = max(rate, c->min_rate);

	pll_low_rate = _1x_round_updown(c, c->u.periph.pll_low, rate, up);
	if (rate <= c->u.periph.threshold) {
		c->u.periph.pll_selected = c->u.periph.pll_low;
		return pll_low_rate;
	}

	pll_high_rate = _1x_round_updown(c, c->u.periph.pll_high, rate, up);
	if (pll_high_rate <= c->u.periph.threshold) {
		c->u.periph.pll_selected = c->u.periph.pll_low;
		return pll_low_rate;  /* prevent oscillation across threshold */
	}

	if (up) {
		/* rounding up: both plls may hit max, and round down */
		if (pll_high_rate < rate) {
			if (pll_low_rate < pll_high_rate) {
				c->u.periph.pll_selected = c->u.periph.pll_high;
				return pll_high_rate;
			}
		} else {
			if ((pll_low_rate < rate) ||
			    (pll_low_rate > pll_high_rate)) {
				c->u.periph.pll_selected = c->u.periph.pll_high;
				return pll_high_rate;
			}
		}
	} else if (pll_low_rate < pll_high_rate) {
		/* rounding down: to get here both plls able to round down */
		c->u.periph.pll_selected = c->u.periph.pll_high;
		return pll_high_rate;
	}
	c->u.periph.pll_selected = c->u.periph.pll_low;
	return pll_low_rate;
}

static long tegra21_1xbus_round_rate(struct clk *c, unsigned long rate)
{
	return tegra21_1xbus_round_updown(c, rate, true);
}

static int tegra21_1xbus_set_rate(struct clk *c, unsigned long rate)
{
	/* Compensate rate truncating during rounding */
	return tegra21_periph_clk_set_rate(c, rate + 1);
}

static int tegra21_clk_1xbus_update(struct clk *c)
{
	int ret;
	struct clk *new_parent;
	unsigned long rate, old_rate;

	if (detach_shared_bus)
		return 0;

	rate = tegra21_clk_shared_bus_update(c, NULL, NULL, NULL);

	old_rate = clk_get_rate_locked(c);
	pr_debug("\n1xbus %s: rate %lu on parent %s: new request %lu\n",
		 c->name, old_rate, c->parent->name, rate);
	if (rate == old_rate)
		return 0;

	if (!c->u.periph.min_div_low || !c->u.periph.min_div_high) {
		unsigned long r, m = c->max_rate;
		r = clk_get_rate(c->u.periph.pll_low);
		c->u.periph.min_div_low = DIV_ROUND_UP(r, m) * c->mul;
		r = clk_get_rate(c->u.periph.pll_high);
		c->u.periph.min_div_high = DIV_ROUND_UP(r, m) * c->mul;
	}

	new_parent = c->u.periph.pll_selected;

	/*
	 * The transition procedure below is guaranteed to switch to the target
	 * parent/rate without violation of max clock limits. It would attempt
	 * to switch without dip in bus rate if it is possible, but this cannot
	 * be guaranteed (example: switch from 408 MHz : 1 to 624 MHz : 2 with
	 * maximum bus limit 408 MHz will be executed as 408 => 204 => 312 MHz,
	 * and there is no way to avoid rate dip in this case).
	 */
	if (new_parent != c->parent) {
		int interim_div = 0;
		/* Switching to pll_high may over-clock bus if current divider
		   is too small - increase divider to safe value */
		if ((new_parent == c->u.periph.pll_high) &&
		    (c->div < c->u.periph.min_div_high))
			interim_div = c->u.periph.min_div_high;

		/* Switching to pll_low may dip down rate if current divider
		   is too big - decrease divider as much as we can */
		if ((new_parent == c->u.periph.pll_low) &&
		    (c->div > c->u.periph.min_div_low) &&
		    (c->div > c->u.periph.min_div_high))
			interim_div = c->u.periph.min_div_low;

		if (interim_div) {
			u64 interim_rate = old_rate * c->div;
			do_div(interim_rate, interim_div);
			ret = clk_set_rate_locked(c, interim_rate);
			if (ret) {
				pr_err("Failed to set %s rate to %lu\n",
				       c->name, (unsigned long)interim_rate);
				return ret;
			}
			pr_debug("1xbus %s: rate %lu on parent %s\n", c->name,
				 clk_get_rate_locked(c), c->parent->name);
		}

		ret = clk_set_parent_locked(c, new_parent);
		if (ret) {
			pr_err("Failed to set %s parent %s\n",
			       c->name, new_parent->name);
			return ret;
		}

		old_rate = clk_get_rate_locked(c);
		pr_debug("1xbus %s: rate %lu on parent %s\n", c->name,
			 old_rate, c->parent->name);
		if (rate == old_rate)
			return 0;
	}

	ret = clk_set_rate_locked(c, rate);
	if (ret) {
		pr_err("Failed to set %s rate to %lu\n", c->name, rate);
		return ret;
	}
	pr_debug("1xbus %s: rate %lu on parent %s\n", c->name,
		 clk_get_rate_locked(c), c->parent->name);
	return 0;

}

static struct clk_ops tegra_1xbus_clk_ops = {
	.init			= &tegra21_periph_clk_init,
	.enable			= &tegra21_periph_clk_enable,
	.disable		= &tegra21_periph_clk_disable,
	.set_parent		= &tegra21_periph_clk_set_parent,
	.set_rate		= &tegra21_1xbus_set_rate,
	.round_rate		= &tegra21_1xbus_round_rate,
	.round_rate_updown	= &tegra21_1xbus_round_updown,
	.reset			= &tegra21_periph_clk_reset,
	.shared_bus_update	= &tegra21_clk_1xbus_update,
};

/* Periph extended clock configuration ops */
static int
tegra21_vi_clk_cfg_ex(struct clk *c, enum tegra_clk_ex_param p, u32 setting)
{
	if (p == TEGRA_CLK_VI_INP_SEL) {
		u32 val = clk_readl(c->reg);
		val &= ~PERIPH_CLK_VI_SEL_EX_MASK;
		val |= (setting << PERIPH_CLK_VI_SEL_EX_SHIFT) &
			PERIPH_CLK_VI_SEL_EX_MASK;
		clk_writel(val, c->reg);
		return 0;
	}
	return -EINVAL;
}

static struct clk_ops tegra_vi_clk_ops = {
	.init			= &tegra21_periph_clk_init,
	.enable			= &tegra21_periph_clk_enable,
	.disable		= &tegra21_periph_clk_disable,
	.set_parent		= &tegra21_periph_clk_set_parent,
	.set_rate		= &tegra21_periph_clk_set_rate,
	.round_rate		= &tegra21_periph_clk_round_rate,
	.clk_cfg_ex		= &tegra21_vi_clk_cfg_ex,
	.reset			= &tegra21_periph_clk_reset,
};

static int
tegra21_sor_clk_cfg_ex(struct clk *c, enum tegra_clk_ex_param p, u32 setting)
{
	if (p == TEGRA_CLK_SOR_CLK_SEL) {
		u32 val = clk_readl(c->reg);
		val &= ~PERIPH_CLK_SOR_CLK_SEL_MASK;
		val |= (setting << PERIPH_CLK_SOR_CLK_SEL_SHIFT) &
			PERIPH_CLK_SOR_CLK_SEL_MASK;
		clk_writel(val, c->reg);
		return 0;
	}
	return -EINVAL;
}

static struct clk_ops tegra_sor_clk_ops = {
	.init			= &tegra21_periph_clk_init,
	.enable			= &tegra21_periph_clk_enable,
	.disable		= &tegra21_periph_clk_disable,
	.set_parent		= &tegra21_periph_clk_set_parent,
	.set_rate		= &tegra21_periph_clk_set_rate,
	.round_rate		= &tegra21_periph_clk_round_rate,
	.clk_cfg_ex		= &tegra21_sor_clk_cfg_ex,
	.reset			= &tegra21_periph_clk_reset,
};

static int
tegra21_dtv_clk_cfg_ex(struct clk *c, enum tegra_clk_ex_param p, u32 setting)
{
	if (p == TEGRA_CLK_DTV_INVERT) {
		u32 val = clk_readl(c->reg);
		if (setting)
			val |= PERIPH_CLK_DTV_POLARITY_INV;
		else
			val &= ~PERIPH_CLK_DTV_POLARITY_INV;
		clk_writel(val, c->reg);
		return 0;
	}
	return -EINVAL;
}

static struct clk_ops tegra_dtv_clk_ops = {
	.init			= &tegra21_periph_clk_init,
	.enable			= &tegra21_periph_clk_enable,
	.disable		= &tegra21_periph_clk_disable,
	.set_parent		= &tegra21_periph_clk_set_parent,
	.set_rate		= &tegra21_periph_clk_set_rate,
	.round_rate		= &tegra21_periph_clk_round_rate,
	.clk_cfg_ex		= &tegra21_dtv_clk_cfg_ex,
	.reset			= &tegra21_periph_clk_reset,
};

static struct clk_ops tegra_dsi_clk_ops = {
	.init			= &tegra21_periph_clk_init,
	.enable			= &tegra21_periph_clk_enable,
	.disable		= &tegra21_periph_clk_disable,
	.set_rate		= &tegra21_periph_clk_set_rate,
	.round_rate		= &tegra21_periph_clk_round_rate,
	.reset			= &tegra21_periph_clk_reset,
};

/* pciex clock support only reset function */
static void tegra21_pciex_clk_init(struct clk *c)
{
	c->state = c->parent->state;
}

static int tegra21_pciex_clk_enable(struct clk *c)
{
	return 0;
}

static void tegra21_pciex_clk_disable(struct clk *c)
{
}

static int tegra21_pciex_clk_set_rate(struct clk *c, unsigned long rate)
{
	unsigned long parent_rate = clk_get_rate(c->parent);

	/*
	 * the only supported pcie configurations:
	 * Gen1: plle = 100MHz, link at 250MHz
	 * Gen2: plle = 100MHz, link at 500MHz
	 */
	if (parent_rate == 100000000) {
		if (rate == 500000000) {
			c->mul = 5;
			c->div = 1;
			return 0;
		} else if (rate == 250000000) {
			c->mul = 5;
			c->div = 2;
			return 0;
		}
	}
	return -EINVAL;
}

static struct clk_ops tegra_pciex_clk_ops = {
	.init     = tegra21_pciex_clk_init,
	.enable	  = tegra21_pciex_clk_enable,
	.disable  = tegra21_pciex_clk_disable,
	.set_rate = tegra21_pciex_clk_set_rate,
	.reset    = tegra21_periph_clk_reset,
};

/* Output clock ops */

static DEFINE_SPINLOCK(clk_out_lock);

static void tegra21_clk_out_init(struct clk *c)
{
	const struct clk_mux_sel *mux = 0;
	const struct clk_mux_sel *sel;
	u32 val = pmc_readl(c->reg);

	c->state = (val & (0x1 << c->u.periph.clk_num)) ? ON : OFF;
	c->mul = 1;
	c->div = 1;

	for (sel = c->inputs; sel->input != NULL; sel++) {
		if (((val & periph_clk_source_mask(c)) >>
		     periph_clk_source_shift(c)) == sel->value)
			mux = sel;
	}
	BUG_ON(!mux);
	c->parent = mux->input;
}

static int tegra21_clk_out_enable(struct clk *c)
{
	u32 val;
	unsigned long flags;

	pr_debug("%s on clock %s\n", __func__, c->name);

	spin_lock_irqsave(&clk_out_lock, flags);
	val = pmc_readl(c->reg);
	val |= (0x1 << c->u.periph.clk_num);
	pmc_writel(val, c->reg);
	spin_unlock_irqrestore(&clk_out_lock, flags);

	return 0;
}

static void tegra21_clk_out_disable(struct clk *c)
{
	u32 val;
	unsigned long flags;

	pr_debug("%s on clock %s\n", __func__, c->name);

	spin_lock_irqsave(&clk_out_lock, flags);
	val = pmc_readl(c->reg);
	val &= ~(0x1 << c->u.periph.clk_num);
	pmc_writel(val, c->reg);
	spin_unlock_irqrestore(&clk_out_lock, flags);
}

static int tegra21_clk_out_set_parent(struct clk *c, struct clk *p)
{
	u32 val;
	unsigned long flags;
	const struct clk_mux_sel *sel;

	pr_debug("%s: %s %s\n", __func__, c->name, p->name);

	for (sel = c->inputs; sel->input != NULL; sel++) {
		if (sel->input == p) {
			if (c->refcnt)
				clk_enable(p);

			spin_lock_irqsave(&clk_out_lock, flags);
			val = pmc_readl(c->reg);
			val &= ~periph_clk_source_mask(c);
			val |= (sel->value << periph_clk_source_shift(c));
			pmc_writel(val, c->reg);
			spin_unlock_irqrestore(&clk_out_lock, flags);

			if (c->refcnt && c->parent)
				clk_disable(c->parent);

			clk_reparent(c, p);
			return 0;
		}
	}
	return -EINVAL;
}

static struct clk_ops tegra_clk_out_ops = {
	.init			= &tegra21_clk_out_init,
	.enable			= &tegra21_clk_out_enable,
	.disable		= &tegra21_clk_out_disable,
	.set_parent		= &tegra21_clk_out_set_parent,
};


/* External memory controller clock ops */
static void tegra21_emc_clk_init(struct clk *c)
{
	tegra21_periph_clk_init(c);
	tegra_emc_dram_type_init(c);
	c->max_rate = clk_get_rate(c->parent);
}

static long tegra21_emc_clk_round_updown(struct clk *c, unsigned long rate,
					 bool up)
{
	unsigned long new_rate = max(rate, c->min_rate);

	new_rate = tegra_emc_round_rate_updown(new_rate, up);
	if (IS_ERR_VALUE(new_rate))
		new_rate = c->max_rate;

	return new_rate;
}

static long tegra21_emc_clk_round_rate(struct clk *c, unsigned long rate)
{
	return tegra21_emc_clk_round_updown(c, rate, true);
}

void tegra_mc_divider_update(struct clk *emc)
{
	emc->child_bus->div = (clk_readl(emc->reg) &
			       PERIPH_CLK_SOURCE_EMC_MC_SAME) ? 1 : 2;
}

static int tegra21_emc_clk_set_rate(struct clk *c, unsigned long rate)
{
	int ret;
	u32 div_value;
	struct clk *p;

	/* The tegra21x memory controller has an interlock with the clock
	 * block that allows memory shadowed registers to be updated,
	 * and then transfer them to the main registers at the same
	 * time as the clock update without glitches. During clock change
	 * operation both clock parent and divider may change simultaneously
	 * to achieve requested rate. */
	p = tegra_emc_predict_parent(rate, &div_value);
	div_value += 2;		/* emc has fractional DIV_U71 divider */
	if (IS_ERR_OR_NULL(p)) {
		pr_err("%s: Failed to predict emc parent for rate %lu\n",
		       __func__, rate);
		return -EINVAL;
	}

	if (p == c->parent) {
		if (div_value == c->div)
			return 0;
	} else if (c->refcnt)
		clk_enable(p);

	ret = tegra_emc_set_rate(rate);
	if (ret < 0)
		return ret;

	if (p != c->parent) {
		if(c->refcnt && c->parent)
			clk_disable(c->parent);
		clk_reparent(c, p);
	}
	c->div = div_value;
	c->mul = 2;
	return 0;
}

static int tegra21_clk_emc_bus_update(struct clk *bus)
{
	struct clk *p = NULL;
	unsigned long rate, parent_rate, backup_rate;

	if (detach_shared_bus)
		return 0;

	rate = tegra21_clk_shared_bus_update(bus, NULL, NULL, NULL);

	if (rate == clk_get_rate_locked(bus))
		return 0;

	if (tegra_platform_is_fpga())
		return 0;

	if (!tegra_emc_is_parent_ready(rate, &p, &parent_rate, &backup_rate)) {
		if (bus->parent == p) {
			/* need backup to re-lock current parent */
			if (IS_ERR_VALUE(backup_rate) ||
			    clk_set_rate_locked(bus, backup_rate)) {
				pr_err("%s: Failed to backup %s for rate %lu\n",
				       __func__, bus->name, rate);
				return -EINVAL;
			}

			if (p->refcnt) {
				pr_err("%s: %s has other than emc child\n",
				       __func__, p->name);
				return -EINVAL;
			}
		}
		if (clk_set_rate(p, parent_rate)) {
			pr_err("%s: Failed to set %s rate %lu\n",
			       __func__, p->name, parent_rate);
			return -EINVAL;
		}
	}

	return clk_set_rate_locked(bus, rate);
}

static struct clk_ops tegra_emc_clk_ops = {
	.init			= &tegra21_emc_clk_init,
	.enable			= &tegra21_periph_clk_enable,
	.disable		= &tegra21_periph_clk_disable,
	.set_rate		= &tegra21_emc_clk_set_rate,
	.round_rate		= &tegra21_emc_clk_round_rate,
	.round_rate_updown	= &tegra21_emc_clk_round_updown,
	.reset			= &tegra21_periph_clk_reset,
	.shared_bus_update	= &tegra21_clk_emc_bus_update,
};

static void tegra21_mc_clk_init(struct clk *c)
{
	c->state = ON;
	if (!(clk_readl(PERIPH_CLK_TO_ENB_REG(c)) & PERIPH_CLK_TO_BIT(c)))
		c->state = OFF;

	c->parent->child_bus = c;
	tegra_mc_divider_update(c->parent);
	c->mul = 1;
}

static struct clk_ops tegra_mc_clk_ops = {
	.init			= &tegra21_mc_clk_init,
	.enable			= &tegra21_periph_clk_enable,
	.disable		= &tegra21_periph_clk_disable,
};


/* Clock doubler ops (non-atomic shared register access) */
static DEFINE_SPINLOCK(doubler_lock);

static void tegra21_clk_double_init(struct clk *c)
{
	u32 val = clk_readl(c->reg);
	c->mul = val & (0x1 << c->reg_shift) ? 1 : 2;
	c->div = 1;
	c->state = ON;
	if (!(clk_readl(PERIPH_CLK_TO_ENB_REG(c)) & PERIPH_CLK_TO_BIT(c)))
		c->state = OFF;
};

static int tegra21_clk_double_set_rate(struct clk *c, unsigned long rate)
{
	u32 val;
	unsigned long parent_rate = clk_get_rate(c->parent);
	unsigned long flags;

	if (rate == parent_rate) {
		spin_lock_irqsave(&doubler_lock, flags);
		val = clk_readl(c->reg) | (0x1 << c->reg_shift);
		clk_writel(val, c->reg);
		c->mul = 1;
		c->div = 1;
		spin_unlock_irqrestore(&doubler_lock, flags);
		return 0;
	} else if (rate == 2 * parent_rate) {
		spin_lock_irqsave(&doubler_lock, flags);
		val = clk_readl(c->reg) & (~(0x1 << c->reg_shift));
		clk_writel(val, c->reg);
		c->mul = 2;
		c->div = 1;
		spin_unlock_irqrestore(&doubler_lock, flags);
		return 0;
	}
	return -EINVAL;
}

static struct clk_ops tegra_clk_double_ops = {
	.init			= &tegra21_clk_double_init,
	.enable			= &tegra21_periph_clk_enable,
	.disable		= &tegra21_periph_clk_disable,
	.set_rate		= &tegra21_clk_double_set_rate,
};

/* Audio sync clock ops */
static int tegra21_sync_source_set_rate(struct clk *c, unsigned long rate)
{
	c->rate = rate;
	return 0;
}

static struct clk_ops tegra_sync_source_ops = {
	.set_rate		= &tegra21_sync_source_set_rate,
};

static void tegra21_audio_sync_clk_init(struct clk *c)
{
	int source;
	const struct clk_mux_sel *sel;
	u32 val = clk_readl(c->reg);
	c->state = (val & AUDIO_SYNC_DISABLE_BIT) ? OFF : ON;
	source = val & AUDIO_SYNC_SOURCE_MASK;
	for (sel = c->inputs; sel->input != NULL; sel++)
		if (sel->value == source)
			break;
	BUG_ON(sel->input == NULL);
	c->parent = sel->input;
}

static int tegra21_audio_sync_clk_enable(struct clk *c)
{
	u32 val = clk_readl(c->reg);
	clk_writel((val & (~AUDIO_SYNC_DISABLE_BIT)), c->reg);
	return 0;
}

static void tegra21_audio_sync_clk_disable(struct clk *c)
{
	u32 val = clk_readl(c->reg);
	clk_writel((val | AUDIO_SYNC_DISABLE_BIT), c->reg);
}

static int tegra21_audio_sync_clk_set_parent(struct clk *c, struct clk *p)
{
	u32 val;
	const struct clk_mux_sel *sel;
	for (sel = c->inputs; sel->input != NULL; sel++) {
		if (sel->input == p) {
			val = clk_readl(c->reg);
			val &= ~AUDIO_SYNC_SOURCE_MASK;
			val |= sel->value;

			if (c->refcnt)
				clk_enable(p);

			clk_writel(val, c->reg);

			if (c->refcnt && c->parent)
				clk_disable(c->parent);

			clk_reparent(c, p);
			return 0;
		}
	}

	return -EINVAL;
}

static struct clk_ops tegra_audio_sync_clk_ops = {
	.init       = tegra21_audio_sync_clk_init,
	.enable     = tegra21_audio_sync_clk_enable,
	.disable    = tegra21_audio_sync_clk_disable,
	.set_parent = tegra21_audio_sync_clk_set_parent,
};

/* cml0 (pcie), and cml1 (sata) clock ops */
static void tegra21_cml_clk_init(struct clk *c)
{
	u32 val = clk_readl(c->reg);
	c->state = val & (0x1 << c->u.periph.clk_num) ? ON : OFF;
}

static int tegra21_cml_clk_enable(struct clk *c)
{
	u32 val = clk_readl(c->reg);
	val |= (0x1 << c->u.periph.clk_num);
	clk_writel(val, c->reg);
	return 0;
}

static void tegra21_cml_clk_disable(struct clk *c)
{
	u32 val = clk_readl(c->reg);
	val &= ~(0x1 << c->u.periph.clk_num);
	clk_writel(val, c->reg);
}

static struct clk_ops tegra_cml_clk_ops = {
	.init			= &tegra21_cml_clk_init,
	.enable			= &tegra21_cml_clk_enable,
	.disable		= &tegra21_cml_clk_disable,
};


/* cbus ops */
/*
 * Some clocks require dynamic re-locking of source PLL in order to
 * achieve frequency scaling granularity that matches characterized
 * core voltage steps. The cbus clock creates a shared bus that
 * provides a virtual root for such clocks to hide and synchronize
 * parent PLL re-locking as well as backup operations.
*/

static void tegra21_clk_cbus_init(struct clk *c)
{
	c->state = OFF;
	c->set = true;
}

static int tegra21_clk_cbus_enable(struct clk *c)
{
	return 0;
}

static long tegra21_clk_cbus_round_updown(struct clk *c, unsigned long rate,
					  bool up)
{
	int i;
	const int *millivolts;

	if (!c->dvfs) {
		if (!c->min_rate)
			c->min_rate = c->parent->min_rate;
		rate = max(rate, c->min_rate);
		return rate;
	}

	/* update min now, since no dvfs table was available during init
	   (skip placeholder entries set to 1 kHz) */
	if (!c->min_rate) {
		for (i = 0; i < (c->dvfs->num_freqs - 1); i++) {
			if (c->dvfs->freqs[i] > 1 * c->dvfs->freqs_mult) {
				c->min_rate = c->dvfs->freqs[i];
				break;
			}
		}
		BUG_ON(!c->min_rate);
	}
	rate = max(rate, c->min_rate);

	millivolts = tegra_dvfs_get_millivolts_pll(c->dvfs);
	for (i = 0; ; i++) {
		unsigned long f = c->dvfs->freqs[i];
		int mv = millivolts[i];
		if ((f >= rate) || (mv >= c->dvfs->max_millivolts) ||
		    ((i + 1) >=  c->dvfs->num_freqs)) {
			if (!up && i && (f > rate))
				i--;
			break;
		}
	}
	return c->dvfs->freqs[i];
}

static long tegra21_clk_cbus_round_rate(struct clk *c, unsigned long rate)
{
	return tegra21_clk_cbus_round_updown(c, rate, true);
}

static int cbus_switch_one(struct clk *c, struct clk *p, u32 div, bool abort)
{
	int ret = 0;

	/* set new divider if it is bigger than the current one */
	if (c->div < c->mul * div) {
		ret = clk_set_div(c, div);
		if (ret) {
			pr_err("%s: failed to set %s clock divider %u: %d\n",
			       __func__, c->name, div, ret);
			if (abort)
				return ret;
		}
	}

	if (c->parent != p) {
		ret = clk_set_parent(c, p);
		if (ret) {
			pr_err("%s: failed to set %s clock parent %s: %d\n",
			       __func__, c->name, p->name, ret);
			if (abort)
				return ret;
		}
	}

	/* set new divider if it is smaller than the current one */
	if (c->div > c->mul * div) {
		ret = clk_set_div(c, div);
		if (ret)
			pr_err("%s: failed to set %s clock divider %u: %d\n",
			       __func__, c->name, div, ret);
	}

	return ret;
}

static int cbus_backup(struct clk *c)
{
	int ret;
	struct clk *user;

	list_for_each_entry(user, &c->shared_bus_list,
			u.shared_bus_user.node) {
		struct clk *client = user->u.shared_bus_user.client;
		if (client && (client->state == ON) &&
		    (client->parent == c->parent)) {
			ret = cbus_switch_one(
				client, c->shared_bus_backup.input,
				c->shared_bus_backup.value, true);
			if (ret)
				return ret;
		}
	}
	return 0;
}

static int cbus_dvfs_set_rate(struct clk *c, unsigned long rate)
{
	int ret;
	struct clk *user;

	list_for_each_entry(user, &c->shared_bus_list,
			u.shared_bus_user.node) {
		struct clk *client =  user->u.shared_bus_user.client;
		if (client && client->refcnt && (client->parent == c->parent)) {
			ret = tegra_dvfs_set_rate(c, rate);
			if (ret)
				return ret;
		}
	}
	return 0;
}

static void cbus_restore(struct clk *c)
{
	struct clk *user;

	list_for_each_entry(user, &c->shared_bus_list,
			u.shared_bus_user.node) {
		struct clk *client = user->u.shared_bus_user.client;
		if (client)
			cbus_switch_one(client, c->parent, c->div, false);
	}
}

static void cbus_skip(struct clk *c, unsigned long bus_rate)
{
	struct clk *user;
	unsigned long rate;

	list_for_each_entry(user, &c->shared_bus_list,
			u.shared_bus_user.node) {
		struct clk *client = user->u.shared_bus_user.client;
		if (client && client->skipper &&
		    user->u.shared_bus_user.enabled) {
			/* Make sure skipper output is above the target */
			rate = user->u.shared_bus_user.rate;
			rate += bus_rate >> SUPER_SKIPPER_TERM_SIZE;

			clk_set_rate(client->skipper, rate);
			user->div = client->skipper->div;
			user->mul = client->skipper->mul;
		}
	}
}

static int get_next_backup_div(struct clk *c, unsigned long rate)
{
	u32 div = c->div;
	unsigned long backup_rate = clk_get_rate(c->shared_bus_backup.input);

	rate = max(rate, clk_get_rate_locked(c));
	rate = rate - (rate >> 2);	/* 25% margin for backup rate */
	if ((u64)rate * div < backup_rate)
		div = DIV_ROUND_UP(backup_rate, rate);

	BUG_ON(!div);
	return div;
}

static int tegra21_clk_cbus_set_rate(struct clk *c, unsigned long rate)
{
	int ret;
	bool dramp;

	if (rate == 0)
		return 0;

	ret = clk_enable(c->parent);
	if (ret) {
		pr_err("%s: failed to enable %s clock: %d\n",
		       __func__, c->name, ret);
		return ret;
	}

	dramp = tegra21_is_dyn_ramp(c->parent, rate * c->div, false);
	if (!dramp) {
		c->shared_bus_backup.value = get_next_backup_div(c, rate);
		ret = cbus_backup(c);
		if (ret)
			goto out;
	}

	ret = clk_set_rate(c->parent, rate * c->div);
	if (ret) {
		pr_err("%s: failed to set %s clock rate %lu: %d\n",
		       __func__, c->name, rate, ret);
		goto out;
	}

	/* Safe voltage setting is taken care of by cbus clock dvfs; the call
	 * below only records requirements for each enabled client.
	 */
	if (dramp)
		ret = cbus_dvfs_set_rate(c, rate);

	cbus_restore(c);
	cbus_skip(c, rate);

out:
	clk_disable(c->parent);
	return ret;
}

static inline void cbus_move_enabled_user(
	struct clk *user, struct clk *dst, struct clk *src)
{
	clk_enable(dst);
	list_move_tail(&user->u.shared_bus_user.node, &dst->shared_bus_list);
	clk_disable(src);
	clk_reparent(user, dst);
}

#ifdef CONFIG_TEGRA_DYNAMIC_CBUS
static int tegra21_clk_cbus_update(struct clk *bus)
{
	int ret, mv;
	struct clk *slow = NULL;
	struct clk *top = NULL;
	unsigned long rate;
	unsigned long old_rate;
	unsigned long ceiling;

	if (detach_shared_bus)
		return 0;

	rate = tegra21_clk_shared_bus_update(bus, &top, &slow, &ceiling);

	/* use dvfs table of the slowest enabled client as cbus dvfs table */
	if (bus->dvfs && slow && (slow != bus->u.cbus.slow_user)) {
		unsigned long *dest = &bus->dvfs->freqs[0];
		unsigned long *src =
			&slow->u.shared_bus_user.client->dvfs->freqs[0];
		memcpy(dest, src, sizeof(*dest) * bus->dvfs->num_freqs);
	}

	/* update bus state variables and rate */
	bus->u.cbus.slow_user = slow;
	bus->u.cbus.top_user = top;

	rate = tegra21_clk_cap_shared_bus(bus, rate, ceiling);
	mv = tegra_dvfs_predict_millivolts(bus, rate);
	if (IS_ERR_VALUE(mv))
		return -EINVAL;

	if (bus->dvfs) {
		mv -= bus->dvfs->cur_millivolts;
		if (bus->refcnt && (mv > 0)) {
			ret = tegra_dvfs_set_rate(bus, rate);
			if (ret)
				return ret;
		}
	}

	old_rate = clk_get_rate_locked(bus);
	if (IS_ENABLED(CONFIG_TEGRA_MIGRATE_CBUS_USERS) || (old_rate != rate)) {
		ret = bus->ops->set_rate(bus, rate);
		if (ret)
			return ret;
	} else {
		/* Skippers may change even if bus rate is the same */
		cbus_skip(bus, rate);
	}

	if (bus->dvfs) {
		if (bus->refcnt && (mv <= 0)) {
			ret = tegra_dvfs_set_rate(bus, rate);
			if (ret)
				return ret;
		}
	}

	clk_rate_change_notify(bus, rate);
	return 0;
};
#else
static int tegra21_clk_cbus_update(struct clk *bus)
{
	unsigned long rate, old_rate;

	if (detach_shared_bus)
		return 0;

	rate = tegra21_clk_shared_bus_update(bus, NULL, NULL, NULL);

	old_rate = clk_get_rate_locked(bus);
	if (rate == old_rate) {
		/* Skippers may change even if bus rate is the same */
		cbus_skip(bus, rate);
		return 0;
	}

	return clk_set_rate_locked(bus, rate);
}
#endif

static int tegra21_clk_cbus_migrate_users(struct clk *user)
{
#ifdef CONFIG_TEGRA_MIGRATE_CBUS_USERS
	struct clk *src_bus, *dst_bus, *top_user, *c;
	struct list_head *pos, *n;

	if (!user->u.shared_bus_user.client || !user->inputs)
		return 0;

	/* Dual cbus on Tegra12 */
	src_bus = user->inputs[0].input;
	dst_bus = user->inputs[1].input;

	if (!src_bus->u.cbus.top_user && !dst_bus->u.cbus.top_user)
		return 0;

	/* Make sure top user on the source bus is requesting highest rate */
	if (!src_bus->u.cbus.top_user || (dst_bus->u.cbus.top_user &&
		bus_user_request_is_lower(src_bus->u.cbus.top_user,
					   dst_bus->u.cbus.top_user)))
		swap(src_bus, dst_bus);

	/* If top user is the slow one on its own (source) bus, do nothing */
	top_user = src_bus->u.cbus.top_user;
	BUG_ON(!top_user->u.shared_bus_user.client);
	if (!bus_user_is_slower(src_bus->u.cbus.slow_user, top_user))
		return 0;

	/* If source bus top user is slower than all users on destination bus,
	   move top user; otherwise move all users slower than the top one */
	if (!dst_bus->u.cbus.slow_user ||
	    !bus_user_is_slower(dst_bus->u.cbus.slow_user, top_user)) {
		cbus_move_enabled_user(top_user, dst_bus, src_bus);
	} else {
		list_for_each_safe(pos, n, &src_bus->shared_bus_list) {
			c = list_entry(pos, struct clk, u.shared_bus_user.node);
			if (c->u.shared_bus_user.enabled &&
			    c->u.shared_bus_user.client &&
			    bus_user_is_slower(c, top_user))
				cbus_move_enabled_user(c, dst_bus, src_bus);
		}
	}

	/* Update destination bus 1st (move clients), then source */
	tegra_clk_shared_bus_update(dst_bus);
	tegra_clk_shared_bus_update(src_bus);
#endif
	return 0;
}

static struct clk_ops tegra_clk_cbus_ops = {
	.init = tegra21_clk_cbus_init,
	.enable = tegra21_clk_cbus_enable,
	.set_rate = tegra21_clk_cbus_set_rate,
	.round_rate = tegra21_clk_cbus_round_rate,
	.round_rate_updown = tegra21_clk_cbus_round_updown,
	.shared_bus_update = tegra21_clk_cbus_update,
};

/* shared bus ops */
/*
 * Some clocks may have multiple downstream users that need to request a
 * higher clock rate.  Shared bus clocks provide a unique shared_bus_user
 * clock to each user.  The frequency of the bus is set to the highest
 * enabled shared_bus_user clock, with a minimum value set by the
 * shared bus.
 *
 * Optionally shared bus may support users migration. Since shared bus and
 * its * children (users) have reversed rate relations: user rates determine
 * bus rate, * switching user from one parent/bus to another may change rates
 * of both parents. Therefore we need a cross-bus lock on top of individual
 * user and bus locks. For now, limit bus switch support to cbus only if
 * CONFIG_TEGRA_MIGRATE_CBUS_USERS is set.
 */

static unsigned long tegra21_clk_shared_bus_update(struct clk *bus,
	struct clk **bus_top, struct clk **bus_slow, unsigned long *rate_cap)
{
	struct clk *c;
	struct clk *slow = NULL;
	struct clk *top = NULL;

	unsigned long override_rate = 0;
	unsigned long top_rate = 0;
	unsigned long rate = bus->min_rate;
	unsigned long bw = 0;
	unsigned long iso_bw = 0;
	unsigned long ceiling = bus->max_rate;
	unsigned long ceiling_but_iso = bus->max_rate;
	u32 usage_flags = 0;
	bool rate_set = false;

	list_for_each_entry(c, &bus->shared_bus_list,
			u.shared_bus_user.node) {
		bool cap_user = (c->u.shared_bus_user.mode == SHARED_CEILING) ||
			(c->u.shared_bus_user.mode == SHARED_CEILING_BUT_ISO);
		/*
		 * Ignore requests from disabled floor and bw users, and from
		 * auto-users riding the bus. Always honor ceiling users, even
		 * if they are disabled - we do not want to keep enabled parent
		 * bus just because ceiling is set.
		 */
		if (c->u.shared_bus_user.enabled || cap_user) {
			unsigned long request_rate = c->u.shared_bus_user.rate;
			usage_flags |= c->u.shared_bus_user.usage_flag;

			if (!(c->flags & BUS_RATE_LIMIT))
				rate_set = true;

			switch (c->u.shared_bus_user.mode) {
			case SHARED_ISO_BW:
				iso_bw += request_rate;
				if (iso_bw > bus->max_rate)
					iso_bw = bus->max_rate;
				/* fall thru */
			case SHARED_BW:
				bw += request_rate;
				if (bw > bus->max_rate)
					bw = bus->max_rate;
				break;
			case SHARED_CEILING_BUT_ISO:
				ceiling_but_iso =
					min(request_rate, ceiling_but_iso);
				break;
			case SHARED_CEILING:
				ceiling = min(request_rate, ceiling);
				break;
			case SHARED_OVERRIDE:
				if (override_rate == 0)
					override_rate = request_rate;
				break;
			case SHARED_AUTO:
				break;
			case SHARED_FLOOR:
			default:
				rate = max(request_rate, rate);
				if (c->u.shared_bus_user.client
							&& request_rate) {
					if (top_rate < request_rate) {
						top_rate = request_rate;
						top = c;
					} else if ((top_rate == request_rate) &&
						bus_user_is_slower(c, top)) {
						top = c;
					}
				}
			}
			if (c->u.shared_bus_user.client &&
				(!slow || bus_user_is_slower(c, slow)))
				slow = c;
		}
	}

	if (bus->flags & PERIPH_EMC_ENB) {
		unsigned long iso_bw_min;
		bw = tegra_emc_apply_efficiency(
			bw, iso_bw, bus->max_rate, usage_flags, &iso_bw_min);
		if (bus->ops && bus->ops->round_rate)
			iso_bw_min = bus->ops->round_rate(bus, iso_bw_min);
		ceiling_but_iso = max(ceiling_but_iso, iso_bw_min);
	}

	rate = override_rate ? : max(rate, bw);
	ceiling = min(ceiling, ceiling_but_iso);
	ceiling = override_rate ? bus->max_rate : ceiling;
	bus->override_rate = override_rate;

	if (bus_top && bus_slow && rate_cap) {
		/* If dynamic bus dvfs table, let the caller to complete
		   rounding and aggregation */
		*bus_top = top;
		*bus_slow = slow;
		*rate_cap = ceiling;
	} else {
		/*
		 * If satic bus dvfs table, complete rounding and aggregation.
		 * In case when no user requested bus rate, and bus retention
		 * is enabled, don't scale down - keep current rate.
		 */
		if (!rate_set && (bus->shared_bus_flags & SHARED_BUS_RETENTION))
			rate = clk_get_rate_locked(bus);

		rate = tegra21_clk_cap_shared_bus(bus, rate, ceiling);
	}

	return rate;
};

static unsigned long tegra21_clk_cap_shared_bus(struct clk *bus,
	unsigned long rate, unsigned long ceiling)
{
	if (bus->ops && bus->ops->round_rate_updown)
		ceiling = bus->ops->round_rate_updown(bus, ceiling, false);

	rate = min(rate, ceiling);

	if (bus->ops && bus->ops->round_rate)
		rate = bus->ops->round_rate(bus, rate);

	return rate;
}

static int tegra_clk_shared_bus_migrate_users(struct clk *user)
{
	if (detach_shared_bus)
		return 0;

	/* Only cbus migration is supported */
	if (user->flags & PERIPH_ON_CBUS)
		return tegra21_clk_cbus_migrate_users(user);
	return -ENOSYS;
}

static void tegra_clk_shared_bus_user_init(struct clk *c)
{
	c->max_rate = c->parent->max_rate;
	c->u.shared_bus_user.rate = c->parent->max_rate;
	c->state = OFF;
	c->set = true;

	if ((c->u.shared_bus_user.mode == SHARED_CEILING) ||
	    (c->u.shared_bus_user.mode == SHARED_CEILING_BUT_ISO)) {
		c->state = ON;
		c->refcnt++;
	}

	if (c->u.shared_bus_user.client_id) {
		c->u.shared_bus_user.client =
			tegra_get_clock_by_name(c->u.shared_bus_user.client_id);
		if (!c->u.shared_bus_user.client) {
			pr_err("%s: could not find clk %s\n", __func__,
			       c->u.shared_bus_user.client_id);
			return;
		}
		c->u.shared_bus_user.client->flags |=
			c->parent->flags & PERIPH_ON_CBUS;
		c->flags |= c->parent->flags & PERIPH_ON_CBUS;
		c->div = 1;
		c->mul = 1;
	}

	list_add_tail(&c->u.shared_bus_user.node,
		&c->parent->shared_bus_list);
}

static int tegra_clk_shared_bus_user_set_parent(struct clk *c, struct clk *p)
{
	int ret;
	const struct clk_mux_sel *sel;

	if (detach_shared_bus)
		return 0;

	if (c->parent == p)
		return 0;

	if (!(c->inputs && c->cross_clk_mutex && clk_cansleep(c)))
		return -ENOSYS;

	for (sel = c->inputs; sel->input != NULL; sel++) {
		if (sel->input == p)
			break;
	}
	if (!sel->input)
		return -EINVAL;

	if (c->refcnt)
		clk_enable(p);

	list_move_tail(&c->u.shared_bus_user.node, &p->shared_bus_list);
	ret = tegra_clk_shared_bus_update(p);
	if (ret) {
		list_move_tail(&c->u.shared_bus_user.node,
			       &c->parent->shared_bus_list);
		tegra_clk_shared_bus_update(c->parent);
		clk_disable(p);
		return ret;
	}

	tegra_clk_shared_bus_update(c->parent);

	if (c->refcnt)
		clk_disable(c->parent);

	clk_reparent(c, p);

	return 0;
}

static int tegra_clk_shared_bus_user_set_rate(struct clk *c, unsigned long rate)
{
	int ret;

	c->u.shared_bus_user.rate = rate;
	ret = tegra_clk_shared_bus_update(c->parent);

	if (!ret && c->cross_clk_mutex && clk_cansleep(c))
		tegra_clk_shared_bus_migrate_users(c);

	return ret;
}

static long tegra_clk_shared_bus_user_round_rate(
	struct clk *c, unsigned long rate)
{
	/*
	 * Defer rounding requests until aggregated. BW users must not be
	 * rounded at all, others just clipped to bus range (some clients
	 * may use round api to find limits).
	 */

	if ((c->u.shared_bus_user.mode != SHARED_BW) &&
	    (c->u.shared_bus_user.mode != SHARED_ISO_BW)) {
		if (rate > c->parent->max_rate) {
			rate = c->parent->max_rate;
		} else {
			/* Skippers allow to run below bus minimum */
			struct clk *client = c->u.shared_bus_user.client;
			int skip = (client && client->skipper) ?
				SUPER_SKIPPER_TERM_SIZE : 0;
			unsigned long min_rate = c->parent->min_rate >> skip;

			if (rate < min_rate)
				rate = min_rate;
		}
	}
	return rate;
}

static int tegra_clk_shared_bus_user_enable(struct clk *c)
{
	int ret;

	c->u.shared_bus_user.enabled = true;
	ret = tegra_clk_shared_bus_update(c->parent);
	if (!ret && c->u.shared_bus_user.client)
		ret = clk_enable(c->u.shared_bus_user.client);

	if (!ret && c->cross_clk_mutex && clk_cansleep(c))
		tegra_clk_shared_bus_migrate_users(c);

	return ret;
}

static void tegra_clk_shared_bus_user_disable(struct clk *c)
{
	if (c->u.shared_bus_user.client)
		clk_disable(c->u.shared_bus_user.client);
	c->u.shared_bus_user.enabled = false;
	tegra_clk_shared_bus_update(c->parent);

	if (c->cross_clk_mutex && clk_cansleep(c))
		tegra_clk_shared_bus_migrate_users(c);
}

static void tegra_clk_shared_bus_user_reset(struct clk *c, bool assert)
{
	if (c->u.shared_bus_user.client) {
		if (c->u.shared_bus_user.client->ops &&
		    c->u.shared_bus_user.client->ops->reset)
			c->u.shared_bus_user.client->ops->reset(
				c->u.shared_bus_user.client, assert);
	}
}

static struct clk_ops tegra_clk_shared_bus_user_ops = {
	.init = tegra_clk_shared_bus_user_init,
	.enable = tegra_clk_shared_bus_user_enable,
	.disable = tegra_clk_shared_bus_user_disable,
	.set_parent = tegra_clk_shared_bus_user_set_parent,
	.set_rate = tegra_clk_shared_bus_user_set_rate,
	.round_rate = tegra_clk_shared_bus_user_round_rate,
	.reset = tegra_clk_shared_bus_user_reset,
};

/* shared bus connector ops (user/bus connector to cascade shared buses) */
static int tegra21_clk_shared_connector_update(struct clk *bus)
{
	unsigned long rate, old_rate;

	if (detach_shared_bus)
		return 0;

	rate = tegra21_clk_shared_bus_update(bus, NULL, NULL, NULL);

	old_rate = clk_get_rate_locked(bus);
	if (rate == old_rate)
		return 0;

	return clk_set_rate_locked(bus, rate);
}

static struct clk_ops tegra_clk_shared_connector_ops = {
	.init = tegra_clk_shared_bus_user_init,
	.enable = tegra_clk_shared_bus_user_enable,
	.disable = tegra_clk_shared_bus_user_disable,
	.set_parent = tegra_clk_shared_bus_user_set_parent,
	.set_rate = tegra_clk_shared_bus_user_set_rate,
	.round_rate = tegra_clk_shared_bus_user_round_rate,
	.reset = tegra_clk_shared_bus_user_reset,
	.shared_bus_update = tegra21_clk_shared_connector_update,
};

/* coupled gate ops */
/*
 * Some clocks may have common enable/disable control, but run at different
 * rates, and have different dvfs tables. Coupled gate clock synchronize
 * enable/disable operations for such clocks.
 */

static int tegra21_clk_coupled_gate_enable(struct clk *c)
{
	int ret;
	const struct clk_mux_sel *sel;

	BUG_ON(!c->inputs);
	pr_debug("%s on clock %s\n", __func__, c->name);

	for (sel = c->inputs; sel->input != NULL; sel++) {
		if (sel->input == c->parent)
			continue;

		ret = clk_enable(sel->input);
		if (ret) {
			while (sel != c->inputs) {
				sel--;
				if (sel->input == c->parent)
					continue;
				clk_disable(sel->input);
			}
			return ret;
		}
	}

	return tegra21_periph_clk_enable(c);
}

static void tegra21_clk_coupled_gate_disable(struct clk *c)
{
	const struct clk_mux_sel *sel;

	BUG_ON(!c->inputs);
	pr_debug("%s on clock %s\n", __func__, c->name);

	tegra21_periph_clk_disable(c);

	if (!c->refcnt)	/* happens only on boot clean-up: don't propagate */
		return;

	for (sel = c->inputs; sel->input != NULL; sel++) {
		if (sel->input == c->parent)
			continue;

		if (sel->input->set)	/* enforce coupling after boot only */
			clk_disable(sel->input);
	}
}

/*
 * AHB and APB shared bus operations
 * APB shared bus is a user of AHB shared bus
 * AHB shared bus is a user of SCLK complex shared bus
 * SCLK/AHB and AHB/APB dividers can be dynamically changed. When AHB and APB
 * users requests are propagated to SBUS target rate, current values of the
 * dividers are ignored, and flat maximum request is selected as SCLK bus final
 * target. Then the dividers will be re-evaluated, based on AHB and APB targets.
 * Both AHB and APB buses are always enabled.
 */
static void tegra21_clk_ahb_apb_init(struct clk *c, struct clk *bus_clk)
{
	tegra_clk_shared_bus_user_init(c);
	c->max_rate = bus_clk->max_rate;
	c->min_rate = bus_clk->min_rate;
	c->mul = bus_clk->mul;
	c->div = bus_clk->div;

	c->u.shared_bus_user.rate = clk_get_rate(bus_clk);
	c->u.shared_bus_user.enabled = true;
	c->parent->child_bus = c;
}

static void tegra21_clk_ahb_init(struct clk *c)
{
	struct clk *bus_clk = c->parent->u.system.hclk;
	tegra21_clk_ahb_apb_init(c, bus_clk);
}

static void tegra21_clk_apb_init(struct clk *c)
{
	struct clk *bus_clk = c->parent->parent->u.system.pclk;
	tegra21_clk_ahb_apb_init(c, bus_clk);
}

static int tegra21_clk_ahb_apb_update(struct clk *bus)
{
	unsigned long rate;

	if (detach_shared_bus)
		return 0;

	rate = tegra21_clk_shared_bus_update(bus, NULL, NULL, NULL);
	return clk_set_rate_locked(bus, rate);
}

static struct clk_ops tegra_clk_ahb_ops = {
	.init = tegra21_clk_ahb_init,
	.set_rate = tegra_clk_shared_bus_user_set_rate,
	.round_rate = tegra_clk_shared_bus_user_round_rate,
	.shared_bus_update = tegra21_clk_ahb_apb_update,
};

static struct clk_ops tegra_clk_apb_ops = {
	.init = tegra21_clk_apb_init,
	.set_rate = tegra_clk_shared_bus_user_set_rate,
	.round_rate = tegra_clk_shared_bus_user_round_rate,
	.shared_bus_update = tegra21_clk_ahb_apb_update,
};

static struct clk_ops tegra_clk_coupled_gate_ops = {
	.init			= tegra21_periph_clk_init,
	.enable			= tegra21_clk_coupled_gate_enable,
	.disable		= tegra21_clk_coupled_gate_disable,
	.reset			= &tegra21_periph_clk_reset,
};


/* Clock definitions */
static struct clk tegra_clk_32k = {
	.name = "clk_32k",
	.rate = 32768,
	.ops  = NULL,
	.max_rate = 32768,
};

static struct clk tegra_clk_osc = {
	.name      = "osc",
	.flags     = ENABLE_ON_INIT,
	.ops       = &tegra_osc_ops,
	.max_rate  = 48000000,
};

static struct clk tegra_clk_m = {
	.name      = "clk_m",
	.flags     = ENABLE_ON_INIT,
	.parent    = &tegra_clk_osc,
	.ops       = &tegra_clk_m_ops,
	.max_rate  = 48000000,
};

static struct clk tegra_clk_m_div2 = {
	.name      = "clk_m_div2",
	.ops       = &tegra_clk_m_div_ops,
	.parent    = &tegra_clk_m,
	.mul       = 1,
	.div       = 2,
	.state     = ON,
	.max_rate  = 24000000,
};

static struct clk tegra_clk_m_div4 = {
	.name      = "clk_m_div4",
	.ops       = &tegra_clk_m_div_ops,
	.parent    = &tegra_clk_m,
	.mul       = 1,
	.div       = 4,
	.state     = ON,
	.max_rate  = 12000000,
};

static struct clk tegra_pll_ref = {
	.name      = "pll_ref",
	.flags     = ENABLE_ON_INIT,
	.ops       = &tegra_pll_ref_ops,
	.parent    = &tegra_clk_osc,
	.max_rate  = 38400000,
};

static struct clk_pll_freq_table tegra_pll_cx_freq_table[] = {
	{ 12000000, 510000000,  85, 1, 2},
	{ 13000000, 510000000,  78, 1, 2},	/* actual: 507.0 MHz */
	{ 38400000, 510000000,  53, 2, 2},	/* actual: 508.8 MHz */
	{ 0, 0, 0, 0, 0, 0 },
};

struct clk_pll_controls pllcx_controls = {
	.enable_mask = PLL_BASE_ENABLE,
	.bypass_mask = PLL_BASE_BYPASS,
	.reset_mask = PLLCX_MISC0_RESET,
	.reset_reg_idx = PLL_MISC0_IDX,
	.iddq_mask = PLLCX_MISC1_IDDQ,
	.iddq_reg_idx = PLL_MISC1_IDX,
	.lock_mask = PLLCX_BASE_LOCK,
	.lock_reg_idx = PLL_BASE_IDX,
};

static struct clk_pll_div_layout pllcx_div_layout = {
	.mdiv_shift = 0,
	.mdiv_mask = 0xff,
	.ndiv_shift = 10,
	.ndiv_mask = 0xff << 10,
	.pdiv_shift = 20,
	.pdiv_mask = 0x1f << 20,
	.pdiv_to_p = pll_qlin_pdiv_to_p,
	.pdiv_max = PLL_QLIN_PDIV_MAX,
};

static struct clk tegra_pll_c = {
	.name      = "pll_c",
	.ops       = &tegra_pllcx_ops,
	.reg       = 0x80,
	.parent    = &tegra_pll_ref,
	.max_rate  = 1200000000,
	.u.pll = {
		.input_min = 12000000,
		.input_max = 700000000,
		.cf_min    = 12000000,
		.cf_max    = 50000000,
		.vco_min   = 600000000,
		.vco_max   = 1200000000,
		.freq_table = tegra_pll_cx_freq_table,
		.lock_delay = 300,
		.misc0 = 0x88 - 0x80,
		.misc1 = 0x8c - 0x80,
		.misc2 = 0x5d0 - 0x80,
		.misc3 = 0x5d4 - 0x80,
		.controls = &pllcx_controls,
		.div_layout = &pllcx_div_layout,
		.round_p_to_pdiv = pll_qlin_p_to_pdiv,
		.dyn_ramp = pllcx_dyn_ramp,
		.set_defaults = pllcx_set_defaults,
	},
};

static struct clk tegra_pll_c_out1 = {
	.name      = "pll_c_out1",
	.ops       = &tegra_pll_div_ops,
	.flags     = DIV_U71 | PERIPH_ON_CBUS,
	.parent    = &tegra_pll_c,
	.reg       = 0x84,
	.reg_shift = 0,
	.max_rate  = 700000000,
};

static struct clk tegra_pll_c2 = {
	.name      = "pll_c2",
	.ops       = &tegra_pllcx_ops,
	.reg       = 0x4e8,
	.parent    = &tegra_pll_ref,
	.max_rate  = 1200000000,
	.u.pll = {
		.input_min = 12000000,
		.input_max = 700000000,
		.cf_min    = 12000000,
		.cf_max    = 50000000,
		.vco_min   = 600000000,
		.vco_max   = 1200000000,
		.freq_table = tegra_pll_cx_freq_table,
		.lock_delay = 300,
		.misc0 = 0x4ec - 0x4e8,
		.misc1 = 0x4f0 - 0x4e8,
		.misc2 = 0x4f4 - 0x4e8,
		.misc3 = 0x4f8 - 0x4e8,
		.controls = &pllcx_controls,
		.div_layout = &pllcx_div_layout,
		.round_p_to_pdiv = pll_qlin_p_to_pdiv,
		.dyn_ramp = pllcx_dyn_ramp,
		.set_defaults = pllcx_set_defaults,
	},
};

static struct clk tegra_pll_c3 = {
	.name      = "pll_c3",
	.ops       = &tegra_pllcx_ops,
	.reg       = 0x4fc,
	.parent    = &tegra_pll_ref,
	.max_rate  = 1200000000,
	.u.pll = {
		.input_min = 12000000,
		.input_max = 700000000,
		.cf_min    = 12000000,
		.cf_max    = 50000000,
		.vco_min   = 600000000,
		.vco_max   = 1200000000,
		.freq_table = tegra_pll_cx_freq_table,
		.lock_delay = 300,
		.misc0 = 0x500 - 0x4fc,
		.misc1 = 0x504 - 0x4fc,
		.misc2 = 0x508 - 0x4fc,
		.misc3 = 0x50c - 0x4fc,
		.controls = &pllcx_controls,
		.div_layout = &pllcx_div_layout,
		.round_p_to_pdiv = pll_qlin_p_to_pdiv,
		.dyn_ramp = pllcx_dyn_ramp,
		.set_defaults = pllcx_set_defaults,
	},
};

static struct clk tegra_pll_a1 = {
	.name      = "pll_a1",
	.ops       = &tegra_pllcx_ops,
	.reg       = 0x6a4,
	.parent    = &tegra_pll_ref,
	.max_rate  = 1200000000,
	.u.pll = {
		.input_min = 12000000,
		.input_max = 700000000,
		.cf_min    = 12000000,
		.cf_max    = 50000000,
		.vco_min   = 600000000,
		.vco_max   = 1200000000,
		.freq_table = tegra_pll_cx_freq_table,
		.lock_delay = 300,
		.misc0 = 0x6a8 - 0x6a4,
		.misc1 = 0x6ac - 0x6a4,
		.misc2 = 0x6b0 - 0x6a4,
		.misc3 = 0x6b4 - 0x6a4,
		.controls = &pllcx_controls,
		.div_layout = &pllcx_div_layout,
		.round_p_to_pdiv = pll_qlin_p_to_pdiv,
		.dyn_ramp = pllcx_dyn_ramp,
		.set_defaults = pllcx_set_defaults,
	},
};

static struct clk_pll_freq_table tegra_pll_a_freq_table[] = {
	{ 12000000, 282240000, 46, 1, 2, 1, 4424},	/* actual: 282240234 */
	{ 12000000, 368640000, 60, 1, 2, 1, 7701},	/* actual: 368640381 */
	{ 12000000, 240000000, 60, 1, 3, 1, },

	{ 13000000, 282240000, 42, 1, 2, 1, 7549},	/* actual: 282239807 */
	{ 13000000, 368640000, 55, 1, 2, 1, 9944},	/* actual: 368640137 */
	{ 13000000, 240000000, 55, 1, 3, 1, },		/* actual: 238.3 MHz */

	{ 38400000, 282240000, 28, 2, 2, 1, 7373},	/* actual: 282240234 */
	{ 38400000, 368640000, 37, 2, 2, 1, 7373},	/* actual: 368640234 */
	{ 38400000, 240000000, 50, 2, 4, 1,},
	{ 0, 0, 0, 0, 0, 0 },
};

struct clk_pll_controls plla_controls = {
	.enable_mask = PLL_BASE_ENABLE,
	.bypass_mask = PLL_BASE_BYPASS,
	.iddq_mask = PLLA_BASE_IDDQ,
	.iddq_reg_idx = PLL_BASE_IDX,
	.lock_mask = PLLA_BASE_LOCK,
	.lock_reg_idx = PLL_BASE_IDX,

	.sdm_en_mask = PLLA_MISC2_EN_SDM,
	.sdm_ctrl_reg_idx = PLL_MISC2_IDX,
};

static struct clk_pll_div_layout plla_div_layout = {
	.mdiv_shift = 0,
	.mdiv_mask = 0xff,
	.ndiv_shift = 8,
	.ndiv_mask = 0xff << 8,
	.pdiv_shift = 20,
	.pdiv_mask = 0x1f << 20,
	.pdiv_to_p = pll_qlin_pdiv_to_p,
	.pdiv_max = PLL_QLIN_PDIV_MAX,

	.sdm_din_shift = 0,
	.sdm_din_mask = 0xffff,
	.sdm_din_reg_idx = PLL_MISC1_IDX,
};

static struct clk tegra_pll_a = {
	.name      = "pll_a",
	.ops       = &tegra_plla_ops,
	.reg       = 0xb0,
	.parent    = &tegra_pll_ref,
	.max_rate  = 1000000000,
	.u.pll = {
		.input_min = 12000000,
		.input_max = 800000000,
		.cf_min    = 12000000,
		.cf_max    = 19200000,
		.vco_min   = 500000000,
		.vco_max   = 1000000000,
		.freq_table = tegra_pll_a_freq_table,
		.lock_delay = 300,
		.misc0 = 0xbc - 0xb0,
		.misc1 = 0xb8 - 0xb0,
		.misc2 = 0x5d8 - 0xb0,
		.controls = &plla_controls,
		.div_layout = &plla_div_layout,
		.round_p_to_pdiv = pll_qlin_p_to_pdiv,
		.set_defaults = plla_set_defaults,
	},
};

static struct clk tegra_pll_a_out0 = {
	.name      = "pll_a_out0",
	.ops       = &tegra_pll_div_ops,
	.flags     = DIV_U71,
	.parent    = &tegra_pll_a,
	.reg       = 0xb4,
	.reg_shift = 0,
	.max_rate  = 100000000,
};

static struct clk_pll_freq_table tegra_pll_d_freq_table[] = {
	{ 12000000, 594000000,  99, 1, 2},
	{ 13000000, 594000000,  90, 1, 2, 0, 7247},	/* actual: 594000183 */
	{ 38400000, 594000000,  60, 2, 2, 0, 11264},
	{ 0, 0, 0, 0, 0, 0 },
};

struct clk_pll_controls plld_controls = {
	.enable_mask = PLL_BASE_ENABLE,
	.bypass_mask = PLL_BASE_BYPASS,
	.iddq_mask = PLLD_MISC0_IDDQ,
	.iddq_reg_idx = PLL_MISC0_IDX,
	.lock_mask = PLLD_BASE_LOCK,
	.lock_reg_idx = PLL_BASE_IDX,

	.sdm_en_mask = PLLD_MISC0_EN_SDM,
	.sdm_ctrl_reg_idx = PLL_MISC0_IDX,
};

static struct clk_pll_div_layout plld_div_layout = {
	.mdiv_shift = 0,
	.mdiv_mask = 0xff,
	.ndiv_shift = 11,
	.ndiv_mask = 0xff << 11,
	.pdiv_shift = 20,
	.pdiv_mask = 0x7 << 20,
	.pdiv_to_p = pll_expo_pdiv_to_p,
	.pdiv_max = PLL_EXPO_PDIV_MAX,

	.sdm_din_shift = 0,
	.sdm_din_mask = 0xffff,
	.sdm_din_reg_idx = PLL_MISC0_IDX,
};

static struct clk tegra_pll_d = {
	.name      = "pll_d",
	.flags     = PLLD,
	.ops       = &tegra_plld_ops,
	.reg       = 0xd0,
	.parent    = &tegra_pll_ref,
	.max_rate  = 1500000000,
	.u.pll = {
		.input_min = 12000000,
		.input_max = 800000000,
		.cf_min    = 12000000,
		.cf_max    = 38400000,
		.vco_min   = 750000000,
		.vco_max   = 1500000000,
		.freq_table = tegra_pll_d_freq_table,
		.lock_delay = 300,
		.misc0 = 0xdc - 0xd0,
		.misc1 = 0xd8 - 0xd0,
		.controls = &plld_controls,
		.div_layout = &plld_div_layout,
		.round_p_to_pdiv = pll_expo_p_to_pdiv,
		.set_defaults = plld_set_defaults,
	},
};

static struct clk tegra_pll_d_out0 = {
	.name      = "pll_d_out0",
	.ops       = &tegra_pll_div_ops,
	.flags     = DIV_2 | PLLD,
	.parent    = &tegra_pll_d,
	.max_rate  = 750000000,
};

static struct clk_pll_freq_table tegra_pll_d2_freq_table[] = {
	{ 12000000, 594000000,  99, 1, 2},
	{ 13000000, 594000000,  90, 1, 2, 0, 7247},	/* actual: 594000183 */
	{ 38400000, 594000000,  60, 2, 2, 0, 11264},
	{ 0, 0, 0, 0, 0, 0 },
};

struct clk_pll_controls plld2_controls = {
	.enable_mask = PLL_BASE_ENABLE,
	.bypass_mask = PLL_BASE_BYPASS,
	.iddq_mask = PLLDSS_BASE_IDDQ,
	.iddq_reg_idx = PLL_BASE_IDX,
	.lock_mask = PLLDSS_BASE_LOCK,
	.lock_reg_idx = PLL_BASE_IDX,

	.sdm_en_mask = PLLDSS_MISC1_CFG_EN_SDM,
	.ssc_en_mask = PLLDSS_MISC1_CFG_EN_SSC,
	.sdm_ctrl_reg_idx = PLL_MISC1_IDX,
};

static struct clk_pll_div_layout plldss_div_layout = {
	.mdiv_shift = 0,
	.mdiv_mask = 0xff,
	.ndiv_shift = 8,
	.ndiv_mask = 0xff << 8,
	.pdiv_shift = 19,
	.pdiv_mask = 0x1f << 19,
	.pdiv_to_p = pll_qlin_pdiv_to_p,
	.pdiv_max = PLL_QLIN_PDIV_MAX,

	.sdm_din_shift = 0,
	.sdm_din_mask = 0xffff,
	.sdm_din_reg_idx = PLL_MISC3_IDX,
};

static struct clk tegra_pll_d2 = {
	.name      = "pll_d2",
	.ops       = &tegra_plld2_ops,
	.reg       = 0x4b8,
	.parent    = &tegra_pll_ref,	/* s/w policy, always tegra_pll_ref */
	.max_rate  = 1500000000,
	.u.pll = {
		.input_min = 12000000,
		.input_max = 800000000,
		.cf_min    = 12000000,
		.cf_max    = 38400000,
		.vco_min   = 750000000,
		.vco_max   = 1500000000,
		.freq_table = tegra_pll_d2_freq_table,
		.lock_delay = 300,
		.misc0 = 0x4bc - 0x4b8,
		.misc1 = 0x570 - 0x4b8,
		.misc2 = 0x574 - 0x4b8,
		.misc3 = 0x578 - 0x4b8,
		.controls = &plld2_controls,
		.div_layout = &plldss_div_layout,
		.round_p_to_pdiv = pll_qlin_p_to_pdiv,
		.set_defaults = plld2_set_defaults,
	},
};

static struct clk_pll_freq_table tegra_pll_dp_freq_table[] = {
	{ 12000000, 270000000,  90, 1, 4},
	{ 13000000, 270000000,  83, 1, 4},	/* actual: 269.8 MHz */
	{ 38400000, 270000000,  56, 2, 4},	/* actual: 268.8 MHz */
	{ 0, 0, 0, 0, 0, 0 },
};

struct clk_pll_controls plldp_controls = {
	.enable_mask = PLL_BASE_ENABLE,
	.bypass_mask = PLL_BASE_BYPASS,
	.iddq_mask = PLLDSS_BASE_IDDQ,
	.iddq_reg_idx = PLL_BASE_IDX,
	.lock_mask = PLLDSS_BASE_LOCK,
	.lock_reg_idx = PLL_BASE_IDX,

	.sdm_en_mask = PLLDSS_MISC1_CFG_EN_SDM,
	.ssc_en_mask = PLLDSS_MISC1_CFG_EN_SSC,
	.sdm_ctrl_reg_idx = PLL_MISC1_IDX,
};

static struct clk tegra_pll_dp = {
	.name      = "pll_dp",
	.ops       = &tegra_plldp_ops,
	.reg       = 0x590,
	.parent    = &tegra_pll_ref,	/* s/w policy, always tegra_pll_ref */
	.max_rate  = 1500000000,
	.u.pll = {
		.input_min = 12000000,
		.input_max = 800000000,
		.cf_min    = 12000000,
		.cf_max    = 38400000,
		.vco_min   = 750000000,
		.vco_max   = 1500000000,
		.freq_table = tegra_pll_dp_freq_table,
		.lock_delay = 300,
		.misc0 = 0x594 - 0x590,
		.misc1 = 0x598 - 0x590,
		.misc2 = 0x59c - 0x590,
		.misc3 = 0x5a0 - 0x590,
		.controls = &plldp_controls,
		.div_layout = &plldss_div_layout,
		.round_p_to_pdiv = pll_qlin_p_to_pdiv,
		.set_defaults = plldp_set_defaults,
	},
};

static struct clk_pll_freq_table tegra_pllc4_vco_freq_table[] = {
	{ 12000000, 600000000,  50, 1, 1},
	{ 13000000, 600000000,  46, 1, 1},	/* actual: 598.0 MHz */
	{ 38400000, 600000000,  31, 2, 1},	/* actual: 595.2 MHz */
	{ 0, 0, 0, 0, 0, 0 },
};

struct clk_pll_controls pllc4_controls = {
	.enable_mask = PLL_BASE_ENABLE,
	.bypass_mask = PLL_BASE_BYPASS,
	.iddq_mask = PLLDSS_BASE_IDDQ,
	.iddq_reg_idx = PLL_BASE_IDX,
	.lock_mask = PLLDSS_BASE_LOCK,
	.lock_reg_idx = PLL_BASE_IDX,
};

static struct clk_pll_div_layout pllc4_div_layout = {
	.mdiv_shift = 0,
	.mdiv_mask = 0xff,
	.ndiv_shift = 8,
	.ndiv_mask = 0xff << 8,
	.pdiv_shift = 19,
	.pdiv_mask = 0x1f << 19,
	.pdiv_to_p = pll_qlin_pdiv_to_p,
	.pdiv_max = PLL_QLIN_PDIV_MAX,
};

static struct clk tegra_pll_c4_vco = {
	.name      = "pll_c4",
	.ops       = &tegra_pllc4_vco_ops,
	.reg       = 0x5a4,
	.parent    = &tegra_pll_ref,	/* s/w policy, always tegra_pll_ref */
	.max_rate  = 1080000000,
	.u.pll = {
		.input_min = 9600000,
		.input_max = 800000000,
		.cf_min    = 9600000,
		.cf_max    = 19200000,
		.vco_min   = 500000000,
		.vco_max   = 1080000000,
		.freq_table = tegra_pllc4_vco_freq_table,
		.lock_delay = 300,
		.misc0 = 0x5a8 - 0x5a4,
		.controls = &pllc4_controls,
		.div_layout = &pllc4_div_layout,
		.round_p_to_pdiv = pll_qlin_p_to_pdiv,
		.set_defaults = pllc4_set_defaults,
		.vco_out = true,
	},
};

static struct clk tegra_pll_c4_out0 = {
	.name      = "pll_c4_out0",
	.ops       = &tegra_pll_out_ops,
	.parent    = &tegra_pll_c4_vco,
};

static struct clk tegra_pll_c4_out1 = {
	.name      = "pll_c4_out1",
	.ops       = &tegra_pll_out_fixed_ops,
	.parent    = &tegra_pll_c4_vco,
	.mul       = 1,
	.div       = 3,
};

static struct clk tegra_pll_c4_out2 = {
	.name      = "pll_c4_out2",
	.ops       = &tegra_pll_out_fixed_ops,
	.parent    = &tegra_pll_c4_vco,
	.mul       = 1,
	.div       = 5,
};

static struct clk tegra_pll_c4_out3 = {
	.name      = "pll_c4_out3",
	.ops       = &tegra_pll_div_ops,
	.flags     = DIV_U71,
	.parent    = &tegra_pll_c4_out0,
	.reg       = 0x5e4,
	.reg_shift = 0,
	.max_rate  = 1080000000,
};

static struct clk_pll_freq_table tegra_pllre_vco_freq_table[] = {
	{ 12000000, 672000000,  56, 1, 1},
	{ 13000000, 672000000,  51, 1, 1},	/* actual: 663.0 MHz */
	{ 38400000, 672000000,  35, 2, 1},
	{ 0, 0, 0, 0, 0, 0 },
};

struct clk_pll_controls pllre_controls = {
	.enable_mask = PLL_BASE_ENABLE,
	.bypass_mask = PLL_BASE_BYPASS,
	.iddq_mask = PLLRE_MISC0_IDDQ,
	.iddq_reg_idx = PLL_MISC0_IDX,
	.lock_mask = PLLRE_MISC0_LOCK,
	.lock_reg_idx = PLL_MISC0_IDX,
};

static struct clk_pll_div_layout pllre_div_layout = {
	.mdiv_shift = 0,
	.mdiv_mask = 0xff,
	.ndiv_shift = 8,
	.ndiv_mask = 0xff << 8,
	.pdiv_shift = 16,
	.pdiv_mask = 0x1f << 16,
	.pdiv_to_p = pll_qlin_pdiv_to_p,
	.pdiv_max = PLL_QLIN_PDIV_MAX,
};

static struct clk tegra_pll_re_vco = {
	.name      = "pll_re_vco",
	.ops       = &tegra_pllre_vco_ops,
	.reg       = 0x4c4,
	.parent    = &tegra_pll_ref,
	.max_rate  = 700000000,
	.u.pll = {
		.input_min = 9600000,
		.input_max = 800000000,
		.cf_min    = 9600000,
		.cf_max    = 19200000,
		.vco_min   = 350000000,
		.vco_max   = 700000000,
		.freq_table = tegra_pllre_vco_freq_table,
		.lock_delay = 300,
		.misc0 = 0x4c8 - 0x4c4,
		.controls = &pllre_controls,
		.div_layout = &pllre_div_layout,
		.round_p_to_pdiv = pll_qlin_p_to_pdiv,
		.set_defaults = pllre_set_defaults,
		.vco_out = true,
	},
};

static struct clk tegra_pll_re_out = {
	.name      = "pll_re_out",
	.ops       = &tegra_pll_out_ops,
	.parent    = &tegra_pll_re_vco,
};

static struct clk tegra_pll_re_out1 = {
	.name      = "pll_re_out1",
	.ops       = &tegra_pll_div_ops,
	.flags     = DIV_U71,
	.parent    = &tegra_pll_re_vco,
	.reg       = 0x4cc,
	.reg_shift = 0,
	.max_rate  = 700000000,
};

static struct clk_pll_freq_table tegra_pll_x_freq_table[] = {
	/* 1 GHz */
	{ 12000000, 1000000000, 166, 1, 2},	/* actual: 996.0 MHz */
	{ 13000000, 1000000000, 153, 1, 2},	/* actual: 994.0 MHz */
	{ 38400000, 1000000000, 104, 2, 2},	/* actual: 998.4 MHz */
	{ 0, 0, 0, 0, 0, 0 },
};

struct clk_pll_controls pllx_controls = {
	.enable_mask = PLL_BASE_ENABLE,
	.bypass_mask = PLL_BASE_BYPASS,
	.iddq_mask = PLLX_MISC3_IDDQ,
	.iddq_reg_idx = PLL_MISC3_IDX,
	.lock_mask = PLLX_BASE_LOCK,
	.lock_reg_idx = PLL_BASE_IDX,

	.dramp_en_mask = PLLX_MISC2_EN_DYNRAMP,
	.dramp_done_mask = PLLX_MISC2_DYNRAMP_DONE,
	.dramp_ctrl_reg_idx = PLL_MISC2_IDX,
};

static struct clk_pll_div_layout pllx_div_layout = {
	.mdiv_shift = 0,
	.mdiv_mask = 0xff,
	.ndiv_shift = 8,
	.ndiv_mask = 0xff << 8,
	.pdiv_shift = 20,
	.pdiv_mask = 0x1f << 20,
	.pdiv_to_p = pll_qlin_pdiv_to_p,
	.pdiv_max = PLL_QLIN_PDIV_MAX,

	.ndiv_new_shift = PLLX_MISC2_NDIV_NEW_SHIFT,
	.ndiv_new_reg_idx = PLL_MISC2_IDX,
};

static struct clk tegra_pll_x = {
	.name      = "pll_x",
	.flags     = PLLX,
	.ops       = &tegra_pllx_ops,
	.reg       = 0xe0,
	.parent    = &tegra_pll_ref,
	.max_rate  = 3000000000UL,
	.u.pll = {
		.input_min = 12000000,
		.input_max = 800000000,
		.cf_min    = 12000000,
		.cf_max    = 38400000,
		.vco_min   = 1350000000,
		.vco_max   = 3000000000UL,
		.freq_table = tegra_pll_x_freq_table,
		.lock_delay = 300,
		.misc0 = 0xe4 - 0xe0,
		.misc1 = 0x510 - 0xe0,
		.misc2 = 0x514 - 0xe0,
		.misc3 = 0x518 - 0xe0,
		.misc4 = 0x5f0 - 0xe0,
		.misc5 = 0x5f4 - 0xe0,
		.controls = &pllx_controls,
		.div_layout = &pllx_div_layout,
		.round_p_to_pdiv = pll_qlin_p_to_pdiv,
		.dyn_ramp = pllx_dyn_ramp,
		.set_defaults = pllx_set_defaults,
	},
};

static struct clk tegra_pll_x_out0 = {
	.name      = "pll_x_out0",
	.ops       = &tegra_pll_div_ops,
	.flags     = DIV_2 | PLLX,
	.parent    = &tegra_pll_x,
	.max_rate  = 700000000,
};

static struct clk_pll_freq_table tegra_pll_m_freq_table[] = {
	{ 12000000, 800000000, 66, 1, 1},	/* actual: 792.0 MHz */
	{ 13000000, 800000000, 61, 1, 1},	/* actual: 793.0 MHz */
	{ 19200000, 800000000, 41, 1, 1},	/* actual: 787.2 MHz */
	{ 38400000, 800000000, 41, 2, 1},	/* FIXME!!! actual: 787.2 MHz */
	{ 0, 0, 0, 0, 0, 0 },
};

static struct clk tegra_pll_m = {
	.name      = "pll_m",
	.flags     = PLLM,
	.ops       = &tegra_pllm_ops,
	.reg       = 0x90,
	.parent    = &tegra_pll_ref,
	.max_rate  = 1066000000,
	.u.pll = {
		.input_min = 12000000,
		.input_max = 500000000,
		.cf_min    = 12000000,
		.cf_max    = 19200000,	/* s/w policy, h/w capability 50 MHz */
		.vco_min   = 500000000,
		.vco_max   = 1066000000,
		.freq_table = tegra_pll_m_freq_table,
		.lock_delay = 300,
		.misc1 = 0x98 - 0x90,
		.round_p_to_pdiv = pllm_round_p_to_pdiv,
	},
};

static struct clk_pll_freq_table tegra_pll_p_freq_table[] = {
	{ 12000000, 408000000, 816, 12, 2, 8},
	{ 13000000, 408000000, 816, 13, 2, 8},
	{ 19200000, 408000000,  85, 16, 1},
	{ 38400000, 408000000,  85,  8, 1}, /* FIXME !!! */
	{ 0, 0, 0, 0, 0, 0 },
};

static struct clk tegra_pll_p = {
	.name      = "pll_p",
	.flags     = ENABLE_ON_INIT | PLL_FIXED | PLL_HAS_CPCON,
	.ops       = &tegra_pllp_ops,
	.reg       = 0xa0,
	.parent    = &tegra_pll_ref,
	.max_rate  = 432000000,
	.u.pll = {
		.input_min = 2000000,
		.input_max = 31000000,
		.cf_min    = 1000000,
		.cf_max    = 6000000,
		.vco_min   = 200000000,
		.vco_max   = 700000000,
		.freq_table = tegra_pll_p_freq_table,
		.lock_delay = 300,
	},
};

static struct clk tegra_pll_p_out1 = {
	.name      = "pll_p_out1",
	.ops       = &tegra_pll_div_ops,
	.flags     = DIV_U71 | DIV_U71_FIXED,
	.parent    = &tegra_pll_p,
	.reg       = 0xa4,
	.reg_shift = 0,
	.max_rate  = 432000000,
};

static struct clk tegra_pll_p_out2 = {
	.name      = "pll_p_out2",
	.ops       = &tegra_pll_div_ops,
	.flags     = DIV_U71 | DIV_U71_FIXED,
	.parent    = &tegra_pll_p,
	.reg       = 0xa4,
	.reg_shift = 16,
	.max_rate  = 432000000,
};

static struct clk tegra_pll_p_out3 = {
	.name      = "pll_p_out3",
	.ops       = &tegra_pll_div_ops,
	.flags     = DIV_U71 | DIV_U71_FIXED,
	.parent    = &tegra_pll_p,
	.reg       = 0xa8,
	.reg_shift = 0,
	.max_rate  = 432000000,
};

static struct clk tegra_pll_p_out4 = {
	.name      = "pll_p_out4",
	.ops       = &tegra_pll_div_ops,
	.flags     = DIV_U71 | DIV_U71_FIXED,
	.parent    = &tegra_pll_p,
	.reg       = 0xa8,
	.reg_shift = 16,
	.max_rate  = 432000000,
};

static struct clk tegra_pll_p_out5 = {
	.name      = "pll_p_out5",
	.ops       = &tegra_pll_div_ops,
	.flags     = DIV_U71 | DIV_U71_FIXED,
	.parent    = &tegra_pll_p,
	.reg       = 0x67c,
	.reg_shift = 16,
	.max_rate  = 432000000,
};

static struct clk_pll_freq_table tegra_pll_u_vco_freq_table[] = {
	{ 12000000, 480000000, 40, 1, 1},
	{ 13000000, 480000000, 36, 1, 1},	/* actual: 468.0 MHz */
	{ 38400000, 480000000, 25, 2, 1},
	{ 0, 0, 0, 0, 0, 0 },
};

struct clk_pll_controls pllu_controls = {
	.enable_mask = PLL_BASE_ENABLE,
	.bypass_mask = PLL_BASE_BYPASS,
	.iddq_mask = PLLU_MISC0_IDDQ,
	.iddq_reg_idx = PLL_MISC0_IDX,
	.lock_mask = PLLU_BASE_LOCK,
	.lock_reg_idx = PLL_BASE_IDX,
};

static struct clk tegra_pll_u_vco = {
	.name      = "pll_u_vco",
	.flags     = PLLU | PLL_FIXED,
	.ops       = &tegra_pllu_vco_ops,
	.reg       = 0xc0,
	.parent    = &tegra_pll_ref,
	.max_rate  = 700000000,
	.u.pll = {
		.input_min = 9600000,
		.input_max = 800000000,
		.cf_min    = 9600000,
		.cf_max    = 19200000,
		.vco_min   = 350000000,
		.vco_max   = 700000000,
		.freq_table = tegra_pll_u_vco_freq_table,
		.lock_delay = 300,
		.misc0 = 0xcc - 0xc0,
		.misc1 = 0xc8 - 0xc0,
		.controls = &pllu_controls,
		.div_layout = &pllre_div_layout,	/* same, re-used */
		.round_p_to_pdiv = pll_qlin_p_to_pdiv,
		.set_defaults = pllu_set_defaults,
		.vco_out = true,
		.fixed_rate = 480000000,
	},
};

static struct clk tegra_pll_u_out = {
	.name      = "pll_u_out",
	.flags     = PLLU | PLL_FIXED,
	.ops       = &tegra_pllu_out_ops,
	.parent    = &tegra_pll_u_vco,
	.u.pll = {
		.fixed_rate = 240000000,
	},
};

static struct clk tegra_pll_u_out1 = {
	.name      = "pll_u_out1",
	.ops       = &tegra_pll_div_ops,
	.flags     = DIV_U71 | DIV_U71_FIXED,
	.parent    = &tegra_pll_u_out,
	.reg       = 0xc4,
	.reg_shift = 0,
	.max_rate  = 480000000,
	.u.pll_div = {
		.default_rate = 48000000,
	},
};

static struct clk tegra_pll_u_out2 = {
	.name      = "pll_u_out2",
	.ops       = &tegra_pll_div_ops,
	.flags     = DIV_U71 | DIV_U71_FIXED,
	.parent    = &tegra_pll_u_out,
	.reg       = 0xc4,
	.reg_shift = 16,
	.max_rate  = 480000000,
	.u.pll_div = {
		.default_rate = 60000000,
	},
};

static struct clk tegra_pll_u_480M = {
	.name      = "pll_u_480M",
	.flags     = PLLU,
	.ops       = &tegra_pll_div_ops,
	.reg       = 0xc0,
	.reg_shift = 22,
	.parent    = &tegra_pll_u_vco,
	.mul       = 1,
	.div       = 1,
};

static struct clk tegra_pll_u_60M = {
	.name      = "pll_u_60M",
	.flags     = PLLU,
	.ops       = &tegra_pll_div_ops,
	.reg       = 0xc0,
	.reg_shift = 23,
	.parent    = &tegra_pll_u_out2,
	.mul       = 1,
	.div       = 1,
};

static struct clk tegra_pll_u_48M = {
	.name      = "pll_u_48M",
	.flags     = PLLU,
	.ops       = &tegra_pll_div_ops,
	.reg       = 0xc0,
	.reg_shift = 25,
	.parent    = &tegra_pll_u_out1,
	.mul       = 1,
	.div       = 1,
};

static struct clk tegra_dfll_cpu = {
	.name      = "dfll_cpu",
	.flags     = DFLL,
	.ops       = &tegra_dfll_ops,
	.reg	   = 0x2f4,
	.max_rate  = 3000000000UL,
};

static struct clk_pll_freq_table tegra_pll_e_freq_table[] = {
	/* PLLE special case: use cpcon field to store cml divider value */
	{ 672000000, 100000000, 100, 42,  16, 11},
	{ 336000000, 100000000, 100, 21,  16, 11},
	{ 312000000, 100000000, 200, 26,  24, 13},
	{ 13000000,  100000000, 200, 1,  26, 13},
	{ 12000000,  100000000, 200, 1,  24, 13},
	{ 0, 0, 0, 0, 0, 0 },
};

static struct clk tegra_pll_e = {
	.name      = "pll_e",
	.flags     = PLL_ALT_MISC_REG,
	.ops       = &tegra_plle_ops,
	.reg       = 0xe8,
	.max_rate  = 100000000,
	.u.pll = {
		.input_min = 12000000,
		.input_max = 1000000000,
		.cf_min    = 12000000,
		.cf_max    = 75000000,
		.vco_min   = 1600000000,
		.vco_max   = 2400000000U,
		.freq_table = tegra_pll_e_freq_table,
		.lock_delay = 300,
		.fixed_rate = 100000000,
	},
};



static struct clk tegra_cml0_clk = {
	.name      = "cml0",
	.parent    = &tegra_pll_e,
	.ops       = &tegra_cml_clk_ops,
	.reg       = PLLE_AUX,
	.max_rate  = 100000000,
	.u.periph  = {
		.clk_num = 0,
	},
};

static struct clk tegra_cml1_clk = {
	.name      = "cml1",
	.parent    = &tegra_pll_e,
	.ops       = &tegra_cml_clk_ops,
	.reg       = PLLE_AUX,
	.max_rate  = 100000000,
	.u.periph  = {
		.clk_num   = 1,
	},
};

static struct clk tegra_pciex_clk = {
	.name      = "pciex",
	.parent    = &tegra_pll_e,
	.ops       = &tegra_pciex_clk_ops,
	.max_rate  = 500000000,
	.u.periph  = {
		.clk_num   = 74,
	},
};

static struct clk tegra_pex_uphy_clk = {
	.name      = "pex_uphy",
	.parent    = &tegra_pll_e,
	.ops       = &tegra_pciex_clk_ops,
	.max_rate  = 500000000,
	.u.periph  = {
		.clk_num   = 205,
	},
};

/* Audio sync clocks */
#define SYNC_SOURCE(_id, _dev)				\
	{						\
		.name      = #_id "_sync",		\
		.lookup    = {				\
			.dev_id    = #_dev ,		\
			.con_id    = "ext_audio_sync",	\
		},					\
		.rate      = 24000000,			\
		.max_rate  = 24000000,			\
		.ops       = &tegra_sync_source_ops	\
	}
static struct clk tegra_sync_source_list[] = {
	SYNC_SOURCE(spdif_in, tegra30-spdif),
	SYNC_SOURCE(i2s0, tegra210-i2s.0),
	SYNC_SOURCE(i2s1, tegra210-i2s.1),
	SYNC_SOURCE(i2s2, tegra210-i2s.2),
	SYNC_SOURCE(i2s3, tegra210-i2s.3),
	SYNC_SOURCE(i2s4, tegra210-i2s.4),
	SYNC_SOURCE(vimclk, vimclk),
};

static struct clk_mux_sel mux_d_audio_clk[] = {
	{ .input = &tegra_pll_a_out0,		.value = 0},
	{ .input = &tegra_pll_p,		.value = 0x8000},
	{ .input = &tegra_clk_m,		.value = 0xc000},
	{ .input = &tegra_sync_source_list[0],	.value = 0xE000},
	{ .input = &tegra_sync_source_list[1],	.value = 0xE001},
	{ .input = &tegra_sync_source_list[2],	.value = 0xE002},
	{ .input = &tegra_sync_source_list[3],	.value = 0xE003},
	{ .input = &tegra_sync_source_list[4],	.value = 0xE004},
	{ .input = &tegra_sync_source_list[5],	.value = 0xE005},
	{ .input = &tegra_pll_a_out0,		.value = 0xE006},
	{ .input = &tegra_sync_source_list[6],	.value = 0xE007},
	{ 0, 0 }
};

static struct clk_mux_sel mux_audio_sync_clk[] =
{
	{ .input = &tegra_sync_source_list[0],	.value = 0},
	{ .input = &tegra_sync_source_list[1],	.value = 1},
	{ .input = &tegra_sync_source_list[2],	.value = 2},
	{ .input = &tegra_sync_source_list[3],	.value = 3},
	{ .input = &tegra_sync_source_list[4],	.value = 4},
	{ .input = &tegra_sync_source_list[5],	.value = 5},
	{ .input = &tegra_pll_a_out0,		.value = 6},
	{ .input = &tegra_sync_source_list[6],	.value = 7},
	{ 0, 0 }
};

#define AUDIO_SYNC_CLK(_id, _dev, _index)			\
	{						\
		.name      = #_id,			\
		.lookup    = {				\
			.dev_id    = #_dev,		\
			.con_id    = "audio_sync",	\
		},					\
		.inputs    = mux_audio_sync_clk,	\
		.reg       = 0x4A0 + (_index) * 4,	\
		.max_rate  = 24000000,			\
		.ops       = &tegra_audio_sync_clk_ops	\
	}
static struct clk tegra_clk_audio_list[] = {
	AUDIO_SYNC_CLK(audio0, tegra210-i2s.0, 0),
	AUDIO_SYNC_CLK(audio1, tegra210-i2s.1, 1),
	AUDIO_SYNC_CLK(audio2, tegra210-i2s.2, 2),
	AUDIO_SYNC_CLK(audio3, tegra210-i2s.3, 3),
	AUDIO_SYNC_CLK(audio4, tegra210-i2s.4, 4),
	AUDIO_SYNC_CLK(audio, tegra30-spdif, 5),	/* SPDIF */
};

#define AUDIO_SYNC_2X_CLK(_id, _dev, _index)				\
	{							\
		.name      = #_id "_2x",			\
		.lookup    = {					\
			.dev_id    = #_dev,			\
			.con_id    = "audio_sync_2x"		\
		},						\
		.flags     = PERIPH_NO_RESET,			\
		.max_rate  = 48000000,				\
		.ops       = &tegra_clk_double_ops,		\
		.reg       = 0x49C,				\
		.reg_shift = 24 + (_index),			\
		.parent    = &tegra_clk_audio_list[(_index)],	\
		.u.periph = {					\
			.clk_num = 113 + (_index),		\
		},						\
	}
static struct clk tegra_clk_audio_2x_list[] = {
	AUDIO_SYNC_2X_CLK(audio0, tegra210-i2s.0, 0),
	AUDIO_SYNC_2X_CLK(audio1, tegra210-i2s.1, 1),
	AUDIO_SYNC_2X_CLK(audio2, tegra210-i2s.2, 2),
	AUDIO_SYNC_2X_CLK(audio3, tegra210-i2s.3, 3),
	AUDIO_SYNC_2X_CLK(audio4, tegra210-i2s.4, 4),
	AUDIO_SYNC_2X_CLK(audio, tegra30-spdif, 5),	/* SPDIF */
};

#define MUX_I2S_SPDIF(_id, _index)					\
static struct clk_mux_sel mux_pllaout0_##_id##_2x_pllp_clkm[] = {	\
	{.input = &tegra_pll_a_out0, .value = 0},			\
	{.input = &tegra_clk_audio_2x_list[(_index)], .value = 2},	\
	{.input = &tegra_pll_p, .value = 4},				\
	{.input = &tegra_clk_m, .value = 6},				\
	{ 0, 0},							\
}
MUX_I2S_SPDIF(audio0, 0);
MUX_I2S_SPDIF(audio1, 1);
MUX_I2S_SPDIF(audio2, 2);
MUX_I2S_SPDIF(audio3, 3);
MUX_I2S_SPDIF(audio4, 4);
MUX_I2S_SPDIF(audio, 5);		/* SPDIF */

/* Audio sync dmic clocks */
#define AUDIO_SYNC_DMIC_CLK(_id, _dev, _reg)		\
	{						\
		.name      = #_id "_dmic",		\
		.lookup    = {				\
			.dev_id    = #_dev,		\
			.con_id    = "audio_sync_dmic",	\
		},					\
		.inputs    = mux_audio_sync_clk,	\
		.reg       = _reg,			\
		.max_rate  = 24000000,			\
		.ops       = &tegra_audio_sync_clk_ops	\
	}

static struct clk tegra_clk_audio_dmic_list[] = {
	AUDIO_SYNC_DMIC_CLK(audio0, tegra30-i2s.0, 0x560),
	AUDIO_SYNC_DMIC_CLK(audio1, tegra30-i2s.1, 0x564),
	AUDIO_SYNC_DMIC_CLK(audio2, tegra30-i2s.2, 0x6b8),
};

#define MUX_AUDIO_DMIC(_id, _index)					\
static struct clk_mux_sel mux_pllaout0_##_id##_dmic_pllp_clkm[] = {	\
	{.input = &tegra_pll_a_out0, .value = 0},			\
	{.input = &tegra_clk_audio_2x_list[(_index)], .value = 1},	\
	{.input = &tegra_pll_p, .value = 2},				\
	{.input = &tegra_clk_m, .value = 3},				\
	{ 0, 0},							\
}
MUX_AUDIO_DMIC(audio0, 0);
MUX_AUDIO_DMIC(audio1, 1);
MUX_AUDIO_DMIC(audio2, 2);

/* External clock outputs (through PMC) */
#define MUX_EXTERN_OUT(_id)						\
static struct clk_mux_sel mux_clkm_clkm2_clkm4_extern##_id[] = {	\
	{.input = &tegra_clk_m,		.value = 0},			\
	{.input = &tegra_clk_m_div2,	.value = 1},			\
	{.input = &tegra_clk_m_div4,	.value = 2},			\
	{.input = NULL,			.value = 3}, /* placeholder */	\
	{ 0, 0},							\
}
MUX_EXTERN_OUT(1);
MUX_EXTERN_OUT(2);
MUX_EXTERN_OUT(3);

static struct clk_mux_sel *mux_extern_out_list[] = {
	mux_clkm_clkm2_clkm4_extern1,
	mux_clkm_clkm2_clkm4_extern2,
	mux_clkm_clkm2_clkm4_extern3,
};

#define CLK_OUT_CLK(_id, _max_rate)					\
	{							\
		.name      = "clk_out_" #_id,			\
		.lookup    = {					\
			.dev_id    = "clk_out_" #_id,		\
			.con_id	   = "extern" #_id,		\
		},						\
		.ops       = &tegra_clk_out_ops,		\
		.reg       = 0x1a8,				\
		.inputs    = mux_clkm_clkm2_clkm4_extern##_id,	\
		.flags     = MUX_CLK_OUT,			\
		.max_rate  = _max_rate,				\
		.u.periph = {					\
			.clk_num   = (_id - 1) * 8 + 2,		\
		},						\
	}
static struct clk tegra_clk_out_list[] = {
	CLK_OUT_CLK(1, 26000000),
	CLK_OUT_CLK(2, 40800000),
	CLK_OUT_CLK(3, 26000000),
};

/* called after peripheral external clocks are initialized */
static void init_clk_out_mux(void)
{
	int i;
	struct clk *c;

	/* output clock con_id is the name of peripheral
	   external clock connected to input 3 of the output mux */
	for (i = 0; i < ARRAY_SIZE(tegra_clk_out_list); i++) {
		c = tegra_get_clock_by_name(
			tegra_clk_out_list[i].lookup.con_id);
		if (!c)
			pr_err("%s: could not find clk %s\n", __func__,
			       tegra_clk_out_list[i].lookup.con_id);
		mux_extern_out_list[i][3].input = c;
	}
}

/* Peripheral muxes */
static struct clk_mux_sel mux_cclk_g[] = {
	{ .input = &tegra_clk_m,	.value = 0},
	{ .input = &tegra_clk_32k,	.value = 2},
	{ .input = &tegra_pll_p,	.value = 4},
	{ .input = &tegra_pll_p_out4,	.value = 5},
	{ .input = &tegra_pll_x,	.value = 8},
	{ .input = &tegra_dfll_cpu,	.value = 15},
	{ 0, 0},
};

static struct clk_mux_sel mux_sclk[] = {
	{ .input = &tegra_clk_m,	.value = 0},
	{ .input = &tegra_pll_c_out1,	.value = 1},
	{ .input = &tegra_pll_p_out4,	.value = 2},
	{ .input = &tegra_pll_p,	.value = 3},
	{ .input = &tegra_pll_p_out2,	.value = 4},
	{ .input = &tegra_pll_c,	.value = 5},
	{ .input = &tegra_clk_32k,	.value = 6},
	{ 0, 0},
};

static struct clk_mux_sel mux_aclk_adsp[] = {
	{ .input = &tegra_pll_a1,	.value = 0},
	{ .input = &tegra_pll_p,	.value = 2},
	{ .input = &tegra_pll_a_out0,	.value = 3},
	{ .input = &tegra_clk_m,	.value = 6},
	{ .input = &tegra_pll_a,	.value = 7},
	{ 0, 0},
};

static void tegra21_adsp_clk_reset(struct clk *c, bool assert)
{
	unsigned long reg = assert ? RST_DEVICES_SET_Y : RST_DEVICES_CLR_Y;
	u32 val = ADSP_NEON | ADSP_SCU | ADSP_WDT | ADSP_DBG
		| ADSP_PERIPH | ADSP_INTF | ADSP_CORE;

	pr_debug("%s %s\n", __func__, assert ? "assert" : "deassert");
	clk_writel(val, reg);
}

static int tegra21_adsp_clk_enable(struct clk *c)
{
	u32 val = ADSP_NEON | ADSP_CORE;

	clk_writel(val, CLK_OUT_ENB_SET_Y);
	return 0;
}

static void tegra21_adsp_clk_disable(struct clk *c)
{
	u32 val = ADSP_NEON | ADSP_CORE;

	clk_writel(val, CLK_OUT_ENB_CLR_Y);
}

static struct clk_ops tegra_adsp_ops = {
	.init		= tegra21_super_clk_init,
	.enable		= tegra21_adsp_clk_enable,
	.disable	= tegra21_adsp_clk_disable,
	.set_parent	= tegra21_super_clk_set_parent,
	.set_rate	= tegra21_super_clk_set_rate,
	.reset		= tegra21_adsp_clk_reset,
};

static struct raw_notifier_head adsp_rate_change_nh;
static struct clk tegra_clk_aclk_adsp = {
	.name   = "adsp",
	.flags  = DIV_U71 | DIV_U71_INT | MUX,
	.inputs	= mux_aclk_adsp,
	.reg	= 0x6e0,
	.ops	= &tegra_adsp_ops,
	.max_rate = 600000000UL,
	.rate_change_nh = &adsp_rate_change_nh,
};

static struct clk tegra_clk_cclk_g = {
	.name	= "cclk_g",
	.flags  = DIV_U71 | DIV_U71_INT | MUX,
	.inputs	= mux_cclk_g,
	.reg	= 0x368,
	.ops	= &tegra_super_ops,
	.max_rate = 3000000000UL,
};

static struct clk tegra_clk_virtual_cpu_g = {
	.name      = "cpu_g",
	.parent    = &tegra_clk_cclk_g,
	.ops       = &tegra_cpu_ops,
	.max_rate  = 3000000000UL,
	.u.cpu = {
		.main      = &tegra_pll_x,
		.backup    = &tegra_pll_p_out4,
		.dynamic   = &tegra_dfll_cpu,
		.mode      = MODE_G,
	},
};

static struct clk_mux_sel mux_cpu_cmplx[] = {
	{ .input = &tegra_clk_virtual_cpu_g,	.value = 0},
	{ 0, 0},
};

static struct clk tegra_clk_cpu_cmplx = {
	.name      = "cpu",
	.inputs    = mux_cpu_cmplx,
	.ops       = &tegra_cpu_cmplx_ops,
	.max_rate  = 3000000000UL,
};

static struct clk tegra_clk_sclk_mux = {
	.name	= "sclk_mux",
	.inputs	= mux_sclk,
	.reg	= 0x28,
	.ops	= &tegra_super_ops,
	.max_rate = 600000000,
	.min_rate = 12000000,
};

static struct clk_mux_sel sclk_mux_out[] = {
	{ .input = &tegra_clk_sclk_mux, .value = 0},
	{ 0, 0},
};

static struct clk tegra_clk_sclk_div = {
	.name = "sclk_div",
	.ops = &tegra_periph_clk_ops,
	.reg = 0x400,
	.max_rate = 600000000,
	.min_rate = 12000000,
	.inputs = sclk_mux_out,
	.flags = DIV_U71 | PERIPH_NO_ENB | PERIPH_NO_RESET,
};

static struct clk tegra_clk_sclk = {
	.name = "sclk",
	.ops = &tegra_clk_super_skip_ops,
	.reg = 0x2c,
	.parent = &tegra_clk_sclk_div,
};

static struct clk tegra_clk_cop = {
	.name      = "cop",
	.parent    = &tegra_clk_sclk,
	.ops       = &tegra_cop_ops,
	.max_rate  = 600000000,
};

static struct clk tegra_clk_hclk = {
	.name		= "hclk",
	.flags		= DIV_BUS,
	.parent		= &tegra_clk_sclk,
	.reg		= 0x30,
	.reg_shift	= 4,
	.ops		= &tegra_bus_ops,
	.max_rate       = 600000000,
	.min_rate       = 12000000,
};

static struct clk tegra_clk_pclk = {
	.name		= "pclk",
	.flags		= DIV_BUS,
	.parent		= &tegra_clk_hclk,
	.reg		= 0x30,
	.reg_shift	= 0,
	.ops		= &tegra_bus_ops,
	.max_rate       = 600000000,
	.min_rate       = 12000000,
};

static struct raw_notifier_head sbus_rate_change_nh;

static struct clk tegra_clk_sbus_cmplx = {
	.name	   = "sbus",
	.parent    = &tegra_clk_sclk,
	.ops       = &tegra_sbus_cmplx_ops,
	.u.system  = {
		.pclk = &tegra_clk_pclk,
		.hclk = &tegra_clk_hclk,
		.sclk_low = &tegra_pll_p,
		.sclk_high = &tegra_pll_p,
	},
	.rate_change_nh = &sbus_rate_change_nh,
};

static struct clk tegra_clk_ahb = {
	.name	   = "ahb.sclk",
	.flags	   = DIV_BUS,
	.parent    = &tegra_clk_sbus_cmplx,
	.ops       = &tegra_clk_ahb_ops,
};

static struct clk tegra_clk_apb = {
	.name	   = "apb.sclk",
	.flags	   = DIV_BUS,
	.parent    = &tegra_clk_ahb,
	.ops       = &tegra_clk_apb_ops,
};

static struct clk tegra_clk_blink = {
	.name		= "blink",
	.parent		= &tegra_clk_32k,
	.reg		= 0x40,
	.ops		= &tegra_blink_clk_ops,
	.max_rate	= 32768,
};


/* Multimedia modules muxes */
static struct clk_mux_sel mux_pllc2_c_c3_pllp_plla1_clkm[] = {
	{ .input = &tegra_pll_c2, .value = 1},
	{ .input = &tegra_pll_c,  .value = 2},
	{ .input = &tegra_pll_c3, .value = 3},
	{ .input = &tegra_pll_p,  .value = 4},
	{ .input = &tegra_pll_a1, .value = 6},
	{ .input = &tegra_clk_m, .value = 7},
	{ 0, 0},
};

static struct clk_mux_sel mux_pllc_pllp_clkm_plla_pllc4[] = {
	{ .input = &tegra_pll_c,  .value = 2},
	{ .input = &tegra_pll_p,  .value = 4},
	{ .input = &tegra_clk_m,  .value = 5},
	{ .input = &tegra_pll_a_out0, .value = 6},
	{ .input = &tegra_pll_c4_out0, .value = 7},
	{ 0, 0},
};

static struct clk_mux_sel mux_pllc_pllp_plla[] = {
	{ .input = &tegra_pll_c, .value = 2},
	{ .input = &tegra_pll_p, .value = 4},
	{ .input = &tegra_pll_a_out0, .value = 6},
	{ 0, 0},
};

static struct clk_mux_sel mux_pllc_pllp_plla_pllc4[] = {
	{ .input = &tegra_pll_c, .value = 1},
	{ .input = &tegra_pll_p, .value = 2},
	{ .input = &tegra_pll_a_out0, .value = 3},
	/* Skip C2(4) */
	{ .input = &tegra_pll_c4_out0, .value = 5},
	{ 0, 0},
};

static struct clk_mux_sel mux_pllc_pllp_plla1_pllc2_c3_clkm[] = {
	{ .input = &tegra_pll_c, .value = 1},
	{ .input = &tegra_pll_p, .value = 2},
	{ .input = &tegra_pll_a1, .value = 3},
	{ .input = &tegra_pll_c2, .value = 4},
	{ .input = &tegra_pll_c3, .value = 5},
	{ .input = &tegra_clk_m, .value = 6},
	{ 0, 0},
};

static struct clk_mux_sel mux_pllc2_c_c3_pllp_plla1_pllc4[] = {
	{ .input = &tegra_pll_c2, .value = 1},
	{ .input = &tegra_pll_c, .value = 2},
	{ .input = &tegra_pll_c3, .value = 3},
	{ .input = &tegra_pll_p, .value = 4},
	{ .input = &tegra_pll_a1, .value = 6},
	{ .input = &tegra_pll_c4_out0, .value = 7},
	{ 0, 0},
};

static struct clk_mux_sel mux_pllc_pllp_plla1_pllc2_c3_clkm_pllc4[] = {
	{ .input = &tegra_pll_c, .value = 1},
	{ .input = &tegra_pll_p, .value = 2},
	{ .input = &tegra_pll_a1, .value = 3},
	{ .input = &tegra_pll_c2, .value = 4},
	{ .input = &tegra_pll_c3, .value = 5},
	{ .input = &tegra_clk_m, .value = 6},
	{ .input = &tegra_pll_c4_out0, .value = 7},
	{ 0, 0},
};

static struct clk_mux_sel mux_plla_pllc_pllp_clkm[] = {
	{ .input = &tegra_pll_a_out0, .value = 0},
	{ .input = &tegra_pll_c, .value = 2},
	{ .input = &tegra_pll_p, .value = 4},
	{ .input = &tegra_clk_m, .value = 6},
	{ 0, 0},
};

/* EMC muxes */
/* FIXME: add EMC latency mux */
static struct clk_mux_sel mux_pllm_pllc_pllp_clkm[] = {
	{ .input = &tegra_pll_m, .value = 0},
	{ .input = &tegra_pll_c, .value = 1},
	{ .input = &tegra_pll_p, .value = 2},
	{ .input = &tegra_clk_m, .value = 3},
	{ .input = &tegra_pll_m, .value = 4}, /* low jitter PLLM output */
	/* { .input = &tegra_pll_c2, .value = 5}, - no use on tegra21x */
	/* { .input = &tegra_pll_c3, .value = 6}, - no use on tegra21x */
	{ .input = &tegra_pll_c, .value = 7}, /* low jitter PLLC output */
	{ 0, 0},
};


/* Display subsystem muxes */
static struct clk_mux_sel mux_pllp_plld_plla_pllc_plld2_clkm[] = {
	{.input = &tegra_pll_p, .value = 0},
	{.input = &tegra_pll_d_out0, .value = 2},
	{.input = &tegra_pll_a_out0, .value = 3},
	{.input = &tegra_pll_c, .value = 4},
	{.input = &tegra_pll_d2, .value = 5},
	{.input = &tegra_clk_m, .value = 6},
	{ 0, 0},
};

static struct clk_mux_sel mux_pllp_plld_plld2_clkm[] = {
	{.input = &tegra_pll_p, .value = 0},
	{.input = &tegra_pll_d_out0, .value = 2},
	{.input = &tegra_pll_d2, .value = 5},
	{.input = &tegra_clk_m, .value = 6},
	{ 0, 0},
};

static struct clk_mux_sel mux_plld_out0[] = {
	{ .input = &tegra_pll_d_out0,  .value = 0},
	{ 0, 0},
};

static struct clk_mux_sel mux_pllp_pllc_clkm[] = {
	{.input = &tegra_pll_p,     .value = 0},
	{.input = &tegra_pll_c,     .value = 2},
	{.input = &tegra_clk_m,     .value = 6},
	{ 0, 0},
};

static struct clk_mux_sel mux_pllp_pllc4_out2_pllc4_out1_clkm_pllc4_out0[] = {
	{.input = &tegra_pll_p,             .value = 0},
	{.input = &tegra_pll_c4_out2,       .value = 3},
	{.input = &tegra_pll_c4_out1,       .value = 4},
	{.input = &tegra_clk_m,             .value = 6},
	{.input = &tegra_pll_c4_out0,       .value = 7},
	{ 0, 0},
};

/* Peripheral muxes */
static struct clk_mux_sel mux_pllp_pllc2_c_c3_clkm[] = {
	{ .input = &tegra_pll_p,  .value = 0},
	{ .input = &tegra_pll_c2, .value = 1},
	{ .input = &tegra_pll_c,  .value = 2},
	{ .input = &tegra_pll_c3, .value = 3},
	{ .input = &tegra_clk_m,  .value = 6},
	{ 0, 0},
};

static struct clk_mux_sel mux_pllp_pllc[] = {
	{.input = &tegra_pll_p,     .value = 0},
	{.input = &tegra_pll_c,     .value = 1},
	{ 0, 0},
};

static struct clk_mux_sel mux_pllp_clkm_2[] = {
	{ .input = &tegra_pll_p, .value = 2},
	{ .input = &tegra_clk_m, .value = 6},
	{ 0, 0},
};

static struct clk_mux_sel mux_pllp_clkm_1[] = {
	{ .input = &tegra_pll_p, .value = 0},
	{ .input = &tegra_clk_m, .value = 2},
	{ 0, 0},
};

static struct clk_mux_sel mux_pllp_clkm[] = {
	{ .input = &tegra_pll_p, .value = 0},
	{ .input = &tegra_clk_m, .value = 6},
	{ 0, 0},
};

static struct clk_mux_sel mux_pllp_pllp_out3_clkm_clk32k_plla[] = {
	{ .input = &tegra_pll_p, .value = 0},
	{ .input = &tegra_pll_p_out3, .value = 1},
	{ .input = &tegra_clk_m, .value = 2},
	{ .input = &tegra_clk_32k, .value = 3},
	{ .input = &tegra_pll_a_out0, .value = 4},
	{ 0, 0},
};

static struct clk_mux_sel mux_pllp_out3_clkm_pllp_pllc4[] = {
	{ .input = &tegra_pll_p_out3, .value = 0},
	{ .input = &tegra_clk_m, .value = 3},
	{ .input = &tegra_pll_p, .value = 4},
	{ .input = &tegra_pll_c4_out0, .value = 5},
	{ .input = &tegra_pll_c4_out1, .value = 6},
	{ .input = &tegra_pll_c4_out2, .value = 7},
	{ 0, 0},
};

static struct clk_mux_sel mux_pllp_pllc_clk32_clkm[] = {
	{.input = &tegra_pll_p,     .value = 0},
	{.input = &tegra_pll_c,     .value = 2},
	{.input = &tegra_clk_32k,   .value = 4},
	{.input = &tegra_clk_m,     .value = 6},
	{ 0, 0},
};

static struct clk_mux_sel mux_pllp_pllc_clkm_clk32[] = {
	{.input = &tegra_pll_p,     .value = 0},
	{.input = &tegra_pll_c,     .value = 2},
	{.input = &tegra_clk_m,     .value = 4},
	{.input = &tegra_clk_32k,   .value = 6},
	{ 0, 0},
};

static struct clk_mux_sel mux_plla_clk32_pllp_clkm_plle[] = {
	{ .input = &tegra_pll_a_out0, .value = 0},
	{ .input = &tegra_clk_32k,    .value = 1},
	{ .input = &tegra_pll_p,      .value = 2},
	{ .input = &tegra_clk_m,      .value = 3},
	{ .input = &tegra_pll_e,      .value = 4},
	{ 0, 0},
};

static struct clk_mux_sel mux_clkm_pllp_pllc_pllre[] = {
	{ .input = &tegra_clk_m,  .value = 0},
	{ .input = &tegra_pll_p,  .value = 1},
	{ .input = &tegra_pll_c,  .value = 3},
	{ .input = &tegra_pll_re_out,  .value = 5},
	{ 0, 0},
};

static struct clk_mux_sel mux_clkm_48M_pllp_480M[] = {
	{ .input = &tegra_clk_m,      .value = 0},
	{ .input = &tegra_pll_u_48M,  .value = 2},
	{ .input = &tegra_pll_p,      .value = 4},
	{ .input = &tegra_pll_u_480M, .value = 6},
	{ 0, 0},
};

static struct clk_mux_sel mux_clkm_pllre_clk32_480M_pllc_ref[] = {
	{ .input = &tegra_clk_m,      .value = 0},
	{ .input = &tegra_pll_re_out, .value = 1},
	{ .input = &tegra_clk_32k,    .value = 2},
	{ .input = &tegra_pll_u_480M, .value = 3},
	{ .input = &tegra_pll_c,      .value = 4},
	{ .input = &tegra_pll_ref,    .value = 7},
	{ 0, 0},
};

static struct clk_mux_sel mux_pllp3_pllc_clkm[] = {
	{ .input = &tegra_pll_p_out3, .value = 0},
	{ .input = &tegra_pll_c,  .value = 1},
	{ .input = &tegra_clk_m,  .value = 3},
	{ 0, 0},
};

/* Single clock source ("fake") muxes */
static struct clk_mux_sel mux_clk_m[] = {
	{ .input = &tegra_clk_m, .value = 0},
	{ 0, 0},
};

static struct clk_mux_sel mux_pllp[] = {
	{ .input = &tegra_pll_p, .value = 0},
	{ 0, 0},
};

static struct clk_mux_sel mux_pllp_out3[] = {
	{ .input = &tegra_pll_p_out3, .value = 0},
	{ 0, 0},
};

static struct clk_mux_sel mux_clk_32k[] = {
	{ .input = &tegra_clk_32k, .value = 0},
	{ 0, 0},
};

static struct clk tegra_clk_mc;
static struct clk_mux_sel mux_clk_mc[] = {
	{ .input = &tegra_clk_mc, .value = 0},
	{ 0, 0},
};

static struct clk_mux_sel mux_plld[] = {
	{ .input = &tegra_pll_d_out0, .value = 1},
	{ 0, 0},
};

static struct raw_notifier_head emc_rate_change_nh;

static struct clk tegra_clk_emc = {
	.name = "emc",
	.ops = &tegra_emc_clk_ops,
	.reg = 0x19c,
	.max_rate = 1066000000,
	.min_rate = 12750000,
	.inputs = mux_pllm_pllc_pllp_clkm,
	.flags = MUX | DIV_U71 | PERIPH_EMC_ENB,
	.u.periph = {
		.clk_num = 57,
	},
	.rate_change_nh = &emc_rate_change_nh,
};

static struct clk tegra_clk_mc = {
	.name = "mc",
	.ops = &tegra_mc_clk_ops,
	.max_rate = 1066000000,
	.parent = &tegra_clk_emc,
	.flags = PERIPH_NO_RESET,
	.u.periph = {
		.clk_num = 32,
	},
};

static struct raw_notifier_head host1x_rate_change_nh;

static struct clk tegra_clk_host1x = {
	.name      = "host1x",
	.lookup    = {
		.dev_id = "host1x",
	},
	.ops       = &tegra_1xbus_clk_ops,
	.reg       = 0x180,
	.inputs    = mux_pllc_pllp_clkm_plla_pllc4,
	.flags     = MUX | DIV_U71 | DIV_U71_INT,
	.max_rate  = 408000000,
	.min_rate  = 12000000,
	.u.periph = {
		.clk_num   = 28,
		.pll_low = &tegra_pll_p,
		.pll_high = &tegra_pll_c,
	},
	.rate_change_nh = &host1x_rate_change_nh,
};

static struct raw_notifier_head c2bus_rate_change_nh;
static struct raw_notifier_head c3bus_rate_change_nh;

static struct clk tegra_clk_c2bus = {
	.name      = "c2bus",
	.parent    = &tegra_pll_c2,
	.ops       = &tegra_clk_cbus_ops,
	.max_rate  = 700000000,
	.mul       = 1,
	.div       = 1,
	.flags     = PERIPH_ON_CBUS,
	.shared_bus_backup = {
		.input = &tegra_pll_p,
	},
	.rate_change_nh = &c2bus_rate_change_nh,
};
static struct clk tegra_clk_c3bus = {
	.name      = "c3bus",
	.parent    = &tegra_pll_c3,
	.ops       = &tegra_clk_cbus_ops,
	.max_rate  = 700000000,
	.mul       = 1,
	.div       = 1,
	.flags     = PERIPH_ON_CBUS,
	.shared_bus_backup = {
		.input = &tegra_pll_p,
	},
	.rate_change_nh = &c3bus_rate_change_nh,
};

#ifdef CONFIG_TEGRA_MIGRATE_CBUS_USERS
static DEFINE_MUTEX(cbus_mutex);
#define CROSS_CBUS_MUTEX (&cbus_mutex)
#else
#define CROSS_CBUS_MUTEX NULL
#endif


static struct clk_mux_sel mux_clk_cbus[] = {
	{ .input = &tegra_clk_c2bus, .value = 0},
	{ .input = &tegra_clk_c3bus, .value = 1},
	{ 0, 0},
};

#define DUAL_CBUS_CLK(_name, _dev, _con, _parent, _id, _div, _mode)\
	{						\
		.name      = _name,			\
		.lookup    = {				\
			.dev_id    = _dev,		\
			.con_id    = _con,		\
		},					\
		.ops = &tegra_clk_shared_bus_user_ops,	\
		.parent = _parent,			\
		.inputs = mux_clk_cbus,			\
		.flags = MUX,				\
		.u.shared_bus_user = {			\
			.client_id = _id,		\
			.client_div = _div,		\
			.mode = _mode,			\
		},					\
		.cross_clk_mutex = CROSS_CBUS_MUTEX,	\
	}

static struct clk_ops tegra_clk_gpu_ops = {
	.enable		= &tegra21_periph_clk_enable,
	.disable	= &tegra21_periph_clk_disable,
	.reset		= &tegra21_periph_clk_reset,
};

/* This is a dummy clock for gpu. The enable/disable/reset routine controls
   input clock of the actual gpu clock. The input clock itself has a fixed
   frequency. The actual gpu clock's frequency is controlled by gpu driver,
   not here in clock framework. However, we assoicate this dummy clock with
   dvfs to control voltage of gpu rail along with frequency change of actual
   gpu clock. So frequency here and in dvfs are based on the acutal gpu clock. */
static struct clk tegra_clk_gpu = {
	.name      = "gpu_ref",
	.ops       = &tegra_clk_gpu_ops,
	.parent    = &tegra_pll_ref,
	.u.periph  = {
		.clk_num = 184,
	},
	.max_rate  = 48000000,
	.min_rate  = 12000000,
};

#define RATE_GRANULARITY	100000 /* 0.1 MHz */
#if defined(CONFIG_TEGRA_CLOCK_DEBUG_FUNC)
static int gbus_round_pass_thru;
void tegra_gbus_round_pass_thru_enable(bool enable)
{
	if (enable)
		gbus_round_pass_thru = 1;
	else
		gbus_round_pass_thru = 0;
}
EXPORT_SYMBOL(tegra_gbus_round_pass_thru_enable);
#else
#define gbus_round_pass_thru	0
#endif

static void tegra21_clk_gbus_init(struct clk *c)
{
	unsigned long rate;
	bool enabled;

	pr_debug("%s on clock %s (export ops %s)\n", __func__,
		 c->name, c->u.export_clk.ops ? "ready" : "not ready");

	if (!c->u.export_clk.ops || !c->u.export_clk.ops->init)
		return;

	c->u.export_clk.ops->init(c->u.export_clk.ops->data, &rate, &enabled);
	c->div = clk_get_rate(c->parent) / RATE_GRANULARITY;
	c->mul = rate / RATE_GRANULARITY;
	c->state = enabled ? ON : OFF;
}

static int tegra21_clk_gbus_enable(struct clk *c)
{
	pr_debug("%s on clock %s (export ops %s)\n", __func__,
		 c->name, c->u.export_clk.ops ? "ready" : "not ready");

	if (!c->u.export_clk.ops || !c->u.export_clk.ops->enable)
		return -ENOENT;

	return c->u.export_clk.ops->enable(c->u.export_clk.ops->data);
}

static void tegra21_clk_gbus_disable(struct clk *c)
{
	pr_debug("%s on clock %s (export ops %s)\n", __func__,
		 c->name, c->u.export_clk.ops ? "ready" : "not ready");

	if (!c->u.export_clk.ops || !c->u.export_clk.ops->disable)
		return;

	c->u.export_clk.ops->disable(c->u.export_clk.ops->data);
}

static int tegra21_clk_gbus_set_rate(struct clk *c, unsigned long rate)
{
	int ret;

	pr_debug("%s %lu on clock %s (export ops %s)\n", __func__,
		 rate, c->name, c->u.export_clk.ops ? "ready" : "not ready");

	if (!c->u.export_clk.ops || !c->u.export_clk.ops->set_rate)
		return -ENOENT;

	ret = c->u.export_clk.ops->set_rate(c->u.export_clk.ops->data, &rate);
	if (!ret)
		c->mul = rate / RATE_GRANULARITY;
	return ret;
}

static long tegra21_clk_gbus_round_updown(struct clk *c, unsigned long rate,
					  bool up)
{
	return gbus_round_pass_thru ? rate :
		tegra21_clk_cbus_round_updown(c, rate, up);
}

static long tegra21_clk_gbus_round_rate(struct clk *c, unsigned long rate)
{
	return tegra21_clk_gbus_round_updown(c, rate, true);
}

static struct clk_ops tegra_clk_gbus_ops = {
	.init		= tegra21_clk_gbus_init,
	.enable		= tegra21_clk_gbus_enable,
	.disable	= tegra21_clk_gbus_disable,
	.set_rate	= tegra21_clk_gbus_set_rate,
	.round_rate	= tegra21_clk_gbus_round_rate,
	.round_rate_updown = tegra21_clk_gbus_round_updown,
	.shared_bus_update = tegra21_clk_shared_connector_update, /* re-use */
};

static struct clk tegra_clk_gbus = {
	.name      = "gbus",
	.ops       = &tegra_clk_gbus_ops,
	.parent    = &tegra_clk_gpu,
	.max_rate  = 1000000000,
	.shared_bus_flags = SHARED_BUS_RETENTION,
};

static void tegra21_camera_mclk_init(struct clk *c)
{
	c->state = OFF;
	c->set = true;

	if (!strcmp(c->name, "mclk")) {
		c->parent = tegra_get_clock_by_name("vi_sensor");
		c->max_rate = c->parent->max_rate;
	} else if (!strcmp(c->name, "mclk2")) {
		c->parent = tegra_get_clock_by_name("vi_sensor2");
		c->max_rate = c->parent->max_rate;
	}
}

static int tegra21_camera_mclk_set_rate(struct clk *c, unsigned long rate)
{
	return clk_set_rate(c->parent, rate);
}

static struct clk_ops tegra_camera_mclk_ops = {
	.init     = tegra21_camera_mclk_init,
	.enable   = tegra21_periph_clk_enable,
	.disable  = tegra21_periph_clk_disable,
	.set_rate = tegra21_camera_mclk_set_rate,
};

static struct clk tegra_camera_mclk = {
	.name = "mclk",
	.ops = &tegra_camera_mclk_ops,
	.u.periph = {
		.clk_num = 92, /* csus */
	},
	.flags = PERIPH_NO_RESET,
};

static struct clk tegra_camera_mclk2 = {
	.name = "mclk2",
	.ops = &tegra_camera_mclk_ops,
	.u.periph = {
		.clk_num = 171, /* vim2_clk */
	},
	.flags = PERIPH_NO_RESET,
};

static struct clk tegra_clk_isp = {
	.name = "isp",
	.ops = &tegra_periph_clk_ops,
	.reg = 0x144,
	.max_rate = 600000000,
	.inputs = mux_pllc_pllp_plla1_pllc2_c3_clkm_pllc4,
	.flags = MUX | DIV_U71 | PERIPH_NO_ENB | PERIPH_NO_RESET,
};

static struct clk_mux_sel mux_isp[] = {
	{ .input = &tegra_clk_isp, .value = 0},
	{ 0, 0},
};

static struct raw_notifier_head cbus_rate_change_nh;

static struct clk tegra_clk_cbus = {
	.name	   = "cbus",
	.parent    = &tegra_pll_c,
	.ops       = &tegra_clk_cbus_ops,
	.max_rate  = 700000000,
	.mul	   = 1,
	.div	   = 1,
	.flags     = PERIPH_ON_CBUS,
	.shared_bus_backup = {
		.input = &tegra_pll_p,
	},
	.rate_change_nh = &cbus_rate_change_nh,
};

#define PERIPH_CLK(_name, _dev, _con, _clk_num, _reg, _max, _inputs, _flags) \
	{						\
		.name      = _name,			\
		.lookup    = {				\
			.dev_id    = _dev,		\
			.con_id	   = _con,		\
		},					\
		.ops       = &tegra_periph_clk_ops,	\
		.reg       = _reg,			\
		.inputs    = _inputs,			\
		.flags     = _flags,			\
		.max_rate  = _max,			\
		.u.periph = {				\
			.clk_num   = _clk_num,		\
		},					\
	}

#define PERIPH_CLK_EX(_name, _dev, _con, _clk_num, _reg, _max, _inputs,	\
			_flags, _ops) 					\
	{						\
		.name      = _name,			\
		.lookup    = {				\
			.dev_id    = _dev,		\
			.con_id	   = _con,		\
		},					\
		.ops       = _ops,			\
		.reg       = _reg,			\
		.inputs    = _inputs,			\
		.flags     = _flags,			\
		.max_rate  = _max,			\
		.u.periph = {				\
			.clk_num   = _clk_num,		\
		},					\
	}

#define SUPER_SKIP_CLK(_name, _dev, _con, _reg, _parent, _flags) \
	{						\
		.name      = _name,			\
		.lookup    = {				\
			.dev_id    = _dev,		\
			.con_id	   = _con,		\
		},					\
		.ops       = &tegra_clk_super_skip_ops,	\
		.reg       = _reg,			\
		.parent    = _parent,			\
		.flags     = _flags,			\
	}

#define PERIPH_CLK_SKIP(_name, _dev, _con, _clk_num, _reg, _reg_skip, _max, _inputs, _flags) \
	PERIPH_CLK(_name, _dev, _con, _clk_num, _reg, _max, _inputs, _flags), \
	SUPER_SKIP_CLK(_name "_skip", _dev, "skip", _reg_skip, NULL, 0)

#define D_AUDIO_CLK(_name, _dev, _con, _clk_num, _reg, _max, _inputs, _flags) \
	{						\
		.name      = _name,			\
		.lookup    = {				\
			.dev_id    = _dev,		\
			.con_id	   = _con,		\
		},					\
		.ops       = &tegra_periph_clk_ops,	\
		.reg       = _reg,			\
		.inputs    = _inputs,			\
		.flags     = _flags,			\
		.max_rate  = _max,			\
		.u.periph = {				\
			.clk_num   = _clk_num,		\
			.src_mask  = 0xE01F << 16,	\
			.src_shift = 16,		\
		},					\
	}

#define SHARED_CLK(_name, _dev, _con, _parent, _id, _div, _mode)\
	{						\
		.name      = _name,			\
		.lookup    = {				\
			.dev_id    = _dev,		\
			.con_id    = _con,		\
		},					\
		.ops = &tegra_clk_shared_bus_user_ops,	\
		.parent = _parent,			\
		.u.shared_bus_user = {			\
			.client_id = _id,		\
			.client_div = _div,		\
			.mode = _mode,			\
		},					\
	}
#define SHARED_LIMIT(_name, _dev, _con, _parent, _id, _div, _mode)\
	{						\
		.name      = _name,			\
		.lookup    = {				\
			.dev_id    = _dev,		\
			.con_id    = _con,		\
		},					\
		.ops = &tegra_clk_shared_bus_user_ops,	\
		.parent = _parent,			\
		.flags     = BUS_RATE_LIMIT,		\
		.u.shared_bus_user = {			\
			.client_id = _id,		\
			.client_div = _div,		\
			.mode = _mode,			\
		},					\
	}
#define SHARED_CONNECT(_name, _dev, _con, _parent, _id, _div, _mode)\
	{						\
		.name      = _name,			\
		.lookup    = {				\
			.dev_id    = _dev,		\
			.con_id    = _con,		\
		},					\
		.ops = &tegra_clk_shared_connector_ops,	\
		.parent = _parent,			\
		.u.shared_bus_user = {			\
			.client_id = _id,		\
			.client_div = _div,		\
			.mode = _mode,			\
		},					\
	}
#define SHARED_EMC_CLK(_name, _dev, _con, _parent, _id, _div, _mode, _flag)\
	{						\
		.name      = _name,			\
		.lookup    = {				\
			.dev_id    = _dev,		\
			.con_id    = _con,		\
		},					\
		.ops = &tegra_clk_shared_bus_user_ops,	\
		.parent = _parent,			\
		.u.shared_bus_user = {			\
			.client_id = _id,		\
			.client_div = _div,		\
			.mode = _mode,			\
			.usage_flag = _flag,		\
		},					\
	}

static DEFINE_MUTEX(sbus_cross_mutex);
#define SHARED_SCLK(_name, _dev, _con, _parent, _id, _div, _mode)\
	{						\
		.name = _name,				\
		.lookup = {				\
			.dev_id = _dev,			\
			.con_id = _con,			\
		},					\
		.ops = &tegra_clk_shared_bus_user_ops,	\
		.parent = _parent,			\
		.u.shared_bus_user = {			\
			.client_id = _id,		\
			.client_div = _div,		\
			.mode = _mode,			\
		},					\
		.cross_clk_mutex = &sbus_cross_mutex,	\
}

struct clk tegra_list_clks[] = {
	PERIPH_CLK("apbdma",	"tegra-apbdma",		NULL,	34,	0,	38400000,  mux_clk_m,			0),
	PERIPH_CLK("rtc",	"rtc-tegra",		NULL,	4,	0,	32768,     mux_clk_32k,			PERIPH_NO_RESET | PERIPH_ON_APB),
	PERIPH_CLK("kbc",	"tegra-kbc",		NULL,	36,	0,	32768,	   mux_clk_32k, 		PERIPH_NO_RESET | PERIPH_ON_APB),
	PERIPH_CLK("timer",	"timer",		NULL,	5,	0,	38400000,  mux_clk_m,			0),
	PERIPH_CLK("spare1",	"spare1",		NULL,	192,	0,	38400000,  mux_clk_m,			0),
	PERIPH_CLK("axiap",	"axiap",		NULL,	196,	0,	38400000,  mux_clk_m,			0),
	PERIPH_CLK("sor_safe",	"sor_safe",		NULL,	222,	0,	600000000, mux_clk_m,			PERIPH_NO_RESET),
	PERIPH_CLK("pllp_out_cpu", "pllp_out_cpu",	NULL,	223,	0,	408000000, mux_pllp,			PERIPH_NO_RESET),
	PERIPH_CLK("iqc1",	"iqc1",			NULL,	221,	0,	38400000,  mux_clk_m,			PERIPH_NO_RESET | PERIPH_ON_APB),
	PERIPH_CLK("iqc2",	"iqc2",			NULL,	220,	0,	38400000,  mux_clk_m,			PERIPH_NO_RESET | PERIPH_ON_APB),
	PERIPH_CLK("kfuse",	"kfuse-tegra",		NULL,	40,	0,	38400000,  mux_clk_m,			PERIPH_ON_APB),
	PERIPH_CLK("fuse",	"fuse-tegra",		"fuse",	39,	0,	38400000,  mux_clk_m,			PERIPH_ON_APB),
	PERIPH_CLK("fuse_burn",	"fuse-tegra",		"fuse_burn",	39,	0,	38400000,  mux_clk_m,		PERIPH_ON_APB),
	PERIPH_CLK("apbif",	"tegra210-admaif",	NULL, 107,	0,	38400000,  mux_clk_m,			PERIPH_NO_RESET | PERIPH_ON_APB),
	PERIPH_CLK("i2s0",	"tegra210-i2s.0",	NULL,	30,	0x1d8,	204000000,  mux_pllaout0_audio0_2x_pllp_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("i2s1",	"tegra210-i2s.1",	NULL,	11,	0x100,	204000000,  mux_pllaout0_audio1_2x_pllp_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("i2s2",	"tegra210-i2s.2",	NULL,	18,	0x104,	204000000,  mux_pllaout0_audio2_2x_pllp_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("i2s3",	"tegra210-i2s.3",	NULL,	101,	0x3bc,	204000000,  mux_pllaout0_audio3_2x_pllp_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("i2s4",	"tegra210-i2s.4",	NULL,	102,	0x3c0,	204000000,  mux_pllaout0_audio4_2x_pllp_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("spdif_out",	"tegra30-spdif",	"spdif_out",	10,	0x108,	 24576000, mux_pllaout0_audio_2x_pllp_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("spdif_in",	"tegra30-spdif",	"spdif_in",	10,	0x10c,	408000000, mux_pllp_pllc,		MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("dmic1",	"tegra30-i2s.0",	NULL,	161,	0x64c,	24576000, mux_pllaout0_audio0_dmic_pllp_clkm,	MUX | DIV_U71 | PERIPH_NO_RESET | PERIPH_ON_APB),
	PERIPH_CLK("dmic2",	"tegra30-i2s.1",	NULL,	162,	0x650,	24576000, mux_pllaout0_audio1_dmic_pllp_clkm,	MUX | DIV_U71 | PERIPH_NO_RESET | PERIPH_ON_APB),
	PERIPH_CLK("dmic3",	"tegra30-i2s.2",	NULL,	197,	0x6bc,	24576000, mux_pllaout0_audio2_dmic_pllp_clkm,	MUX | DIV_U71 | PERIPH_NO_RESET | PERIPH_ON_APB),
	PERIPH_CLK("ape",	NULL,			"ape",	198,	0x6c0,	300000000, mux_plla_pllc_pllp_clkm,		MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("maud",	"maud",			NULL,	202,	0x6d4,	300000000, mux_pllp_pllp_out3_clkm_clk32k_plla,		MUX | DIV_U71 | PERIPH_NO_RESET | PERIPH_ON_APB),
	PERIPH_CLK("pwm",	"pwm",			NULL,	17,	0x110,	48000000, mux_pllp_pllc_clk32_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	D_AUDIO_CLK("d_audio",	"tegra210-axbar",		"ahub",	106,	0x3d0,	48000000,  mux_d_audio_clk,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("hda",	"tegra30-hda",		"hda",   125,	0x428,	108000000, mux_pllp_pllc_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("hda2codec_2x",	"tegra30-hda",	"hda2codec",   111,	0x3e4,	910000000,  mux_pllp_pllc_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("hda2hdmi",	"tegra30-hda",		"hda2hdmi",	128,	0,	408000000,  mux_clk_m, PERIPH_ON_APB),
	PERIPH_CLK("qspi",	"qspi", 		NULL,	211,	0x6c4, 166000000, mux_pllp_pllc_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("vi_i2c",	"vi_i2c", 		NULL,	208,	0x6c8, 136000000, mux_pllp_pllc_clkm,	MUX | DIV_U151 | PERIPH_ON_APB),
	PERIPH_CLK("sbc1",	"spi-tegra114.0", 	NULL,	41,	0x134, 204000000, mux_pllp_pllc_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("sbc2",	"spi-tegra114.1", 	NULL,	44,	0x118, 204000000, mux_pllp_pllc_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("sbc3",	"spi-tegra114.2", 	NULL,	46,	0x11c, 204000000, mux_pllp_pllc_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("sbc4",	"spi-tegra114.3", 	NULL,	68,	0x1b4, 48000000, mux_pllp_pllc_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("sata_oob",	"tegra_sata_oob",	NULL,	123,	0x420,	216000000, mux_pllp_pllc_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("sata",	"tegra_sata",		NULL,	124,	0x424,	216000000, mux_pllp_pllc_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("sata_cold",	"tegra_sata_cold",	NULL,	129,	0,	48000000,  mux_clk_m, PERIPH_ON_APB),
	PERIPH_CLK("sdmmc1",	"sdhci-tegra.0",	NULL,	14,	0x150,	208000000, mux_pllp_pllc4_out2_pllc4_out1_clkm_pllc4_out0,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("sdmmc2",	"sdhci-tegra.1",	NULL,	9,	0x154,	200000000, mux_pllp_pllc4_out2_pllc4_out1_clkm_pllc4_out0,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("sdmmc3",	"sdhci-tegra.2",	NULL,	69,	0x1bc,	208000000, mux_pllp_pllc4_out2_pllc4_out1_clkm_pllc4_out0,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("sdmmc4",	"sdhci-tegra.3",	NULL,	15,	0x164,	200000000, mux_pllp_pllc4_out2_pllc4_out1_clkm_pllc4_out0,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("sdmmc1_ddr",	"sdhci-tegra.0",	"ddr",	14,	0x150,	208000000, mux_pllp_pllc4_out2_pllc4_out1_clkm_pllc4_out0,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("sdmmc2_ddr",	"sdhci-tegra.1",	"ddr",	9,	0x154,	200000000, mux_pllp_pllc4_out2_pllc4_out1_clkm_pllc4_out0,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("sdmmc3_ddr",	"sdhci-tegra.2",	"ddr",	69,	0x1bc,	208000000, mux_pllp_pllc4_out2_pllc4_out1_clkm_pllc4_out0,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("sdmmc4_ddr",	"sdhci-tegra.3",	"ddr",	15,	0x164,	200000000, mux_pllp_pllc4_out2_pllc4_out1_clkm_pllc4_out0,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("sdmmc_legacy",	"sdmmc_legacy",	NULL,	193,	0x694,	208000000, mux_pllp_out3_clkm_pllp_pllc4, MUX | DIV_U71 | PERIPH_NO_RESET | PERIPH_ON_APB),
	PERIPH_CLK("vcp",	"nvavp",		"vcp",	29,	0,	250000000, mux_clk_m, 			0),
	PERIPH_CLK("bsea",	"nvavp",		"bsea",	62,	0,	250000000, mux_clk_m, 			0),
	PERIPH_CLK("bsev",	"tegra-aes",		"bsev",	63,	0,	250000000, mux_clk_m, 			0),
	PERIPH_CLK("cec",	"tegra_cec",		NULL,	136,	0,	250000000, mux_clk_m,			PERIPH_ON_APB),
	PERIPH_CLK("csite",	"csite",		NULL,	73,	0x1d4,	144000000, mux_pllp_pllc_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("la",	"la",			NULL,	76,	0x1f8,	26000000,  mux_pllp_pllc_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("owr",	"tegra_w1",		NULL,	71,	0x1cc,	26000000,  mux_pllp_pllc_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("i2c1",	"tegra21-i2c.0",	"div-clk",	12,	0x124,	136000000, mux_pllp_clkm,	MUX | DIV_U16 | PERIPH_ON_APB),
	PERIPH_CLK("i2c2",	"tegra21-i2c.1",	"div-clk",	54,	0x198,	136000000, mux_pllp_clkm,	MUX | DIV_U16 | PERIPH_ON_APB),
	PERIPH_CLK("i2c3",	"tegra21-i2c.2",	"div-clk",	67,	0x1b8,	136000000, mux_pllp_clkm,	MUX | DIV_U16 | PERIPH_ON_APB),
	PERIPH_CLK("i2c4",	"tegra21-i2c.3",	"div-clk",	103,	0x3c4,	136000000, mux_pllp_clkm,	MUX | DIV_U16 | PERIPH_ON_APB),
	PERIPH_CLK("i2c5",	"tegra21-i2c.4",	"div-clk",	47,	0x128,	58300000,  mux_pllp_clkm,	MUX | DIV_U16 | PERIPH_ON_APB),
	PERIPH_CLK("i2c6",	"tegra21-i2c.5",	"div-clk",	166,	0x65c,	58300000,  mux_pllp_clkm,	MUX | DIV_U16 | PERIPH_ON_APB),
	PERIPH_CLK("mipibif",	"mipibif",		NULL,	173,	0x660,	408000000,  mux_pllp_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("mipi-cal",	"mipi-cal",		NULL,	56,	0,	60000000,  mux_clk_m, PERIPH_ON_APB),
	PERIPH_CLK("mipi-cal-fixed", "mipi-cal-fixed",	NULL,	0,	0,	108000000, mux_pllp_out3,	PERIPH_NO_ENB),
	PERIPH_CLK("uarta",	"serial-tegra.0",		NULL,	6,	0x178,	800000000, mux_pllp_pllc_clkm,	MUX | DIV_U151 | DIV_U151_UART | PERIPH_ON_APB),
	PERIPH_CLK("uartb",	"serial-tegra.1",		NULL,	7,	0x17c,	800000000, mux_pllp_pllc_clkm,	MUX | DIV_U151 | DIV_U151_UART | PERIPH_ON_APB),
	PERIPH_CLK("uartc",	"serial-tegra.2",		NULL,	55,	0x1a0,	800000000, mux_pllp_pllc_clkm,	MUX | DIV_U151 | DIV_U151_UART | PERIPH_ON_APB),
	PERIPH_CLK("uartd",	"serial-tegra.3",		NULL,	65,	0x1c0,	800000000, mux_pllp_pllc_clkm,	MUX | DIV_U151 | DIV_U151_UART | PERIPH_ON_APB),
	PERIPH_CLK("uartape",	"uartape",		NULL,	212,	0x710,	50000000, mux_pllp_pllc_clkm,	MUX | DIV_U151 | DIV_U151_UART | PERIPH_NO_RESET | PERIPH_ON_APB),
	PERIPH_CLK_EX("vi",	"vi",			"vi",	20,	0x148,	600000000, mux_pllc2_c_c3_pllp_plla1_pllc4,	MUX | DIV_U71 | DIV_U71_INT, &tegra_vi_clk_ops),
	PERIPH_CLK("vi_sensor",	 NULL,			"vi_sensor",	164,	0x1a8,	408000000, mux_pllc_pllp_plla,	MUX | DIV_U71 | PERIPH_NO_RESET),
	PERIPH_CLK("vi_sensor2", NULL,			"vi_sensor2",	165,	0x658,	4080000000, mux_pllc_pllp_plla,	MUX | DIV_U71 | PERIPH_NO_RESET),
	PERIPH_CLK_EX("dtv",	"dtv",			NULL,	79,	0x1dc,	250000000, mux_clk_m,			PERIPH_ON_APB,	&tegra_dtv_clk_ops),
	PERIPH_CLK("disp1",	"tegradc.0",		NULL,	27,	0x138,	600000000, mux_pllp_plld_plla_pllc_plld2_clkm,	MUX),
	PERIPH_CLK("disp2",	"tegradc.1",		NULL,	26,	0x13c,	600000000, mux_pllp_plld_plla_pllc_plld2_clkm,	MUX),
	PERIPH_CLK_EX("sor0",	"sor0",			NULL,	182,	0x414,	408000000, mux_pllp_plld_plla_pllc_plld2_clkm,	MUX | DIV_U71, &tegra_sor_clk_ops),
	PERIPH_CLK_EX("sor1",	"sor1",			NULL,	183,	0x410,	600000000, mux_pllp_plld_plld2_clkm,	MUX | DIV_U71, &tegra_sor_clk_ops),
	PERIPH_CLK("dpaux",	"dpaux",		NULL,	181,	0,	408000000, mux_pllp,			0),
	PERIPH_CLK("dpaux1",	"dpaux1",		NULL,	207,	0,	408000000, mux_pllp,			0),
	PERIPH_CLK("usbd",	"tegra-udc.0",		NULL,	22,	0,	480000000, mux_clk_m,			0),
	PERIPH_CLK("usb2",	"tegra-ehci.1",		NULL,	58,	0,	480000000, mux_clk_m,			0),
	PERIPH_CLK("usb3",	"tegra-ehci.2",		NULL,	59,	0,	480000000, mux_clk_m,			0),
	PERIPH_CLK("hsic_trk",	"hsic_trk",		NULL,	209,	0x6cc,	38400000, mux_clk_m,			PERIPH_NO_RESET),
	PERIPH_CLK("usb2_trk",	"usb2_trk",		NULL,	210,	0x6cc,	38400000, mux_clk_m,			PERIPH_NO_RESET),
	PERIPH_CLK_EX("dsia",	"tegradc.0",		"dsia",	48,	0xd0,	500000000, mux_plld_out0,		PLLD,	&tegra_dsi_clk_ops),
	PERIPH_CLK_EX("dsib",	"tegradc.1",		"dsib",	82,	0x4b8,	500000000, mux_plld_out0,		PLLD,	&tegra_dsi_clk_ops),
	PERIPH_CLK("dsi1-fixed", "tegradc.0",		"dsi-fixed",	0,	0,	108000000, mux_pllp_out3,	PERIPH_NO_ENB),
	PERIPH_CLK("dsi2-fixed", "tegradc.1",		"dsi-fixed",	0,	0,	108000000, mux_pllp_out3,	PERIPH_NO_ENB),
	PERIPH_CLK("csi",	"vi",			"csi",	52,	0,	480000000, mux_plld,			PLLD),
	PERIPH_CLK("ispa",	"isp",			"ispa",	23,	0,	600000000, mux_isp,			PERIPH_ON_APB),
	PERIPH_CLK("ispb",	"isp",			"ispb",	3,	0,	600000000, mux_isp,			PERIPH_ON_APB),
	PERIPH_CLK("csus",	"vi",			"csus",	92,	0,	150000000, mux_clk_m,			PERIPH_NO_RESET),
	PERIPH_CLK("vim2_clk",	"vi",			"vim2_clk",	171,	0,	150000000, mux_clk_m,		PERIPH_NO_RESET),
	PERIPH_CLK("cilab",	"vi",			"cilab", 144,	0x614,	102000000, mux_pllp_pllc_clkm,		MUX | DIV_U71),
	PERIPH_CLK("cilcd",	"vi",			"cilcd", 145,	0x618,	102000000, mux_pllp_pllc_clkm,		MUX | DIV_U71),
	PERIPH_CLK("cile",	"vi",			"cile",  146,	0x61c,	102000000, mux_pllp_pllc_clkm,		MUX | DIV_U71),
	PERIPH_CLK("dsialp",	"tegradc.0",		"dsialp", 147,	0x620,	156000000, mux_pllp_pllc_clkm,		MUX | DIV_U71),
	PERIPH_CLK("dsiblp",	"tegradc.1",		"dsiblp", 148,	0x624,	156000000, mux_pllp_pllc_clkm,		MUX | DIV_U71),
	PERIPH_CLK("entropy",	"entropy",		NULL, 149,	0x628,	102000000, mux_pllp_clkm_1,		MUX | DIV_U71),
	PERIPH_CLK("clk72mhz",	"clk72mhz",		NULL, 177,	0x66c,	102000000, mux_pllp3_pllc_clkm,		MUX | DIV_U71 | PERIPH_NO_RESET),
	PERIPH_CLK("dbgapb",	"dbgapb",		NULL, 185,	0x718,	136000000, mux_pllp_clkm_2,		MUX | DIV_U71 | PERIPH_NO_RESET),

	PERIPH_CLK("tsensor",	"tegra-tsensor",	NULL,	100,	0x3b8,	216000000, mux_pllp_pllc_clkm_clk32,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("actmon",	"actmon",		NULL,	119,	0x3e8,	216000000, mux_pllp_pllc_clk32_clkm,	MUX | DIV_U71),
	PERIPH_CLK("extern1",	"extern1",		NULL,	120,	0x3ec,	216000000, mux_plla_clk32_pllp_clkm_plle,	MUX | DIV_U71),
	PERIPH_CLK("extern2",	"extern2",		NULL,	121,	0x3f0,	216000000, mux_plla_clk32_pllp_clkm_plle,	MUX | DIV_U71),
	PERIPH_CLK("extern3",	"extern3",		NULL,	122,	0x3f4,	216000000, mux_plla_clk32_pllp_clkm_plle,	MUX | DIV_U71),
	PERIPH_CLK("i2cslow",	"i2cslow",		NULL,	81,	0x3fc,	26000000,  mux_pllp_pllc_clk32_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("pcie",	"tegra-pcie",		"pcie",	70,	0,	250000000, mux_clk_m, 			0),
	PERIPH_CLK("afi",	"tegra-pcie",		"afi",	72,	0,	250000000, mux_clk_m, 			0),
	PERIPH_CLK("mselect",	"mselect",		NULL,	99,	0x3b4,	408000000, mux_pllp_clkm,		MUX | DIV_U71 | DIV_U71_INT),
	PERIPH_CLK("cl_dvfs_ref", "tegra_cl_dvfs",	"ref",	155,	0x62c,	54000000,  mux_pllp_clkm,		MUX | DIV_U71 | DIV_U71_INT | PERIPH_ON_APB),
	PERIPH_CLK("cl_dvfs_soc", "tegra_cl_dvfs",	"soc",	155,	0x630,	54000000,  mux_pllp_clkm,		MUX | DIV_U71 | DIV_U71_INT | PERIPH_ON_APB),
	PERIPH_CLK("soc_therm",	"soc_therm",		NULL,   78,	0x644,	408000000, mux_pllc_pllp_plla_pllc4,	MUX | DIV_U71 | PERIPH_ON_APB),

	PERIPH_CLK("dp2",	"dp2",			NULL,	152,	0,	38400000, mux_clk_m,			PERIPH_ON_APB),
	PERIPH_CLK("mc_bbc",	"mc_bbc",		NULL,	170,	0,	1066000000, mux_clk_mc,			PERIPH_NO_RESET),
	PERIPH_CLK("mc_capa",	"mc_capa",		NULL,	167,	0,	1066000000, mux_clk_mc,			PERIPH_NO_RESET),
	PERIPH_CLK("mc_cbpa",	"mc_cbpa",		NULL,	168,	0,	1066000000, mux_clk_mc,			PERIPH_NO_RESET),
	PERIPH_CLK("mc_cpu",	"mc_cpu",		NULL,	169,	0,	1066000000, mux_clk_mc,			PERIPH_NO_RESET),
	PERIPH_CLK("mc_ccpa",	"mc_ccpa",		NULL,	201,	0,	1066000000, mux_clk_mc,			PERIPH_NO_RESET),
	PERIPH_CLK("mc_cdpa",	"mc_cdpa",		NULL,	200,	0,	1066000000, mux_clk_mc,			PERIPH_NO_RESET),

	PERIPH_CLK_SKIP("vic03", "vic03",	NULL,	178,	0x678,	0x6f0,	500000000, mux_pllc_pllp_plla1_pllc2_c3_clkm,	MUX | DIV_U71),
	PERIPH_CLK_SKIP("msenc", "msenc",	NULL,	219,	0x6a0,	0x6e8,  768000000, mux_pllc2_c_c3_pllp_plla1_clkm,	MUX | DIV_U71 | DIV_U71_INT),
	PERIPH_CLK_SKIP("nvdec", "nvdec",	NULL,	194,	0x698,	0x6f4,	768000000, mux_pllc2_c_c3_pllp_plla1_clkm, MUX | DIV_U71 | DIV_U71_INT),
	PERIPH_CLK_SKIP("nvjpg", "nvjpg",	NULL,	195,	0x69c,	0x700,	768000000, mux_pllc2_c_c3_pllp_plla1_clkm, MUX | DIV_U71 | DIV_U71_INT),
#ifdef CONFIG_TEGRA_SE_ON_CBUS
	PERIPH_CLK_SKIP("se",	"se",		NULL,	127,	0x42c,	0x704,	600000000, mux_pllp_pllc2_c_c3_clkm,	MUX | DIV_U71 | DIV_U71_INT | PERIPH_ON_APB),
#else
	PERIPH_CLK_SKIP("se",	"tegra21-se",	NULL,	127,	0x42c,	0x704,	600000000, mux_pllp_pllc2_c_c3_clkm,	MUX | DIV_U71 | DIV_U71_INT | PERIPH_ON_APB),
#endif
	PERIPH_CLK_SKIP("tsec",	"tsec",		NULL,	83,	0x1f4,	0x708,	768000000, mux_pllp_pllc2_c_c3_clkm,	MUX | DIV_U71 | DIV_U71_INT),
	PERIPH_CLK_SKIP("tsecb", "tsecb",	NULL,	206,	0x6d8,	0x70c,	768000000, mux_pllp_pllc2_c_c3_clkm,	MUX | DIV_U71 | DIV_U71_INT),

	SHARED_CLK("avp.sclk",	"nvavp",		"sclk",	&tegra_clk_sbus_cmplx, NULL, 0, 0),
	SHARED_CLK("bsea.sclk",	"tegra-aes",		"sclk",	&tegra_clk_sbus_cmplx, NULL, 0, 0),
	SHARED_CLK("usbd.sclk",	"tegra-udc.0",		"sclk",	&tegra_clk_ahb, NULL, 0, 0),
	SHARED_CLK("usb1.sclk",	"tegra-ehci.0",		"sclk",	&tegra_clk_ahb, NULL, 0, 0),
	SHARED_CLK("usb2.sclk",	"tegra-ehci.1",		"sclk",	&tegra_clk_ahb, NULL, 0, 0),
	SHARED_CLK("usb3.sclk",	"tegra-ehci.2",		"sclk",	&tegra_clk_ahb, NULL, 0, 0),
	SHARED_CLK("sdmmc3.sclk","sdhci-tegra.2",	"sclk",	&tegra_clk_apb, NULL, 0, 0),
	SHARED_CLK("sdmmc4.sclk","sdhci-tegra.3",	"sclk",	&tegra_clk_apb, NULL, 0, 0),
	SHARED_CLK("wake.sclk",	"wake_sclk",		"sclk",	&tegra_clk_sbus_cmplx, NULL, 0, 0),
	SHARED_CLK("camera.sclk","vi",			"sclk",	&tegra_clk_sbus_cmplx, NULL, 0, 0),
	SHARED_CLK("mon.avp",	"tegra_actmon",		"avp",	&tegra_clk_sbus_cmplx, NULL, 0, 0),
	SHARED_CLK("cap.sclk",	"cap_sclk",		NULL,	&tegra_clk_sbus_cmplx, NULL, 0, SHARED_CEILING),
	SHARED_CLK("cap.throttle.sclk",	"cap_throttle",	NULL,	&tegra_clk_sbus_cmplx, NULL, 0, SHARED_CEILING),
	SHARED_CLK("floor.sclk", "floor_sclk",		NULL,	&tegra_clk_sbus_cmplx, NULL, 0, 0),
	SHARED_CLK("override.sclk", "override_sclk",    NULL,   &tegra_clk_sbus_cmplx, NULL, 0, SHARED_OVERRIDE),
	SHARED_SCLK("sbc1.sclk", "tegra12-spi.0",	"sclk", &tegra_clk_apb,        NULL, 0, 0),
	SHARED_SCLK("sbc2.sclk", "tegra12-spi.1",	"sclk", &tegra_clk_apb,        NULL, 0, 0),
	SHARED_SCLK("sbc3.sclk", "tegra12-spi.2",	"sclk", &tegra_clk_apb,        NULL, 0, 0),
	SHARED_SCLK("sbc4.sclk", "tegra12-spi.3",	"sclk", &tegra_clk_apb,        NULL, 0, 0),

	SHARED_EMC_CLK("avp.emc",	"nvavp",	"emc",	&tegra_clk_emc, NULL, 0, 0, 0),
	SHARED_EMC_CLK("cpu.emc",	"cpu",		"emc",	&tegra_clk_emc, NULL, 0, 0, 0),
	SHARED_EMC_CLK("disp1.emc",	"tegradc.0",	"emc",	&tegra_clk_emc, NULL, 0, SHARED_ISO_BW, BIT(EMC_USER_DC1)),
	SHARED_EMC_CLK("disp2.emc",	"tegradc.1",	"emc",	&tegra_clk_emc, NULL, 0, SHARED_ISO_BW, BIT(EMC_USER_DC2)),
	SHARED_EMC_CLK("usbd.emc",	"tegra-udc.0",	"emc",	&tegra_clk_emc, NULL, 0, 0, 0),
	SHARED_EMC_CLK("usb1.emc",	"tegra-ehci.0",	"emc",	&tegra_clk_emc, NULL, 0, 0, 0),
	SHARED_EMC_CLK("usb2.emc",	"tegra-ehci.1",	"emc",	&tegra_clk_emc, NULL, 0, 0, 0),
	SHARED_EMC_CLK("usb3.emc",	"tegra-ehci.2",	"emc",	&tegra_clk_emc, NULL, 0, 0, 0),
	SHARED_EMC_CLK("sdmmc3.emc",    "sdhci-tegra.2","emc",  &tegra_clk_emc, NULL, 0, 0, 0),
	SHARED_EMC_CLK("sdmmc4.emc",    "sdhci-tegra.3","emc",  &tegra_clk_emc, NULL, 0, 0, 0),
	SHARED_EMC_CLK("mon.emc",	"tegra_actmon",	"emc",	&tegra_clk_emc, NULL, 0, 0, 0),
	SHARED_EMC_CLK("cap.emc",	"cap.emc",	NULL,	&tegra_clk_emc, NULL, 0, SHARED_CEILING, 0),
	SHARED_EMC_CLK("cap.throttle.emc", "cap_throttle", NULL, &tegra_clk_emc, NULL, 0, SHARED_CEILING_BUT_ISO, 0),
	SHARED_EMC_CLK("3d.emc",	"tegra_gm20b.0",	"emc",	&tegra_clk_emc, NULL, 0, 0,		BIT(EMC_USER_3D)),
	SHARED_EMC_CLK("msenc.emc",	"tegra_msenc",	"emc",	&tegra_clk_emc, NULL, 0, SHARED_BW,	BIT(EMC_USER_MSENC)),
	SHARED_EMC_CLK("nvjpg.emc",	"tegra_nvjpg",	"emc",	&tegra_clk_emc, NULL, 0, SHARED_BW,	BIT(EMC_USER_NVJPG)),
	SHARED_EMC_CLK("nvdec.emc",	"tegra_nvdec",	"emc",	&tegra_clk_emc, NULL, 0, SHARED_BW,	BIT(EMC_USER_NVDEC)),
	SHARED_EMC_CLK("tsec.emc",	"tegra_tsec",	"emc",	&tegra_clk_emc, NULL, 0, 0, 0),
	SHARED_EMC_CLK("tsecb.emc",	"tegra_tsecb",	"emc",	&tegra_clk_emc, NULL, 0, 0, 0),
	SHARED_EMC_CLK("camera.emc", "vi",		"emc",	&tegra_clk_emc, NULL, 0, SHARED_ISO_BW,	BIT(EMC_USER_VI)),
	SHARED_EMC_CLK("iso.emc",	"iso",		"emc",	&tegra_clk_emc, NULL, 0, SHARED_ISO_BW, 0),
	SHARED_EMC_CLK("floor.emc",	"floor.emc",	NULL,	&tegra_clk_emc, NULL, 0, 0, 0),
	SHARED_EMC_CLK("override.emc", "override.emc",	NULL,	&tegra_clk_emc, NULL, 0, SHARED_OVERRIDE, 0),
	SHARED_EMC_CLK("edp.emc",	"edp.emc",	NULL,   &tegra_clk_emc, NULL, 0, SHARED_CEILING, 0),
	SHARED_EMC_CLK("vic.emc",	"tegra_vic03",	"emc",  &tegra_clk_emc, NULL, 0, 0, 0),

	DUAL_CBUS_CLK("vic03.cbus",	"tegra_vic03",		"vic03", &tegra_clk_c2bus, "vic03", 0, 0),
	SHARED_LIMIT("cap.c2bus",		"cap.c2bus",		NULL,	 &tegra_clk_c2bus, NULL,    0, SHARED_CEILING),
	SHARED_LIMIT("cap.throttle.c2bus", "cap_throttle",	NULL,	 &tegra_clk_c2bus, NULL,    0, SHARED_CEILING),
	SHARED_LIMIT("floor.c2bus",	"floor.c2bus",		NULL,	 &tegra_clk_c2bus, NULL,    0, 0),
	SHARED_CLK("override.c2bus",	"override.c2bus",	NULL,	 &tegra_clk_c2bus, NULL,  0, SHARED_OVERRIDE),
	SHARED_LIMIT("edp.c2bus",         "edp.c2bus",            NULL,   &tegra_clk_c2bus, NULL,  0, SHARED_CEILING),

	DUAL_CBUS_CLK("msenc.cbus",	"tegra_msenc",		"msenc", &tegra_clk_c3bus, "msenc", 0, 0),
	DUAL_CBUS_CLK("nvjpg.cbus",	"tegra_nvjpg",		"nvjpg", &tegra_clk_c3bus, "nvjpg", 0, 0),
	DUAL_CBUS_CLK("nvdec.cbus",	"tegra_nvdec",		"nvdec", &tegra_clk_c3bus, "nvdec", 0, 0),
	DUAL_CBUS_CLK("se.cbus",	"tegra21-se",		NULL,	 &tegra_clk_c3bus, "se",    0, 0),
	/* FIXME: move tsec out of cbus */
	DUAL_CBUS_CLK("tsec.cbus",	"tegra_tsec",		"tsec",  &tegra_clk_c3bus,  "tsec", 0, 0),
	DUAL_CBUS_CLK("tsecb.cbus",	"tegra_tsecb",		"tsecb",  &tegra_clk_c3bus,  "tsecb", 0, 0),
	SHARED_LIMIT("cap.c3bus",		"cap.c3bus",		NULL,	 &tegra_clk_c3bus, NULL,    0, SHARED_CEILING),
	SHARED_LIMIT("cap.throttle.c3bus", "cap_throttle",	NULL,	 &tegra_clk_c3bus, NULL,    0, SHARED_CEILING),
	SHARED_LIMIT("floor.c3bus",	"floor.c3bus",		NULL,	 &tegra_clk_c3bus, NULL,    0, 0),
	SHARED_CLK("override.c3bus",	"override.c3bus",	NULL,	 &tegra_clk_c3bus, NULL,  0, SHARED_OVERRIDE),

	SHARED_CLK("gm20b.gbus",	"tegra_gk20a",	"gpu",	&tegra_clk_gbus, NULL,  0, 0),
	SHARED_LIMIT("cap.gbus",		"cap.gbus",	NULL,	&tegra_clk_gbus, NULL,  0, SHARED_CEILING),
	SHARED_LIMIT("edp.gbus",		"edp.gbus",	NULL,	&tegra_clk_gbus, NULL,  0, SHARED_CEILING),
	SHARED_LIMIT("cap.throttle.gbus", "cap_throttle",	NULL,	&tegra_clk_gbus, NULL,  0, SHARED_CEILING),
	SHARED_LIMIT("cap.profile.gbus", "profile.gbus",	"cap",	&tegra_clk_gbus, NULL,  0, SHARED_CEILING),
	SHARED_CLK("override.gbus",	"override.gbus", NULL,	&tegra_clk_gbus, NULL,  0, SHARED_OVERRIDE),
	SHARED_LIMIT("floor.gbus",	"floor.gbus", NULL,	&tegra_clk_gbus, NULL,  0, 0),
	SHARED_LIMIT("floor.profile.gbus", "profile.gbus", "floor", &tegra_clk_gbus, NULL,  0, 0),

	SHARED_CLK("nv.host1x",	"tegra_host1x",		"host1x", &tegra_clk_host1x, NULL,  0, 0),
	SHARED_CLK("vi.host1x",	"tegra_vi",		"host1x", &tegra_clk_host1x, NULL,  0, 0),
	SHARED_LIMIT("cap.host1x", "cap.host1x",	NULL,	  &tegra_clk_host1x, NULL,  0, SHARED_CEILING),
	SHARED_LIMIT("floor.host1x", "floor.host1x",	NULL,	  &tegra_clk_host1x, NULL,  0, 0),
	SHARED_CLK("override.host1x", "override.host1x", NULL,	  &tegra_clk_host1x, NULL,  0, SHARED_OVERRIDE),
};

/* VI, ISP buses */
static struct clk tegra_visp_clks[] = {
	SHARED_CONNECT("vi.cbus",	"vi.cbus",	NULL,	&tegra_clk_cbus,   "vi",    0, 0),
	SHARED_CONNECT("isp.cbus",	"isp.cbus",	NULL,	&tegra_clk_cbus,   "isp",   0, 0),
	SHARED_CLK("override.cbus",	"override.cbus", NULL,	&tegra_clk_cbus,    NULL,   0, SHARED_OVERRIDE),

#ifndef CONFIG_VI_ONE_DEVICE
	SHARED_CLK("via.vi.cbus",	"via.vi",	NULL,	&tegra_visp_clks[0], NULL,   0, 0),
	SHARED_CLK("vib.vi.cbus",	"vib.vi",	NULL,	&tegra_visp_clks[0], NULL,   0, 0),
#endif

	SHARED_CLK("ispa.isp.cbus",	"ispa.isp",	NULL,	&tegra_visp_clks[1], "ispa", 0, 0),
	SHARED_CLK("ispb.isp.cbus",	"ispb.isp",	NULL,	&tegra_visp_clks[1], "ispb", 0, 0),
};

/* XUSB clocks */
#define XUSB_ID "tegra-xhci"
/* xusb common clock gate - enabled on init and never disabled */
static void tegra21_xusb_gate_clk_init(struct clk *c)
{
	tegra21_periph_clk_enable(c);
}

static struct clk_ops tegra_xusb_gate_clk_ops = {
	.init    = tegra21_xusb_gate_clk_init,
};

static struct clk tegra_clk_xusb_gate = {
	.name      = "xusb_gate",
	.flags     = ENABLE_ON_INIT | PERIPH_NO_RESET,
	.ops       = &tegra_xusb_gate_clk_ops,
	.rate      = 12000000,
	.max_rate  = 48000000,
	.u.periph = {
		.clk_num   = 143,
	},
};

static struct clk tegra_xusb_source_clks[] = {
	PERIPH_CLK("xusb_host_src",	XUSB_ID, "host_src",	143,	0x600,	120000000, mux_clkm_pllp_pllc_pllre,	MUX | DIV_U71 | PERIPH_NO_RESET | PERIPH_ON_APB),
	PERIPH_CLK("xusb_falcon_src",	XUSB_ID, "falcon_src",	143,	0x604,	350000000, mux_clkm_pllp_pllc_pllre,	MUX | DIV_U71 | PERIPH_NO_RESET),
	PERIPH_CLK("xusb_fs_src",	XUSB_ID, "fs_src",	143,	0x608,	 48000000, mux_clkm_48M_pllp_480M,	MUX | DIV_U71 | PERIPH_NO_RESET),
	PERIPH_CLK("xusb_ss_src",	XUSB_ID, "ss_src",	143,	0x610,	120000000, mux_clkm_pllre_clk32_480M_pllc_ref,	MUX | DIV_U71 | PERIPH_NO_RESET),
	PERIPH_CLK("xusb_dev_src",	XUSB_ID, "dev_src",	95,	0x60c,	120000000, mux_clkm_pllp_pllc_pllre,	MUX | DIV_U71 | PERIPH_NO_RESET | PERIPH_ON_APB),
	SHARED_EMC_CLK("xusb.emc",	XUSB_ID, "emc",	&tegra_clk_emc,	NULL,	0,	SHARED_BW, 0),
};

static struct clk tegra_xusb_ss_div2 = {
	.name      = "xusb_ss_div2",
	.ops       = &tegra_clk_m_div_ops,
	.parent    = &tegra_xusb_source_clks[3],
	.mul       = 1,
	.div       = 2,
	.state     = OFF,
	.max_rate  = 61200000,
};

static struct clk_mux_sel mux_ss_div2_pllu_60M[] = {
	{ .input = &tegra_xusb_ss_div2, .value = 0},
	{ .input = &tegra_pll_u_60M,    .value = 1},
	{ 0, 0},
};

static struct clk tegra_xusb_hs_src = {
	.name      = "xusb_hs_src",
	.lookup    = {
		.dev_id    = XUSB_ID,
		.con_id	   = "hs_src",
	},
	.ops       = &tegra_periph_clk_ops,
	.reg       = 0x610,
	.inputs    = mux_ss_div2_pllu_60M,
	.flags     = MUX | PLLU | PERIPH_NO_ENB,
	.max_rate  = 61200000,
	.u.periph = {
		.src_mask  = 0x1 << 25,
		.src_shift = 25,
	},
};

static struct clk_mux_sel mux_xusb_host[] = {
	{ .input = &tegra_xusb_source_clks[0], .value = 0},
	{ .input = &tegra_xusb_source_clks[1], .value = 1},
	{ .input = &tegra_xusb_source_clks[2], .value = 2},
	{ .input = &tegra_xusb_hs_src,         .value = 5},
	{ 0, 0},
};

static struct clk_mux_sel mux_xusb_ss[] = {
	{ .input = &tegra_xusb_source_clks[3], .value = 3},
	{ .input = &tegra_xusb_source_clks[0], .value = 0},
	{ .input = &tegra_xusb_source_clks[1], .value = 1},
	{ 0, 0},
};

static struct clk_mux_sel mux_xusb_dev[] = {
	{ .input = &tegra_xusb_source_clks[4], .value = 4},
	{ .input = &tegra_xusb_source_clks[2], .value = 2},
	{ .input = &tegra_xusb_source_clks[3], .value = 3},
	{ 0, 0},
};

static struct clk tegra_xusb_coupled_clks[] = {
	PERIPH_CLK_EX("xusb_host", XUSB_ID, "host", 89,	0, 350000000, mux_xusb_host, 0,	&tegra_clk_coupled_gate_ops),
	PERIPH_CLK_EX("xusb_ss",   XUSB_ID, "ss",  156,	0, 350000000, mux_xusb_ss,   0,	&tegra_clk_coupled_gate_ops),
	PERIPH_CLK_EX("xusb_dev",  XUSB_ID, "dev",  95, 0, 120000000, mux_xusb_dev,  0,	&tegra_clk_coupled_gate_ops),
};

#define CLK_DUPLICATE(_name, _dev, _con)		\
	{						\
		.name	= _name,			\
		.lookup	= {				\
			.dev_id	= _dev,			\
			.con_id		= _con,		\
		},					\
	}

/* Some clocks may be used by different drivers depending on the board
 * configuration.  List those here to register them twice in the clock lookup
 * table under two names.
 */
struct clk_duplicate tegra_clk_duplicates[] = {
	CLK_DUPLICATE("pll_u_out",  NULL, "pll_u"),
	CLK_DUPLICATE("uarta",  "serial8250.0", NULL),
	CLK_DUPLICATE("uartb",  "serial8250.1", NULL),
	CLK_DUPLICATE("uartc",  "serial8250.2", NULL),
	CLK_DUPLICATE("uartd",  "serial8250.3", NULL),
	CLK_DUPLICATE("usbd", "utmip-pad", NULL),
	CLK_DUPLICATE("usbd", "tegra-ehci.0", NULL),
	CLK_DUPLICATE("usbd", "tegra-otg", NULL),
	CLK_DUPLICATE("disp1", "tegra_dc_dsi_vs1.0", NULL),
	CLK_DUPLICATE("disp1.emc", "tegra_dc_dsi_vs1.0", "emc"),
	CLK_DUPLICATE("disp1", "tegra_dc_dsi_vs1.1", NULL),
	CLK_DUPLICATE("disp1.emc", "tegra_dc_dsi_vs1.1", "emc"),
	CLK_DUPLICATE("dsib", "tegradc.0", "dsib"),
	CLK_DUPLICATE("dsia", "tegradc.1", "dsia"),
	CLK_DUPLICATE("dsiblp", "tegradc.0", "dsiblp"),
	CLK_DUPLICATE("dsialp", "tegradc.1", "dsialp"),
	CLK_DUPLICATE("dsia", "tegra_dc_dsi_vs1.0", "dsia"),
	CLK_DUPLICATE("dsia", "tegra_dc_dsi_vs1.1", "dsia"),
	CLK_DUPLICATE("dsialp", "tegra_dc_dsi_vs1.0", "dsialp"),
	CLK_DUPLICATE("dsialp", "tegra_dc_dsi_vs1.1", "dsialp"),
	CLK_DUPLICATE("dsi1-fixed", "tegra_dc_dsi_vs1.0", "dsi-fixed"),
	CLK_DUPLICATE("dsi1-fixed", "tegra_dc_dsi_vs1.1", "dsi-fixed"),
	CLK_DUPLICATE("pwm", "tegra_pwm.0", NULL),
	CLK_DUPLICATE("pwm", "tegra_pwm.1", NULL),
	CLK_DUPLICATE("pwm", "tegra_pwm.2", NULL),
	CLK_DUPLICATE("pwm", "tegra_pwm.3", NULL),
	CLK_DUPLICATE("cop", "nvavp", "cop"),
	CLK_DUPLICATE("bsev", "nvavp", "bsev"),
	CLK_DUPLICATE("bsea", "tegra-aes", "bsea"),
	CLK_DUPLICATE("cml1", "tegra_sata_cml", NULL),
	CLK_DUPLICATE("cml0", "tegra_pcie", "cml"),
	CLK_DUPLICATE("pciex", "tegra_pcie", "pciex"),
	CLK_DUPLICATE("pex_uphy", "tegra_pcie", "pex_uphy"),
	CLK_DUPLICATE("clk_m", NULL, "apb_pclk"),
	CLK_DUPLICATE("i2c1", "tegra-i2c-slave.0", NULL),
	CLK_DUPLICATE("i2c2", "tegra-i2c-slave.1", NULL),
	CLK_DUPLICATE("i2c3", "tegra-i2c-slave.2", NULL),
	CLK_DUPLICATE("i2c4", "tegra-i2c-slave.3", NULL),
	CLK_DUPLICATE("i2c5", "tegra-i2c-slave.4", NULL),
	CLK_DUPLICATE("cl_dvfs_ref", "tegra21-i2c.4", NULL),
	CLK_DUPLICATE("cl_dvfs_soc", "tegra21-i2c.4", NULL),
	CLK_DUPLICATE("sbc1", "tegra11-spi-slave.0", NULL),
	CLK_DUPLICATE("sbc2", "tegra11-spi-slave.1", NULL),
	CLK_DUPLICATE("sbc3", "tegra11-spi-slave.2", NULL),
	CLK_DUPLICATE("sbc4", "tegra11-spi-slave.3", NULL),
	CLK_DUPLICATE("i2c5", "tegra_cl_dvfs", "i2c"),
	CLK_DUPLICATE("cpu_g", "tegra_cl_dvfs", "safe_dvfs"),
	CLK_DUPLICATE("actmon", "tegra_host1x", "actmon"),
	CLK_DUPLICATE("gpu_ref", "tegra_gm20b.0", "PLLG_ref"),
	CLK_DUPLICATE("gbus", "tegra_gm20b.0", "PLLG_out"),
	CLK_DUPLICATE("pll_p_out5", "tegra_gm20b.0", "pwr"),
	CLK_DUPLICATE("ispa.isp.cbus", "tegra_isp", "isp"),
	CLK_DUPLICATE("ispb.isp.cbus", "tegra_isp.1", "isp"),
#ifdef CONFIG_VI_ONE_DEVICE
	CLK_DUPLICATE("vi.cbus", "tegra_vi", "vi"),
	CLK_DUPLICATE("csi", "tegra_vi", "csi"),
	CLK_DUPLICATE("csus", "tegra_vi", "csus"),
	CLK_DUPLICATE("vim2_clk", "tegra_vi", "vim2_clk"),
	CLK_DUPLICATE("cilab", "tegra_vi", "cilab"),
	CLK_DUPLICATE("cilcd", "tegra_vi", "cilcd"),
	CLK_DUPLICATE("cile", "tegra_vi", "cile"),
#else
	CLK_DUPLICATE("via.vi.cbus", "tegra_vi", "vi"),
	CLK_DUPLICATE("vib.vi.cbus", "tegra_vi.1", "vi"),
	CLK_DUPLICATE("csi", "tegra_vi", "csi"),
	CLK_DUPLICATE("csi", "tegra_vi.1", "csi"),
	CLK_DUPLICATE("csus", "tegra_vi", "csus"),
	CLK_DUPLICATE("vim2_clk", "tegra_vi.1", "vim2_clk"),
	CLK_DUPLICATE("cilab", "tegra_vi", "cilab"),
	CLK_DUPLICATE("cilcd", "tegra_vi.1", "cilcd"),
	CLK_DUPLICATE("cile", "tegra_vi.1", "cile"),
#endif
	CLK_DUPLICATE("spdif_in", NULL, "spdif_in"),
	CLK_DUPLICATE("dmic1", "tegra-dmic.0", NULL),
	CLK_DUPLICATE("dmic2", "tegra-dmic.1", NULL),
	CLK_DUPLICATE("dmic3", "tegra-dmic.2", NULL),
	CLK_DUPLICATE("d_audio", "tegra210-adma", NULL),
	CLK_DUPLICATE("mclk", NULL, "default_mclk"),
};

struct clk *tegra_ptr_clks[] = {
	&tegra_clk_32k,
	&tegra_clk_osc,
	&tegra_clk_m,
	&tegra_clk_m_div2,
	&tegra_clk_m_div4,
	&tegra_pll_ref,
	&tegra_pll_m,
	&tegra_pll_c,
	&tegra_pll_c_out1,
	&tegra_pll_c2,
	&tegra_pll_c3,
	&tegra_pll_a1,
	&tegra_pll_p,
	&tegra_pll_p_out1,
	&tegra_pll_p_out2,
	&tegra_pll_p_out3,
	&tegra_pll_p_out4,
	&tegra_pll_p_out5,
	&tegra_pll_a,
	&tegra_pll_a_out0,
	&tegra_pll_d,
	&tegra_pll_d_out0,
	&tegra_clk_xusb_gate,
	&tegra_pll_u_vco,
	&tegra_pll_u_out,
	&tegra_pll_u_out1,
	&tegra_pll_u_out2,
	&tegra_pll_u_480M,
	&tegra_pll_u_60M,
	&tegra_pll_u_48M,
	&tegra_pll_x,
	&tegra_pll_x_out0,
	&tegra_dfll_cpu,
	&tegra_pll_d2,
	&tegra_pll_c4_vco,
	&tegra_pll_c4_out0,
	&tegra_pll_c4_out1,
	&tegra_pll_c4_out2,
	&tegra_pll_c4_out3,
	&tegra_pll_dp,
	&tegra_pll_re_vco,
	&tegra_pll_re_out,
	&tegra_pll_re_out1,
	&tegra_pll_e,
	&tegra_cml0_clk,
	&tegra_cml1_clk,
	&tegra_pciex_clk,
	&tegra_pex_uphy_clk,
	&tegra_clk_cclk_g,
	&tegra_clk_sclk_mux,
	&tegra_clk_sclk_div,
	&tegra_clk_sclk,
	&tegra_clk_hclk,
	&tegra_clk_pclk,
	&tegra_clk_aclk_adsp,
	&tegra_clk_virtual_cpu_g,
	&tegra_clk_cpu_cmplx,
	&tegra_clk_blink,
	&tegra_clk_cop,
	&tegra_clk_sbus_cmplx,
	&tegra_clk_ahb,
	&tegra_clk_apb,
	&tegra_clk_emc,
	&tegra_clk_mc,
	&tegra_clk_host1x,
	&tegra_clk_c2bus,
	&tegra_clk_c3bus,
	&tegra_clk_gpu,
	&tegra_clk_gbus,
	&tegra_clk_isp,
	&tegra_clk_cbus,
};

struct clk *tegra_ptr_camera_mclks[] = {
	&tegra_camera_mclk,
	&tegra_camera_mclk2,
};

/* Return true from this function if the target rate can be locked without
   switching pll clients to back-up source */
static bool tegra21_is_dyn_ramp(
	struct clk *c, unsigned long rate, bool from_vco_min)
{
#if PLLCX_USE_DYN_RAMP
	/* PLLC2, PLLC3 support dynamic ramp only when output divider <= 8 */
	if ((c == &tegra_pll_c) || (c == &tegra_pll_c2) ||
	    (c == &tegra_pll_c3) || (c == &tegra_pll_a1)) {
		struct clk_pll_freq_table cfg, old_cfg;
		unsigned long input_rate = clk_get_rate(c->parent);

		pll_base_parse_cfg(c, &old_cfg);

		if (!pll_dyn_ramp_find_cfg(c, &cfg, rate, input_rate, NULL)) {
			if ((cfg.m == old_cfg.m) && (cfg.p == old_cfg.p))
				return c->u.pll.defaults_set;
		}
	}
#endif

#if PLLX_USE_DYN_RAMP
	/* PPLX, PLLC support dynamic ramp when changing NDIV only */
	if (c == &tegra_pll_x) {
		struct clk_pll_freq_table cfg, old_cfg;
		unsigned long input_rate = clk_get_rate(c->parent);

		if (from_vco_min) {
			old_cfg.m = PLL_FIXED_MDIV(c, input_rate);
			old_cfg.p = 1;
		} else {
			u32 val = clk_readl(c->reg + PLL_BASE);
			PLL_BASE_PARSE(PLLXC, old_cfg, val);
			old_cfg.p = pllxc_p[old_cfg.p];
		}

		if (!pll_dyn_ramp_find_cfg(c, &cfg, rate, input_rate, NULL)) {
			if ((cfg.m == old_cfg.m) && (cfg.p == old_cfg.p))
				return true;
		}
	}
#endif
	return false;
}

/*
 * Backup pll is used as transitional CPU clock source while main pll is
 * relocking; in addition all CPU rates below backup level are sourced from
 * backup pll only. Target backup levels for each CPU mode are selected high
 * enough to avoid voltage droop when CPU clock is switched between backup and
 * main plls. Actual backup rates will be rounded to match backup source fixed
 * frequency. Backup rates are also used as stay-on-backup thresholds, and must
 * be kept the same in G and LP mode (will need to add a separate stay-on-backup
 * parameter to allow different backup rates if necessary).
 *
 * Sbus threshold must be exact factor of pll_p rate.
 */
#define CPU_G_BACKUP_RATE_TARGET	200000000
#define CPU_LP_BACKUP_RATE_TARGET	200000000

static void tegra21_pllp_init_dependencies(unsigned long pllp_rate)
{
	u32 div;
	unsigned long backup_rate;

	switch (pllp_rate) {
	case 216000000:
		tegra_pll_p_out1.u.pll_div.default_rate = 28800000;
		tegra_pll_p_out3.u.pll_div.default_rate = 72000000;
		tegra_clk_host1x.u.periph.threshold = 108000000;
		break;
	case 408000000:
		tegra_pll_p_out1.u.pll_div.default_rate = 9600000;
		tegra_pll_p_out3.u.pll_div.default_rate = 102000000;
		tegra_clk_host1x.u.periph.threshold = 204000000;
		break;
	case 204000000:
		tegra_pll_p_out1.u.pll_div.default_rate = 4800000;
		tegra_pll_p_out3.u.pll_div.default_rate = 102000000;
		tegra_clk_host1x.u.periph.threshold = 204000000;
		break;
	default:
		pr_err("tegra: PLLP rate: %lu is not supported\n", pllp_rate);
		BUG();
	}
	pr_info("tegra: PLLP fixed rate: %lu\n", pllp_rate);

	div = pllp_rate / CPU_G_BACKUP_RATE_TARGET;
	backup_rate = pllp_rate / div;
	tegra_clk_virtual_cpu_g.u.cpu.backup_rate = backup_rate;

	div = pllp_rate / CPU_LP_BACKUP_RATE_TARGET;
	backup_rate = pllp_rate / div;
}

static void tegra21_init_one_clock(struct clk *c)
{
	clk_init(c);
	INIT_LIST_HEAD(&c->shared_bus_list);
	if (!c->lookup.dev_id && !c->lookup.con_id)
		c->lookup.con_id = c->name;
	c->lookup.clk = c;
	clkdev_add(&c->lookup);
}

void tegra_edp_throttle_cpu_now(u8 factor)
{
	/* empty definition for tegra21 */
	return;
}

bool tegra_clk_is_parent_allowed(struct clk *c, struct clk *p)
{
	/*
	 * Most of the Tegra21 multimedia and peripheral muxes include pll_c2
	 * and pll_c3 as possible inputs. However, per clock policy these plls
	 * are allowed to be used only by handful devices aggregated on cbus.
	 * For all others, instead of enforcing policy at run-time in this
	 * function, we simply stripped out pll_c2 and pll_c3 options from the
	 * respective muxes statically. Similarly pll_a1 is removed from the
	 * set of possible parents of non-cbus  modules.
	 *
	 * Traditionally ubiquitous on tegra chips pll_c is allowed to be used
	 * as parent for cbus modules only as well, but it is not stripped out
	 * from muxes statically, and the respective policy is enforced by this
	 * function.
	 */
	if ((p == &tegra_pll_c) && (c != &tegra_pll_c_out1))
		return c->flags & PERIPH_ON_CBUS;

	/*
	 * On Tegra21 in any configuration pll_m must be used as a clock source
	 * for EMC only.
	 */
	if (p == &tegra_pll_m)
		return c->flags & PERIPH_EMC_ENB;

	return true;
}

/* Internal LA may request some clocks to be enabled on init via TRANSACTION
   SCRATCH register settings */
void __init tegra21x_clk_init_la(void)
{
	struct clk *c;
	u32 reg = readl((void *)
		((uintptr_t)misc_gp_base + MISC_GP_TRANSACTOR_SCRATCH_0));

	if (!(reg & MISC_GP_TRANSACTOR_SCRATCH_LA_ENABLE))
		return;

	c = tegra_get_clock_by_name("la");
	if (WARN(!c, "%s: could not find la clk\n", __func__))
		return;
	clk_enable(c);

	if (reg & MISC_GP_TRANSACTOR_SCRATCH_DP2_ENABLE) {
		c = tegra_get_clock_by_name("dp2");
		if (WARN(!c, "%s: could not find dp2 clk\n", __func__))
			return;
		clk_enable(c);
	}
}

#ifdef CONFIG_CPU_FREQ

/*
 * Frequency table index must be sequential starting at 0 and frequencies
 * must be ascending.
 */
#define CPU_FREQ_STEP 102000 /* 102MHz cpu_g table step */
#define CPU_FREQ_TABLE_MAX_SIZE (2 * MAX_DVFS_FREQS + 1)

static struct cpufreq_frequency_table freq_table[CPU_FREQ_TABLE_MAX_SIZE];
static struct tegra_cpufreq_table_data freq_table_data;

struct tegra_cpufreq_table_data *tegra_cpufreq_table_get(void)
{
	int i, j;
	bool g_vmin_done = false;
	unsigned int freq, lp_backup_freq, g_vmin_freq, g_start_freq, max_freq;
	struct clk *cpu_clk_g = tegra_get_clock_by_name("cpu_g");
	struct clk *cpu_clk_lp = tegra_get_clock_by_name("cpu_lp");

	/* Initialize once */
	if (freq_table_data.freq_table)
		return &freq_table_data;

	/* Clean table */
	for (i = 0; i < CPU_FREQ_TABLE_MAX_SIZE; i++) {
		freq_table[i].index = i;
		freq_table[i].frequency = CPUFREQ_TABLE_END;
	}

	lp_backup_freq = cpu_clk_lp->u.cpu.backup_rate / 1000;
	if (!lp_backup_freq) {
		WARN(1, "%s: cannot make cpufreq table: no LP CPU backup rate\n",
		     __func__);
		return NULL;
	}
	if (!cpu_clk_lp->dvfs) {
		WARN(1, "%s: cannot make cpufreq table: no LP CPU dvfs\n",
		     __func__);
		return NULL;
	}
	if (!cpu_clk_g->dvfs) {
		WARN(1, "%s: cannot make cpufreq table: no G CPU dvfs\n",
		     __func__);
		return NULL;
	}
	g_vmin_freq = cpu_clk_g->dvfs->freqs[0] / 1000;
	if (g_vmin_freq <= lp_backup_freq) {
		WARN(1, "%s: cannot make cpufreq table: LP CPU backup rate"
			" exceeds G CPU rate at Vmin\n", __func__);
		return NULL;
	}

	/* Start with backup frequencies */
	i = 0;
	freq = lp_backup_freq;
	freq_table[i++].frequency = freq/4;
	freq_table[i++].frequency = freq/2;
	freq_table[i++].frequency = freq;

	/* Throttle low index at backup level*/
	freq_table_data.throttle_lowest_index = i - 1;

	/*
	 * Next, set table steps along LP CPU dvfs ladder, but make sure G CPU
	 * dvfs rate at minimum voltage is not missed (if it happens to be below
	 * LP maximum rate)
	 */
	max_freq = cpu_clk_lp->max_rate / 1000;
	for (j = 0; j < cpu_clk_lp->dvfs->num_freqs; j++) {
		freq = cpu_clk_lp->dvfs->freqs[j] / 1000;
		if (freq <= lp_backup_freq)
			continue;

		if (!g_vmin_done && (freq >= g_vmin_freq)) {
			g_vmin_done = true;
			if (freq > g_vmin_freq)
				freq_table[i++].frequency = g_vmin_freq;
		}
		freq_table[i++].frequency = freq;

		if (freq == max_freq)
			break;
	}

	/* Set G CPU min rate at least one table step below LP maximum */
	cpu_clk_g->min_rate = min(freq_table[i-2].frequency, g_vmin_freq)*1000;

	/* Suspend index at max LP CPU */
	freq_table_data.suspend_index = i - 1;

	/* Fill in "hole" (if any) between LP CPU maximum rate and G CPU dvfs
	   ladder rate at minimum voltage */
	if (freq < g_vmin_freq) {
		int n = (g_vmin_freq - freq) / CPU_FREQ_STEP;
		for (j = 0; j <= n; j++) {
			freq = g_vmin_freq - CPU_FREQ_STEP * (n - j);
			freq_table[i++].frequency = freq;
		}
	}

	/* Now, step along the rest of G CPU dvfs ladder */
	g_start_freq = freq;
	max_freq = cpu_clk_g->max_rate / 1000;
	for (j = 0; j < cpu_clk_g->dvfs->num_freqs; j++) {
		freq = cpu_clk_g->dvfs->freqs[j] / 1000;
		if (freq > g_start_freq)
			freq_table[i++].frequency = freq;
		if (freq == max_freq)
			break;
	}

	/* Throttle high index one step below maximum */
	BUG_ON(i >= CPU_FREQ_TABLE_MAX_SIZE);
	freq_table_data.throttle_highest_index = i - 2;
	freq_table_data.freq_table = freq_table;
	return &freq_table_data;
}

unsigned long tegra_emc_to_cpu_ratio(unsigned long cpu_rate)
{
	static unsigned long emc_max_rate;

	if (emc_max_rate == 0)
		emc_max_rate = clk_round_rate(
			tegra_get_clock_by_name("emc"), ULONG_MAX);

	/* Vote on memory bus frequency based on cpu frequency;
	   cpu rate is in kHz, emc rate is in Hz */
	if (cpu_rate >= 1300000)
		return emc_max_rate;	/* cpu >= 1.3GHz, emc max */
	else if (cpu_rate >= 975000)
		return 400000000;	/* cpu >= 975 MHz, emc 400 MHz */
	else if (cpu_rate >= 725000)
		return  200000000;	/* cpu >= 725 MHz, emc 200 MHz */
	else if (cpu_rate >= 500000)
		return  100000000;	/* cpu >= 500 MHz, emc 100 MHz */
	else if (cpu_rate >= 275000)
		return  50000000;	/* cpu >= 275 MHz, emc 50 MHz */
	else
		return 0;		/* emc min */
}

int tegra_update_mselect_rate(unsigned long cpu_rate)
{
	static struct clk *mselect; /* statics init to 0 */

	unsigned long mselect_rate;

	if (!mselect) {
		mselect = tegra_get_clock_by_name("mselect");
		if (!mselect)
			return -ENODEV;
	}

	/* Vote on mselect frequency based on cpu frequency:
	   keep mselect at half of cpu rate up to 102 MHz;
	   cpu rate is in kHz, mselect rate is in Hz */
	mselect_rate = DIV_ROUND_UP(cpu_rate, 2) * 1000;
	mselect_rate = min(mselect_rate, 102000000UL);

	if (mselect_rate != clk_get_rate(mselect))
		return clk_set_rate(mselect, mselect_rate);

	return 0;
}
#endif

#ifdef CONFIG_PM_SLEEP
static u32 clk_rst_suspend[RST_DEVICES_NUM + CLK_OUT_ENB_NUM +
			   PERIPH_CLK_SOURCE_NUM + 25];

static int tegra21_clk_suspend(void)
{
	unsigned long off;
	u32 *ctx = clk_rst_suspend;

	*ctx++ = clk_readl(OSC_CTRL) & OSC_CTRL_MASK;
	*ctx++ = clk_readl(CPU_SOFTRST_CTRL);
	*ctx++ = clk_readl(CPU_SOFTRST_CTRL1);
	*ctx++ = clk_readl(CPU_SOFTRST_CTRL2);

	*ctx++ = clk_readl(tegra_pll_p_out1.reg);
	*ctx++ = clk_readl(tegra_pll_p_out3.reg);
	*ctx++ = clk_readl(tegra_pll_u_out1.reg);
	*ctx++ = clk_readl(tegra_pll_u_vco.reg);

	*ctx++ = clk_readl(tegra_pll_a_out0.reg);
	*ctx++ = clk_readl(tegra_pll_c_out1.reg);
	*ctx++ = clk_readl(tegra_pll_c4_out3.reg);
	*ctx++ = clk_readl(tegra_pll_re_out1.reg);

	*ctx++ = clk_readl(tegra_clk_pclk.reg);
	*ctx++ = clk_readl(tegra_clk_sclk.reg);
	*ctx++ = clk_readl(tegra_clk_sclk_div.reg);
	*ctx++ = clk_readl(tegra_clk_sclk_mux.reg);

	for (off = PERIPH_CLK_SOURCE_I2S1; off <= PERIPH_CLK_SOURCE_LA;
			off += 4) {
		if (off == PERIPH_CLK_SOURCE_EMC)
			continue;
		*ctx++ = clk_readl(off);
	}
	for (off = PERIPH_CLK_SOURCE_MSELECT; off <= PERIPH_CLK_SOURCE_SE;
			off+=4) {
		*ctx++ = clk_readl(off);
	}
	for (off = AUDIO_DLY_CLK; off <= AUDIO_SYNC_CLK_SPDIF; off+=4) {
		*ctx++ = clk_readl(off);
	}
	for (off = PERIPH_CLK_SOURCE_XUSB_HOST;
		off <= PERIPH_CLK_SOURCE_VIC; off += 4)
		*ctx++ = clk_readl(off);

	*ctx++ = clk_readl(RST_DEVICES_L);
	*ctx++ = clk_readl(RST_DEVICES_H);
	*ctx++ = clk_readl(RST_DEVICES_U);
	*ctx++ = clk_readl(RST_DEVICES_V);
	*ctx++ = clk_readl(RST_DEVICES_W);
	*ctx++ = clk_readl(RST_DEVICES_X);
	*ctx++ = clk_readl(RST_DEVICES_Y);

	*ctx++ = clk_readl(CLK_OUT_ENB_L);
	*ctx++ = clk_readl(CLK_OUT_ENB_H);
	*ctx++ = clk_readl(CLK_OUT_ENB_U);
	*ctx++ = clk_readl(CLK_OUT_ENB_V);
	*ctx++ = clk_readl(CLK_OUT_ENB_W);
	*ctx++ = clk_readl(CLK_OUT_ENB_X);
	*ctx++ = clk_readl(CLK_OUT_ENB_Y);

	*ctx++ = clk_readl(tegra_clk_cclk_g.reg);
	*ctx++ = clk_readl(tegra_clk_cclk_g.reg + SUPER_CLK_DIVIDER);

	*ctx++ = clk_readl(SPARE_REG);
	*ctx++ = clk_readl(MISC_CLK_ENB);
	*ctx++ = clk_readl(CLK_MASK_ARM);

	return 0;
}

static void tegra21_clk_resume(void)
{
	unsigned long off;
	const u32 *ctx = clk_rst_suspend;
	u32 val, pll_u_mask, pll_u_base;
	u32 pll_p_out12, pll_p_out34, pll_u_out12;
	u32 pll_a_out0, pll_c_out1, pll_c4_out3, pll_re_out1;
	struct clk *p;

	pll_u_mask = (1 << tegra_pll_u_480M.reg_shift) |
		(1 << tegra_pll_u_60M.reg_shift) |
		(1 << tegra_pll_u_48M.reg_shift);

	/* FIXME: OSC_CTRL already restored by warm boot code? */
	val = clk_readl(OSC_CTRL) & ~OSC_CTRL_MASK;
	val |= *ctx++;
	clk_writel(val, OSC_CTRL);
	clk_writel(*ctx++, CPU_SOFTRST_CTRL);
	clk_writel(*ctx++, CPU_SOFTRST_CTRL1);
	clk_writel(*ctx++, CPU_SOFTRST_CTRL2);

	/* Since we are going to reset devices and switch clock sources in this
	 * function, plls and secondary dividers is required to be enabled. The
	 * actual value will be restored back later. Note that boot plls: pllm,
	 * and pllp, are already configured and enabled
	 */
	val = PLL_OUT_CLKEN | PLL_OUT_RESET_DISABLE;
	val |= val << 16;
	pll_p_out12 = *ctx++;
	clk_writel(pll_p_out12 | val, tegra_pll_p_out1.reg);
	pll_p_out34 = *ctx++;
	clk_writel(pll_p_out34 | val, tegra_pll_p_out3.reg);

	tegra_pllu_out_resume_enable(&tegra_pll_u_out);
	pll_u_out12 = *ctx++;
	clk_writel(pll_u_out12 | val, tegra_pll_u_out1.reg);
	pll_u_base = *ctx++;
	val = clk_readl(tegra_pll_u_vco.reg) | pll_u_mask;
	clk_writel(val, tegra_pll_u_vco.reg);

	tegra_pll_clk_resume_enable(&tegra_pll_c);
	tegra_pll_clk_resume_enable(&tegra_pll_c2);
	tegra_pll_clk_resume_enable(&tegra_pll_c3);
	tegra_pll_clk_resume_enable(&tegra_pll_a1);
	tegra_pll_clk_resume_enable(&tegra_pll_a);
	tegra_pll_clk_resume_enable(&tegra_pll_d);
	tegra_pll_clk_resume_enable(&tegra_pll_d2);
	tegra_pll_clk_resume_enable(&tegra_pll_dp);
	tegra_pll_clk_resume_enable(&tegra_pll_x);
	tegra_pll_out_resume_enable(&tegra_pll_c4_out0);
	tegra_pll_out_resume_enable(&tegra_pll_re_out);

	udelay(1000);

	val = PLL_OUT_CLKEN | PLL_OUT_RESET_DISABLE;
	pll_a_out0 = *ctx++;
	clk_writel(pll_a_out0 | val, tegra_pll_a_out0.reg);
	pll_c_out1 = *ctx++;
	clk_writel(pll_c_out1 | val, tegra_pll_c_out1.reg);
	pll_c4_out3 = *ctx++;
	clk_writel(pll_c4_out3 | val, tegra_pll_c4_out3.reg);
	pll_re_out1 = *ctx++;
	clk_writel(pll_re_out1 | val, tegra_pll_re_out1.reg);

	/* FIXME: the oder here to be checked against boot-rom exit state */
	clk_writel(*ctx++, tegra_clk_pclk.reg);
	clk_writel(*ctx++, tegra_clk_sclk.reg);
	clk_writel(*ctx++, tegra_clk_sclk_div.reg);
	clk_writel(*ctx++, tegra_clk_sclk_mux.reg);

	/* enable all clocks before configuring clock sources */
	clk_writel(CLK_OUT_ENB_L_RESET_MASK, CLK_OUT_ENB_L);
	clk_writel(CLK_OUT_ENB_H_RESET_MASK, CLK_OUT_ENB_H);
	clk_writel(CLK_OUT_ENB_U_RESET_MASK, CLK_OUT_ENB_U);
	clk_writel(CLK_OUT_ENB_V_RESET_MASK, CLK_OUT_ENB_V);
	clk_writel(CLK_OUT_ENB_W_RESET_MASK, CLK_OUT_ENB_W);
	clk_writel(CLK_OUT_ENB_X_RESET_MASK, CLK_OUT_ENB_X);
	clk_writel(CLK_OUT_ENB_Y_RESET_MASK, CLK_OUT_ENB_Y);
	wmb();

	for (off = PERIPH_CLK_SOURCE_I2S1; off <= PERIPH_CLK_SOURCE_LA;
			off += 4) {
		if (off == PERIPH_CLK_SOURCE_EMC)
			continue;
		clk_writel(*ctx++, off);
	}
	for (off = PERIPH_CLK_SOURCE_MSELECT; off <= PERIPH_CLK_SOURCE_SE;
			off += 4) {
		clk_writel(*ctx++, off);
	}
	for (off = AUDIO_DLY_CLK; off <= AUDIO_SYNC_CLK_SPDIF; off+=4) {
		clk_writel(*ctx++, off);
	}
	for (off = PERIPH_CLK_SOURCE_XUSB_HOST;
		off <= PERIPH_CLK_SOURCE_VIC; off += 4)
		clk_writel(*ctx++, off);

	udelay(RESET_PROPAGATION_DELAY);

	clk_writel(*ctx++, RST_DEVICES_L);
	clk_writel(*ctx++, RST_DEVICES_H);
	clk_writel(*ctx++, RST_DEVICES_U);
	clk_writel(*ctx++, RST_DEVICES_V);
	clk_writel(*ctx++, RST_DEVICES_W);
	clk_writel(*ctx++, RST_DEVICES_X);
	clk_writel(*ctx++, RST_DEVICES_Y);
	wmb();

	clk_writel(*ctx++, CLK_OUT_ENB_L);
	clk_writel(*ctx++, CLK_OUT_ENB_H);
	clk_writel(*ctx++, CLK_OUT_ENB_U);
	clk_writel(*ctx++, CLK_OUT_ENB_V);
	clk_writel(*ctx++, CLK_OUT_ENB_W);
	clk_writel(*ctx++, CLK_OUT_ENB_X);
	clk_writel(*ctx++, CLK_OUT_ENB_Y);
	wmb();

	/* DFLL resume after cl_dvfs and i2c5 clocks are resumed */
	tegra21_dfll_clk_resume(&tegra_dfll_cpu);

	/* CPU G clock restored after DFLL and PLLs */
	clk_writel(*ctx++, tegra_clk_cclk_g.reg);
	clk_writel(*ctx++, tegra_clk_cclk_g.reg + SUPER_CLK_DIVIDER);

	clk_writel(*ctx++, SPARE_REG);
	clk_writel(*ctx++, MISC_CLK_ENB);
	clk_writel(*ctx++, CLK_MASK_ARM);

	/* Restore back the actual pll and secondary divider values */
	clk_writel(pll_p_out12, tegra_pll_p_out1.reg);
	clk_writel(pll_p_out34, tegra_pll_p_out3.reg);
	clk_writel(pll_u_out12, tegra_pll_u_out1.reg);
	val = clk_readl(tegra_pll_u_vco.reg) & (~pll_u_mask);
	clk_writel(val | (pll_u_base & pll_u_mask), tegra_pll_u_vco.reg);

	p = &tegra_pll_c;
	if (p->state == OFF)
		p->ops->disable(p);
	p = &tegra_pll_c2;
	if (p->state == OFF)
		p->ops->disable(p);
	p = &tegra_pll_c3;
	if (p->state == OFF)
		p->ops->disable(p);
	p = &tegra_pll_a1;
	if (p->state == OFF)
		p->ops->disable(p);
	p = &tegra_pll_a;
	if (p->state == OFF)
		p->ops->disable(p);
	p = &tegra_pll_d;
	if (p->state == OFF)
		p->ops->disable(p);
	p = &tegra_pll_d2;
	if (p->state == OFF)
		p->ops->disable(p);
	p = &tegra_pll_dp;
	if (p->state == OFF)
		p->ops->disable(p);
	p = &tegra_pll_x;
	if (p->state == OFF)
		p->ops->disable(p);
	p = &tegra_pll_c4_vco;
	if (p->state == OFF)
		p->ops->disable(p);
	p = &tegra_pll_re_vco;
	if (p->state == OFF)
		p->ops->disable(p);

	clk_writel(pll_a_out0, tegra_pll_a_out0.reg);
	clk_writel(pll_c_out1, tegra_pll_c_out1.reg);
	clk_writel(pll_c4_out3, tegra_pll_c4_out3.reg);
	clk_writel(pll_re_out1, tegra_pll_re_out1.reg);

	/* Since EMC clock is not restored, and may not preserve parent across
	   suspend, update current state, and mark EMC DFS as out of sync */
	p = tegra_clk_emc.parent;
	tegra21_periph_clk_init(&tegra_clk_emc);

	if (p != tegra_clk_emc.parent) {
		/* FIXME: old parent is left enabled here even if EMC was its
		   only child before suspend (may happen on Tegra11 !!) */
		pr_debug("EMC parent(refcount) across suspend: %s(%d) : %s(%d)",
			p->name, p->refcnt, tegra_clk_emc.parent->name,
			tegra_clk_emc.parent->refcnt);

		BUG_ON(!p->refcnt);
		p->refcnt--;

		/* the new parent is enabled by low level code, but ref count
		   need to be updated up to the root */
		p = tegra_clk_emc.parent;
		while (p && ((p->refcnt++) == 0))
			p = p->parent;
	}
	tegra_emc_timing_invalidate();

	tegra21_pllu_hw_ctrl_set(&tegra_pll_u_vco); /* PLLU, UTMIP h/w ctrl */
	tegra21_plle_clk_resume(&tegra_pll_e); /* Restore plle parent as pll_re_vco */
	tegra21_pllp_clk_resume(&tegra_pll_p); /* Fire a bug if not restored */
}

static struct syscore_ops tegra_clk_syscore_ops = {
	.suspend = tegra21_clk_suspend,
	.resume = tegra21_clk_resume,
	.save = tegra21_clk_suspend,
	.restore = tegra21_clk_resume,
};
#endif

static void tegra21_init_xusb_clocks(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(tegra_xusb_source_clks); i++)
		tegra21_init_one_clock(&tegra_xusb_source_clks[i]);

	tegra21_init_one_clock(&tegra_xusb_ss_div2);
	tegra21_init_one_clock(&tegra_xusb_hs_src);

	for (i = 0; i < ARRAY_SIZE(tegra_xusb_coupled_clks); i++)
		tegra21_init_one_clock(&tegra_xusb_coupled_clks[i]);
}

void __init tegra21x_init_clocks(void)
{
	int i;
	struct clk *c;

#ifndef	CONFIG_TEGRA_DUAL_CBUS
	BUILD_BUG()
#endif
	for (i = 0; i < ARRAY_SIZE(tegra_ptr_clks); i++)
		tegra21_init_one_clock(tegra_ptr_clks[i]);

	for (i = 0; i < ARRAY_SIZE(tegra_list_clks); i++)
		tegra21_init_one_clock(&tegra_list_clks[i]);

	for (i = 0; i < ARRAY_SIZE(tegra_visp_clks); i++)
		tegra21_init_one_clock(&tegra_visp_clks[i]);

	for (i = 0; i < ARRAY_SIZE(tegra_ptr_camera_mclks); i++)
		tegra21_init_one_clock(tegra_ptr_camera_mclks[i]);

	for (i = 0; i < ARRAY_SIZE(tegra_sync_source_list); i++)
		tegra21_init_one_clock(&tegra_sync_source_list[i]);
	for (i = 0; i < ARRAY_SIZE(tegra_clk_audio_list); i++)
		tegra21_init_one_clock(&tegra_clk_audio_list[i]);
	for (i = 0; i < ARRAY_SIZE(tegra_clk_audio_2x_list); i++)
		tegra21_init_one_clock(&tegra_clk_audio_2x_list[i]);
	for (i = 0; i < ARRAY_SIZE(tegra_clk_audio_dmic_list); i++)
		tegra21_init_one_clock(&tegra_clk_audio_dmic_list[i]);

	init_clk_out_mux();
	for (i = 0; i < ARRAY_SIZE(tegra_clk_out_list); i++)
		tegra21_init_one_clock(&tegra_clk_out_list[i]);

	tegra21_init_xusb_clocks();

	for (i = 0; i < ARRAY_SIZE(tegra_clk_duplicates); i++) {
		c = tegra_get_clock_by_name(tegra_clk_duplicates[i].name);
		if (!c) {
			pr_err("%s: Unknown duplicate clock %s\n", __func__,
				tegra_clk_duplicates[i].name);
			continue;
		}

		tegra_clk_duplicates[i].lookup.clk = c;
		clkdev_add(&tegra_clk_duplicates[i].lookup);
	}

	/* Initialize to default */
	tegra_init_cpu_edp_limits(0);

#ifdef CONFIG_PM_SLEEP
	register_syscore_ops(&tegra_clk_syscore_ops);
#endif
}

static int __init tegra21x_clk_late_init(void)
{
	clk_disable(&tegra_pll_re_vco);
	return 0;
}
late_initcall(tegra21x_clk_late_init);
