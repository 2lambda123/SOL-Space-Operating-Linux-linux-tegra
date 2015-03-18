/*
 * drivers/video/tegra/host/gk20a/clk_gk20a.c
 *
 * GK20A Clocks
 *
 * Copyright (c) 2011-2013, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/clk.h>
#include <linux/delay.h>	/* for mdelay */
#include <linux/module.h>
#include <linux/debugfs.h>
#include <mach/thermal.h>

#include <mach/clk.h>

#include "../dev.h"

#include "gk20a.h"
#include "hw_trim_gk20a.h"

#define nvhost_dbg_clk(fmt, arg...) \
	nvhost_dbg(dbg_clk, fmt, ##arg)

#define KHZ 1000
#define MHZ 1000000

/* from vbios PLL info table */
struct pll_parms gpc_pll_params = {
	204, 1248,	/* freq */
	1000, 2000,	/* vco */
	12, 38,		/* u */
	1, 255,		/* M */
	8, 255,		/* N */
	1, 32,		/* PL */
};

/*  dummy data for now */
static struct gpufreq_table_data
						gpu_cooling_freq[] = {
	{0, 700},
	{1, 650},
	{2, 600},
	{3, 450},
	{4,	GPUFREQ_TABLE_END},
};

struct gpufreq_table_data *tegra_gpufreq_table_get(void)
{
	return &gpu_cooling_freq[0];
}

unsigned int tegra_gpufreq_table_size_get(void)
{
	return ARRAY_SIZE(gpu_cooling_freq);
}

static u8 pl_to_div[] = {
/* PL:   0, 1, 2, 3, 4, 5, 6,  7,  8,  9, 10, 11, 12, 13, 14 */
/* p: */ 1, 2, 3, 4, 5, 6, 8, 10, 12, 16, 12, 16, 20, 24, 32 };

/* Calculate and update M/N/PL as well as pll->freq
    ref_clk_f = clk_in_f / src_div = clk_in_f; (src_div = 1 on gk20a)
    u_f = ref_clk_f / M;
    PLL output = vco_f = u_f * N = ref_clk_f * N / M;
    gpc2clk = target clock frequency = vco_f / PL;
    gpcclk = gpc2clk / 2; */
static int clk_config_pll(struct clk_gk20a *clk, struct pll *pll,
	struct pll_parms *pll_params, u32 *target_freq, bool best_fit)
{
	u32 min_vco_f, max_vco_f;
	u32 best_M, best_N;
	u32 low_PL, high_PL, best_PL;
	u32 m, n, n2;
	u32 target_vco_f, vco_f;
	u32 ref_clk_f, target_clk_f, u_f;
	u32 delta, lwv, best_delta = ~0;
	int pl;

	BUG_ON(target_freq == NULL);

	nvhost_dbg_fn("request target freq %d MHz", *target_freq);

	ref_clk_f = pll->clk_in;
	target_clk_f = *target_freq;
	max_vco_f = pll_params->max_vco;
	min_vco_f = pll_params->min_vco;
	best_M = pll_params->max_M;
	best_N = pll_params->min_N;
	best_PL = pll_params->min_PL;

	target_vco_f = target_clk_f + target_clk_f / 50;
	if (max_vco_f < target_vco_f)
		max_vco_f = target_vco_f;

	high_PL = (max_vco_f + target_vco_f - 1) / target_vco_f;
	high_PL = min(high_PL, pll_params->max_PL);
	high_PL = max(high_PL, pll_params->min_PL);

	low_PL = min_vco_f / target_vco_f;
	low_PL = min(low_PL, pll_params->max_PL);
	low_PL = max(low_PL, pll_params->min_PL);

	/* Find Indices of high_PL and low_PL */
	for (pl = 0; pl < 14; pl++) {
		if (pl_to_div[pl] >= low_PL) {
			low_PL = pl;
			break;
		}
	}
	for (pl = 0; pl < 14; pl++) {
		if (pl_to_div[pl] >= high_PL) {
			high_PL = pl;
			break;
		}
	}
	nvhost_dbg_info("low_PL %d(div%d), high_PL %d(div%d)",
			low_PL, pl_to_div[low_PL], high_PL, pl_to_div[high_PL]);

	for (pl = high_PL; pl >= (int)low_PL; pl--) {
		target_vco_f = target_clk_f * pl_to_div[pl];

		for (m = pll_params->min_M; m <= pll_params->max_M; m++) {
			u_f = ref_clk_f / m;

			if (u_f < pll_params->min_u)
				break;
			if (u_f > pll_params->max_u)
				continue;

			n = (target_vco_f * m) / ref_clk_f;
			n2 = ((target_vco_f * m) + (ref_clk_f - 1)) / ref_clk_f;

			if (n > pll_params->max_N)
				break;

			for (; n <= n2; n++) {
				if (n < pll_params->min_N)
					continue;
				if (n > pll_params->max_N)
					break;

				vco_f = ref_clk_f * n / m;

				if (vco_f >= min_vco_f && vco_f <= max_vco_f) {
					lwv = (vco_f + (pl_to_div[pl] / 2))
						/ pl_to_div[pl];
					delta = abs(lwv - target_clk_f);

					if (delta < best_delta) {
						best_delta = delta;
						best_M = m;
						best_N = n;
						best_PL = pl;

						if (best_delta == 0 ||
						    /* 0.45% for non best fit */
						    (!best_fit && (vco_f / best_delta > 218))) {
							goto found_match;
						}

						nvhost_dbg_info("delta %d @ M %d, N %d, PL %d",
							delta, m, n, pl);
					}
				}
			}
		}
	}

found_match:
	BUG_ON(best_delta == ~0);

	if (best_fit && best_delta != 0)
		nvhost_dbg_clk("no best match for target @ %dMHz on gpc_pll",
			target_clk_f);

	pll->M = best_M;
	pll->N = best_N;
	pll->PL = best_PL;

	/* save current frequency */
	pll->freq = ref_clk_f * pll->N / (pll->M * pl_to_div[pll->PL]);

	*target_freq = pll->freq;

	nvhost_dbg_clk("actual target freq %d MHz, M %d, N %d, PL %d(div%d)",
		*target_freq, pll->M, pll->N, pll->PL, pl_to_div[pll->PL]);

	nvhost_dbg_fn("done");

	return 0;
}

static int clk_program_gpc_pll(struct gk20a *g, struct clk_gk20a *clk)
{
	u32 data, cfg, coeff, timeout;

	nvhost_dbg_fn("");

	/* put PLL in bypass before programming it */
	data = gk20a_readl(g, trim_sys_sel_vco_r());
	data = set_field(data, trim_sys_sel_vco_gpc2clk_out_m(),
		trim_sys_sel_vco_gpc2clk_out_bypass_f());
	gk20a_writel(g, trim_sys_sel_vco_r(), data);

	/* get out from IDDQ */
	cfg = gk20a_readl(g, trim_sys_gpcpll_cfg_r());
	if (trim_sys_gpcpll_cfg_iddq_v(cfg)) {
		cfg = set_field(cfg, trim_sys_gpcpll_cfg_iddq_m(),
				trim_sys_gpcpll_cfg_iddq_power_on_v());
		gk20a_writel(g, trim_sys_gpcpll_cfg_r(), cfg);
		udelay(2);
	}

	/* disable PLL before changing coefficients */
	cfg = gk20a_readl(g, trim_sys_gpcpll_cfg_r());
	cfg = set_field(cfg, trim_sys_gpcpll_cfg_enable_m(),
			trim_sys_gpcpll_cfg_enable_no_f());
	gk20a_writel(g, trim_sys_gpcpll_cfg_r(), cfg);

	/* change coefficients */
	coeff = trim_sys_gpcpll_coeff_mdiv_f(clk->gpc_pll.M) |
		trim_sys_gpcpll_coeff_ndiv_f(clk->gpc_pll.N) |
		trim_sys_gpcpll_coeff_pldiv_f(clk->gpc_pll.PL);
	gk20a_writel(g, trim_sys_gpcpll_coeff_r(), coeff);

	/* enable PLL after changing coefficients */
	cfg = gk20a_readl(g, trim_sys_gpcpll_cfg_r());
	cfg = set_field(cfg, trim_sys_gpcpll_cfg_enable_m(),
			trim_sys_gpcpll_cfg_enable_yes_f());
	gk20a_writel(g, trim_sys_gpcpll_cfg_r(), cfg);

	/* lock pll */
	cfg = gk20a_readl(g, trim_sys_gpcpll_cfg_r());
	if (cfg & trim_sys_gpcpll_cfg_enb_lckdet_power_off_f()){
		cfg = set_field(cfg, trim_sys_gpcpll_cfg_enb_lckdet_m(),
			trim_sys_gpcpll_cfg_enb_lckdet_power_on_f());
		gk20a_writel(g, trim_sys_gpcpll_cfg_r(), cfg);
	}

	/* wait pll lock */
	timeout = clk->pll_delay / 100 + 1;
	do {
		cfg = gk20a_readl(g, trim_sys_gpcpll_cfg_r());
		if (cfg & trim_sys_gpcpll_cfg_pll_lock_true_f())
			goto pll_locked;
		udelay(100);
	} while (--timeout > 0);

	/* PLL is messed up. What can we do here? */
	BUG();
	return -EBUSY;

pll_locked:
	/* put PLL back on vco */
	data = gk20a_readl(g, trim_sys_sel_vco_r());
	data = set_field(data, trim_sys_sel_vco_gpc2clk_out_m(),
		trim_sys_sel_vco_gpc2clk_out_vco_f());
	gk20a_writel(g, trim_sys_sel_vco_r(), data);

	return 0;
}

static int gk20a_init_clk_reset_enable_hw(struct gk20a *g)
{
	nvhost_dbg_fn("");
	return 0;
}

static int gk20a_init_clk_setup_sw(struct gk20a *g)
{
	struct clk_gk20a *clk = &g->clk;
	static int initialized;

	nvhost_dbg_fn("");

	if (clk->sw_ready) {
		nvhost_dbg_fn("skip init");
		return 0;
	}

	/* TBD: set this according to different environments */
	clk->pll_delay = 5000000; /* usec */

	clk->gpc_pll.id = GK20A_GPC_PLL;
	clk->gpc_pll.clk_in = 12; /* MHz */

	/* Decide initial frequency */
	if (!initialized) {
		initialized = 1;
		clk->gpc_pll.M = 1;
		clk->gpc_pll.N = 60; /* 12 x 60 = 720 MHz */
		clk->gpc_pll.PL = 0;
		clk->gpc_pll.freq = clk->gpc_pll.clk_in * clk->gpc_pll.N;
	}

	clk->tegra_clk = clk_get_sys("tegra_gk20a", "PLLG_ref");
	if (IS_ERR_OR_NULL(clk->tegra_clk)) {
		nvhost_err(dev_from_gk20a(g),
			"fail to get tegra ref clk tegra_gk20a/PLLG_ref");
		return -EINVAL;
	}

	mutex_init(&clk->clk_mutex);

	clk->sw_ready = true;

	nvhost_dbg_fn("done");
	return 0;
}

static int gk20a_init_clk_setup_hw(struct gk20a *g)
{
	struct clk_gk20a *clk = &g->clk;
	u32 data;

	nvhost_dbg_fn("");

	data = gk20a_readl(g, trim_sys_gpc2clk_out_r());
	data = set_field(data,
			trim_sys_gpc2clk_out_sdiv14_m() |
			trim_sys_gpc2clk_out_vcodiv_m() |
			trim_sys_gpc2clk_out_bypdiv_m(),
			trim_sys_gpc2clk_out_sdiv14_indiv4_mode_f() |
			trim_sys_gpc2clk_out_vcodiv_by1_f() |
			trim_sys_gpc2clk_out_bypdiv_by31_f());
	gk20a_writel(g, trim_sys_gpc2clk_out_r(), data);

	return clk_program_gpc_pll(g, clk);
}

int gk20a_init_clk_support(struct gk20a *g)
{
	struct clk_gk20a *clk = &g->clk;
	u32 err;

	nvhost_dbg_fn("");

	clk->g = g;

	err = gk20a_init_clk_reset_enable_hw(g);
	if (err)
		return err;

	err = gk20a_init_clk_setup_sw(g);
	if (err)
		return err;

	err = tegra_dvfs_set_rate(clk->tegra_clk, clk->gpc_pll.freq * MHZ);
	if (err)
		return err;

	err = gk20a_init_clk_setup_hw(g);
	if (err)
		return err;

	gk20a_writel(g, 0x9080, 0x00100000);

	return err;
}

u32 gk20a_clk_get_rate(struct gk20a *g)
{
	struct clk_gk20a *clk = &g->clk;
	return clk->gpc_pll.freq;
}

/* TBD: interface to change clock and dvfs in one function */
int gk20a_clk_set_rate(struct gk20a *g, u32 rate)
{
	struct clk_gk20a *clk = &g->clk;
	struct clk *tegra_clk = clk->tegra_clk;

	/* save old freq for compare and recover */
	u32 freq = clk->gpc_pll.freq;
	int err = 0;

	nvhost_dbg_fn("curr freq: %dMHz, target freq %dMHz",
		freq, rate);
	mutex_lock(&clk->clk_mutex);
	if (rate > gpc_pll_params.max_freq)
		rate = gpc_pll_params.max_freq;
	else if (rate < gpc_pll_params.min_freq)
		rate = gpc_pll_params.min_freq;

	if (rate > clk->cap_freq)
		rate = clk->cap_freq;

	if (rate > clk->cap_freq_thermal)
		rate = clk->cap_freq_thermal;

	if (rate == freq)
		goto clean_up;

	/* gpc_pll.freq is changed to new value here */
	err = clk_config_pll(clk, &clk->gpc_pll, &gpc_pll_params,
			&rate, true);
	if (err)
		goto clean_up;

	/* change frequency only if power is on */
	/* FIXME: Need a lock to protect power gating state during
	   clk_program_gpc_pll(g, clk) */
	if (g->power_on) {
		/* raise freq, call dvfs first to raise voltage */
		if (rate > freq) {
			err = tegra_dvfs_set_rate(tegra_clk, rate * MHZ);
			if (err)
				goto clean_up;
		}

		err = clk_program_gpc_pll(g, clk);
		if (err)
			goto clean_up;

		/* lower freq, call dvfs after to lower voltage */
		if (rate < freq) {
			err = tegra_dvfs_set_rate(tegra_clk, rate * MHZ);
			if (err)
				goto clean_up;
		}
	}

clean_up:

	mutex_unlock(&clk->clk_mutex);

	/* Just report error but not restore PLL since dvfs could already changed
	    voltage even when it returns error. */
	if (err)
		nvhost_err(dev_from_gk20a(g),
			"failed to set rate to @ %d", rate);
	return err;
}

static u32 gk20a_clk_get_cap(struct gk20a *g)
{
	struct clk_gk20a *clk = &g->clk;
	return clk->cap_freq;
}

static int gk20a_clk_set_cap(struct gk20a *g, u32 rate)
{
	struct clk_gk20a *clk = &g->clk;

	if (rate > gpc_pll_params.max_freq)
		rate = gpc_pll_params.max_freq;
	else if (rate < gpc_pll_params.min_freq)
		rate = gpc_pll_params.min_freq;

	clk->cap_freq = rate;
	if (gk20a_clk_get_rate(g) <= rate)
		return 0;
	return gk20a_clk_set_rate(g, rate);
}

static u32 gk20a_clk_get_cap_thermal(struct gk20a *g)
{
	struct clk_gk20a *clk = &g->clk;
	return clk->cap_freq_thermal;
}

static int gk20a_clk_set_cap_thermal(struct gk20a *g, u32 rate)
{
	struct clk_gk20a *clk = &g->clk;

	if (rate > gpc_pll_params.max_freq)
		rate = gpc_pll_params.max_freq;
	else if (rate < gpc_pll_params.min_freq)
		rate = gpc_pll_params.min_freq;

	clk->cap_freq_thermal = rate;
	if (gk20a_clk_get_rate(g) <= rate)
		return 0;
	return gk20a_clk_set_rate(g, rate);
}

static u32 gk20a_clk_get_max(void)
{
	return gpc_pll_params.max_freq;
}

static struct gk20a_clk_cap_info gk20a_clk_cap = {
	.set_cap_thermal = gk20a_clk_set_cap_thermal,
	.get_max = gk20a_clk_get_max,
};

int gk20a_clk_init_cap_freqs(struct gk20a *g)
{
	struct clk_gk20a *clk = &g->clk;

	/* init cap_freq == max_freq */
	clk->cap_freq = gpc_pll_params.max_freq;
	clk->cap_freq_thermal = gpc_pll_params.max_freq;

	gk20a_clk_cap.g = g;

	tegra_throttle_gk20a_clk_cap_register(&gk20a_clk_cap);
}

#ifdef CONFIG_DEBUG_FS

static int init_set(void *data, u64 val)
{
	struct gk20a *g = (struct gk20a *)data;
	return gk20a_init_clk_support(g);
}
DEFINE_SIMPLE_ATTRIBUTE(init_fops, NULL, init_set, "%llu\n");

static int rate_get(void *data, u64 *val)
{
	struct gk20a *g = (struct gk20a *)data;
	*val = (u64)gk20a_clk_get_rate(g);
	return 0;
}
static int rate_set(void *data, u64 val)
{
	struct gk20a *g = (struct gk20a *)data;
	return gk20a_clk_set_rate(g, (u32)val);
}
DEFINE_SIMPLE_ATTRIBUTE(rate_fops, rate_get, rate_set, "%llu\n");

static int cap_get(void *data, u64 *val)
{
	struct gk20a *g = (struct gk20a *)data;
	*val = (u64)gk20a_clk_get_cap(g);
	return 0;
}
static int cap_set(void *data, u64 val)
{
	struct gk20a *g = (struct gk20a *)data;
	return gk20a_clk_set_cap(g, (u32)val);
}
DEFINE_SIMPLE_ATTRIBUTE(cap_fops, cap_get, cap_set, "%llu\n");

static int cap_thermal_get(void *data, u64 *val)
{
	struct gk20a *g = (struct gk20a *)data;
	*val = (u64)gk20a_clk_get_cap_thermal(g);
	return 0;
}
static int cap_thermal_set(void *data, u64 val)
{
	struct gk20a *g = (struct gk20a *)data;
	return gk20a_clk_set_cap_thermal(g, (u32)val);
}
DEFINE_SIMPLE_ATTRIBUTE(cap_thermal_fops, cap_thermal_get,
		cap_thermal_set, "%llu\n");

static int monitor_get(void *data, u64 *val)
{
	struct gk20a *g = (struct gk20a *)data;
	struct clk_gk20a *clk = &g->clk;

	u32 NV_PTRIM_GPC_CLK_CNTR_NCGPCCLK_CFG = 0x00134124;
	u32 NV_PTRIM_GPC_CLK_CNTR_NCGPCCLK_CNT = 0x00134128;
	u32 ncycle = 100; /* count GPCCLK for ncycle of clkin */
	u32 clkin = clk->gpc_pll.clk_in;
	u32 count1, count2;

	gk20a_writel(g, NV_PTRIM_GPC_CLK_CNTR_NCGPCCLK_CFG,
		(1<<24)); /* reset */
	gk20a_writel(g, NV_PTRIM_GPC_CLK_CNTR_NCGPCCLK_CFG,
		(1<<20) | (1<<16) | ncycle); /* start */

	/* It should take about 8us to finish 100 cycle of 12MHz.
	   But longer than 100us delay is required here. */
	udelay(2000);

	count1 = gk20a_readl(g, NV_PTRIM_GPC_CLK_CNTR_NCGPCCLK_CNT);
	udelay(100);
	count2 = gk20a_readl(g, NV_PTRIM_GPC_CLK_CNTR_NCGPCCLK_CNT);
	*val = (u64)(count2 * clkin / ncycle);

	if (count1 != count2)
		return -EBUSY;
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(monitor_fops, monitor_get, NULL, "%llu\n");

int clk_gk20a_debugfs_init(struct platform_device *dev)
{
	struct dentry *d;
	struct nvhost_device_data *pdata = platform_get_drvdata(dev);
	struct gk20a *g = get_gk20a(dev);

	d = debugfs_create_file(
		"init", S_IRUGO|S_IWUSR, pdata->debugfs, g, &init_fops);
	if (!d)
		goto err_out;

	d = debugfs_create_file(
		"rate", S_IRUGO|S_IWUSR, pdata->debugfs, g, &rate_fops);
	if (!d)
		goto err_out;

	d = debugfs_create_file(
		"cap", S_IRUGO|S_IWUSR, pdata->debugfs, g, &cap_fops);
	if (!d)
		goto err_out;

	d = debugfs_create_file(
		"cap_thermal", S_IRUGO|S_IWUSR, pdata->debugfs, g,
							&cap_thermal_fops);
	if (!d)
		goto err_out;

	d = debugfs_create_file(
		"monitor", S_IRUGO, pdata->debugfs, g, &monitor_fops);
	if (!d)
		goto err_out;

	return 0;

err_out:
	pr_err("%s: Failed to make debugfs node\n", __func__);
	debugfs_remove_recursive(pdata->debugfs);
	return -ENOMEM;
}

int gk20a_clk_disable_gpcpll(struct gk20a *g)
{
	u32 cfg;

	cfg = gk20a_readl(g, trim_sys_gpcpll_cfg_r());
	cfg = set_field(cfg, trim_sys_gpcpll_cfg_enable_m(),
			trim_sys_gpcpll_cfg_enable_no_f());
	gk20a_writel(g, trim_sys_gpcpll_cfg_r(), cfg);

	return 0;
}
#endif /* CONFIG_DEBUG_FS */
