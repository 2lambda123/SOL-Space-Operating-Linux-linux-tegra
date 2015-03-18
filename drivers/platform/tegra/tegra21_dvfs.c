/*
 * drivers/platform/tegra/tegra21_dvfs.c
 *
 * Copyright (c) 2012-2014 NVIDIA CORPORATION. All rights reserved.
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

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/module.h>
#include <linux/clk.h>
#include <linux/kobject.h>
#include <linux/err.h>
#include <linux/tegra-fuse.h>
#include <linux/pm_qos.h>

#include "clock.h"
#include "dvfs.h"
#include "board.h"
#include "tegra_cl_dvfs.h"
#include "tegra_core_sysfs_limits.h"

static bool tegra_dvfs_cpu_disabled;
static bool tegra_dvfs_core_disabled;
static bool tegra_dvfs_gpu_disabled;

#define KHZ 1000
#define MHZ 1000000

/* FIXME: need tegra21 step */
#define VDD_SAFE_STEP			100

/* FIXME: tegra21 data */
static int vdd_core_vmin_trips_table[MAX_THERMAL_LIMITS];
static int vdd_core_therm_floors_table[MAX_THERMAL_LIMITS];

static int vdd_core_vmax_trips_table[MAX_THERMAL_LIMITS];
static int vdd_core_therm_caps_table[MAX_THERMAL_LIMITS];

static struct tegra_cooling_device core_vmax_cdev = {
	.cdev_type = "core_hot",
};

static struct tegra_cooling_device core_vmin_cdev = {
	.cdev_type = "core_cold",
};

static struct tegra_cooling_device gpu_vmin_cdev = {
	.cdev_type = "gpu_cold",
};

static struct tegra_cooling_device gpu_vts_cdev = {
	.cdev_type = "gpu_scaling",
};

/* FIXME: fill in actual hw numbers for all rails */
static struct dvfs_rail tegra21_dvfs_rail_vdd_cpu = {
	.reg_id = "vdd_cpu",
	.max_millivolts = 1300,
	.min_millivolts = 800,
	.step = VDD_SAFE_STEP,
	.jmp_to_zero = true,
	.alignment = {
		.step_uv = 10000, /* 10mV */
	},
	.stats = {
		.bin_uV = 10000, /* 10mV */
	},
	.version = "p4v00",
};

static struct dvfs_rail tegra21_dvfs_rail_vdd_core = {
	.reg_id = "vdd_core",
	.max_millivolts = 1300,
	.min_millivolts = 800,
	.step = VDD_SAFE_STEP,
	.step_up = 1300,
	.vmin_cdev = &core_vmin_cdev,
	.vmax_cdev = &core_vmax_cdev,
	.version = "p4v00",
};

static struct dvfs_rail tegra21_dvfs_rail_vdd_gpu = {
	.reg_id = "vdd_gpu",
	.max_millivolts = 1300,
	.min_millivolts = 800,
	.step = VDD_SAFE_STEP,
	.step_up = 1300,
	.in_band_pm = true,
	.vts_cdev = &gpu_vts_cdev,
	.vmin_cdev = &gpu_vmin_cdev,
	.alignment = {
		.step_uv = 10000, /* 10mV */
	},
	.stats = {
		.bin_uV = 10000, /* 10mV */
	},
	.version = "p4v00",
};

static struct dvfs_rail *tegra21_dvfs_rails[] = {
	&tegra21_dvfs_rail_vdd_cpu,
	&tegra21_dvfs_rail_vdd_core,
	&tegra21_dvfs_rail_vdd_gpu,
};

void __init tegra21x_vdd_cpu_align(int step_uv, int offset_uv)
{
	tegra21_dvfs_rail_vdd_cpu.alignment.step_uv = step_uv;
	tegra21_dvfs_rail_vdd_cpu.alignment.offset_uv = offset_uv;
}

/* CPU DVFS tables */
/* FIXME: real data */
static unsigned long cpu_max_freq[] = {
/* speedo_id	0	 */
		2015000,
};

static struct cpu_cvb_dvfs cpu_cvb_dvfs_table[] = {
	{
		.speedo_id = -1,
		.process_id = -1,
		.dfll_tune_data  = {
			.tune0		= 0x00f0409f,
			.tune0_high_mv	= 0x00f0409f,
			.tune1		= 0x000000a0,
			.droop_rate_min = 1000000,
			.min_millivolts = 800,
		},
		.max_mv = 1260,
		.freqs_mult = KHZ,
		.speedo_scale = 100,
		.voltage_scale = 1000,
		.cvb_table = {
			/*f       dfll: c0,     c1,   c2  pll:  c0,   c1,    c2 */
			{408000 , {2244479,  -117955,  2292}, { 960000,  0,  0} },
			{510000 , {2279779,  -119575,  2292}, { 960000,  0,  0} },
			{612000 , {2317504,  -121185,  2292}, { 960000,  0,  0} },
			{714000 , {2357653,  -122795,  2292}, { 972000,  0,  0} },
			{816000 , {2400228,  -124415,  2292}, { 984000,  0,  0} },
			{918000 , {2445228,  -126025,  2292}, { 996000,  0,  0} },
			{1020000, {2492653,  -127635,  2292}, { 1020000, 0,  0} },
			{1122000, {2542502,  -129255,  2292}, { 1032000, 0,  0} },
			{1224000, {2594777,  -130865,  2292}, { 1056000, 0,  0} },
			{1326000, {2649477,  -132475,  2292}, { 1092000, 0,  0} },
			{1428000, {2706601,  -134095,  2292}, { 1116000, 0,  0} },
			{1530000, {2766150,  -135705,  2292}, { 1152000, 0,  0} },
			{1632000, {2828125,  -137315,  2292}, { 1188000, 0,  0} },
			{1734000, {2892524,  -138935,  2292}, { 1224000, 0,  0} },
			{1836000, {2959348,  -140545,  2292}, { 1260000, 0,  0} },
			{1913000, {3028598,  -142155,  2292}, { 1308000, 0,  0} },
			{2015000, {3100272,  -143775,  2292}, { 1356000, 0,  0} },
			{2117000, {3174371,  -145385,  2292}, { 1404000, 0,  0} },
			{      0, {      0,        0,     0}, {       0, 0,  0} },
		},
	},
};

static int cpu_millivolts[MAX_DVFS_FREQS];
static int cpu_dfll_millivolts[MAX_DVFS_FREQS];

static struct dvfs cpu_dvfs = {
	.clk_name	= "cpu_g",
	.millivolts	= cpu_millivolts,
	.dfll_millivolts = cpu_dfll_millivolts,
	.auto_dvfs	= true,
	.dvfs_rail	= &tegra21_dvfs_rail_vdd_cpu,
};

/* GPU DVFS tables */
/* FIXME: fill in actual hw numbers */
static unsigned long gpu_max_freq[] = {
/* speedo_id	0	*/
		652800,
};
static struct gpu_cvb_dvfs gpu_cvb_dvfs_table[] = {
	{
		.speedo_id =  -1,
		.process_id = -1,
		.max_mv = 1200,
		.freqs_mult = KHZ,
		.speedo_scale = 100,
		.thermal_scale = 10,
		.voltage_scale = 1000,
		.cvb_table = {
			/*f        dfll  pll:   c0,     c1,   c2 */
			{  204000, {  }, {  810000,      0,   0}, },
			{  264000, {  }, {  860000,      0,   0}, },
			{  351000, {  }, {  900000,      0,   0}, },
			{  492000, {  }, {  990000,      0,   0}, },
			{  652800, {  }, { 1080000,      0,   0}, },
			{       0, {  }, {       0,      0,   0}, },
		},
		.vts_trips_table = { 0, 70, },
	},
};

static int gpu_vmin[MAX_THERMAL_RANGES];
static int gpu_peak_millivolts[MAX_DVFS_FREQS];
static int gpu_millivolts[MAX_THERMAL_RANGES][MAX_DVFS_FREQS];
static struct dvfs gpu_dvfs = {
	.clk_name	= "gbus",
	.auto_dvfs	= true,
	.dvfs_rail	= &tegra21_dvfs_rail_vdd_gpu,
};

/* Core DVFS tables */
/* FIXME: real data */
static const int core_millivolts[MAX_DVFS_FREQS] = {
	810, 860, 900, 1000, 1100};

#define CORE_DVFS(_clk_name, _speedo_id, _process_id, _auto, _mult, _freqs...) \
	{							\
		.clk_name	= _clk_name,			\
		.speedo_id	= _speedo_id,			\
		.process_id	= _process_id,			\
		.freqs		= {_freqs},			\
		.freqs_mult	= _mult,			\
		.millivolts	= core_millivolts,		\
		.auto_dvfs	= _auto,			\
		.dvfs_rail	= &tegra21_dvfs_rail_vdd_core,	\
	}

#define OVRRD_DVFS(_clk_name, _speedo_id, _process_id, _auto, _mult, _freqs...) \
	{							\
		.clk_name	= _clk_name,			\
		.speedo_id	= _speedo_id,			\
		.process_id	= _process_id,			\
		.freqs		= {_freqs},			\
		.freqs_mult	= _mult,			\
		.millivolts	= core_millivolts,		\
		.auto_dvfs	= _auto,			\
		.can_override	= true,				\
		.dvfs_rail	= &tegra21_dvfs_rail_vdd_core,	\
	}

static struct dvfs core_dvfs_table[] = {
	/* Core voltages (mV):		         810,    860,    900,    1000,    1100*/
	/* Clock limits for internal blocks, PLLs */
	CORE_DVFS("emc",    -1, -1, 1, KHZ,   264000, 348000, 384000, 528000,  1599000),

	CORE_DVFS("cpu_lp", -1, -1, 1, KHZ,   144000, 252000, 288000, 444000,  624000),

	/*
	 * Disable framework auto-dvfs on sbus - let platform code to call
	 * tegra_dvfs_set_rate manually. It is necessary for special handling
	 * of system clock skipper enabled on T210
	 */
	CORE_DVFS("sbus",   -1, -1, 0, KHZ,    81600, 102000, 136000, 204000,  204000),

	CORE_DVFS("vic03",  -1, -1, 1, KHZ,   120000, 144000, 168000, 216000,  372000),
	CORE_DVFS("c2bus",  -1, -1, 1, KHZ,   120000, 144000, 168000, 216000,  372000),

	CORE_DVFS("msenc",  -1, -1, 1, KHZ,    72000,  84000, 102000, 180000,  252000),
	CORE_DVFS("nvdec",  -1, -1, 1, KHZ,    72000,  84000, 102000, 180000,  252000),
	CORE_DVFS("nvjpg",  -1, -1, 1, KHZ,    72000,  84000, 102000, 180000,  252000),
	CORE_DVFS("se",     -1, -1, 1, KHZ,    72000,  84000, 102000, 180000,  252000),
	CORE_DVFS("tsec",   -1, -1, 1, KHZ,    72000,  84000, 102000, 180000,  252000),
	CORE_DVFS("tsecb",  -1, -1, 1, KHZ,    72000,  84000, 102000, 180000,  252000),
	CORE_DVFS("c3bus",  -1, -1, 1, KHZ,    72000,  84000, 102000, 180000,  252000),

	CORE_DVFS("host1x", -1, -1, 1, KHZ,    81600, 102000, 136000, 163000,  204000),

	CORE_DVFS("vi",     -1, -1, 1, KHZ,   120000, 156000, 182000, 312000,  700000),
	CORE_DVFS("isp",    -1, -1, 1, KHZ,   120000, 156000, 182000, 312000,  700000),
	CORE_DVFS("cbus",   -1, -1, 1, KHZ,   120000, 156000, 182000, 312000,  700000),

	CORE_DVFS("pll_m",  -1, -1, 1, KHZ,   800000, 800000, 1066000, 1066000, 1599000),
	CORE_DVFS("pll_mb", -1, -1, 1, KHZ,   800000, 800000, 1066000, 1066000, 1599000),
	CORE_DVFS("pll_c",  -1, -1, 1, KHZ,   800000, 800000, 1066000, 1066000, 1066000),
	CORE_DVFS("pll_c2", -1, -1, 1, KHZ,   800000, 800000, 1066000, 1066000, 1066000),
	CORE_DVFS("pll_c3", -1, -1, 1, KHZ,   800000, 800000, 1066000, 1066000, 1066000),

	/* Core voltages (mV):		         810,    860,    900,    990,    1080*/
	/* Clock limits for I/O peripherals */
	CORE_DVFS("sbc1",   -1, -1, 1, KHZ,    24000,  24000,  48000,  48000,   48000),
	CORE_DVFS("sbc2",   -1, -1, 1, KHZ,    24000,  24000,  48000,  48000,   48000),
	CORE_DVFS("sbc3",   -1, -1, 1, KHZ,    24000,  24000,  48000,  48000,   48000),
	CORE_DVFS("sbc4",   -1, -1, 1, KHZ,    24000,  24000,  48000,  48000,   48000),
	CORE_DVFS("sbc5",   -1, -1, 1, KHZ,    24000,  24000,  48000,  48000,   48000),
	CORE_DVFS("sbc6",   -1, -1, 1, KHZ,    24000,  24000,  48000,  48000,   48000),

	OVRRD_DVFS("sdmmc1", -1, -1, 1, KHZ,   102000, 102000, 163200, 163200,  163200),
	OVRRD_DVFS("sdmmc3", -1, -1, 1, KHZ,   102000, 102000, 163200, 163200,  163200),
	OVRRD_DVFS("sdmmc4", -1, -1, 1, KHZ,   102000, 102000, 178200, 178200,  178200),

	CORE_DVFS("hdmi",   -1, -1, 1, KHZ,    99000, 118800, 148500, 198000,  198000),
	CORE_DVFS("pciex",  -1, -1, 1, KHZ,   250000, 250000, 250000, 500000,  500000),

	/*
	 * The clock rate for the display controllers that determines the
	 * necessary core voltage depends on a divider that is internal
	 * to the display block.  Disable auto-dvfs on the display clocks,
	 * and let the display driver call tegra_dvfs_set_rate manually
	 */
	CORE_DVFS("disp1",  -1, -1, 0, KHZ,   108000, 120000, 144000, 192000,  240000),
	CORE_DVFS("disp2",  -1, -1, 0, KHZ,   108000, 120000, 144000, 192000,  240000),

	/* xusb clocks */
	CORE_DVFS("xusb_falcon_src", -1, -1, 1, KHZ,  204000, 204000, 204000, 336000, 336000),
	CORE_DVFS("xusb_host_src",   -1, -1, 1, KHZ,   58300,  58300,  58300, 112000, 112000),
	CORE_DVFS("xusb_dev_src",    -1, -1, 1, KHZ,   58300,  58300,  58300, 112000, 112000),
	CORE_DVFS("xusb_ss_src",     -1, -1, 1, KHZ,   60000,  60000,  60000, 120000, 120000),
	CORE_DVFS("xusb_fs_src",     -1, -1, 1, KHZ,       0,  48000,  48000,  48000,  48000),
	CORE_DVFS("xusb_hs_src",     -1, -1, 1, KHZ,       0,  60000,  60000,  60000,  60000),
};

int tegra_dvfs_disable_core_set(const char *arg, const struct kernel_param *kp)
{
	int ret;

	ret = param_set_bool(arg, kp);
	if (ret)
		return ret;

	if (tegra_dvfs_core_disabled)
		tegra_dvfs_rail_disable(&tegra21_dvfs_rail_vdd_core);
	else
		tegra_dvfs_rail_enable(&tegra21_dvfs_rail_vdd_core);

	return 0;
}

int tegra_dvfs_disable_cpu_set(const char *arg, const struct kernel_param *kp)
{
	int ret;

	ret = param_set_bool(arg, kp);
	if (ret)
		return ret;

	if (tegra_dvfs_cpu_disabled)
		tegra_dvfs_rail_disable(&tegra21_dvfs_rail_vdd_cpu);
	else
		tegra_dvfs_rail_enable(&tegra21_dvfs_rail_vdd_cpu);

	return 0;
}

int tegra_dvfs_disable_gpu_set(const char *arg, const struct kernel_param *kp)
{
	int ret;

	ret = param_set_bool(arg, kp);
	if (ret)
		return ret;

	if (tegra_dvfs_gpu_disabled)
		tegra_dvfs_rail_disable(&tegra21_dvfs_rail_vdd_gpu);
	else
		tegra_dvfs_rail_enable(&tegra21_dvfs_rail_vdd_gpu);

	return 0;
}

int tegra_dvfs_disable_get(char *buffer, const struct kernel_param *kp)
{
	return param_get_bool(buffer, kp);
}

static struct kernel_param_ops tegra_dvfs_disable_core_ops = {
	.set = tegra_dvfs_disable_core_set,
	.get = tegra_dvfs_disable_get,
};

static struct kernel_param_ops tegra_dvfs_disable_cpu_ops = {
	.set = tegra_dvfs_disable_cpu_set,
	.get = tegra_dvfs_disable_get,
};

static struct kernel_param_ops tegra_dvfs_disable_gpu_ops = {
	.set = tegra_dvfs_disable_gpu_set,
	.get = tegra_dvfs_disable_get,
};

module_param_cb(disable_core, &tegra_dvfs_disable_core_ops,
	&tegra_dvfs_core_disabled, 0644);
module_param_cb(disable_cpu, &tegra_dvfs_disable_cpu_ops,
	&tegra_dvfs_cpu_disabled, 0644);
module_param_cb(disable_gpu, &tegra_dvfs_disable_gpu_ops,
	&tegra_dvfs_gpu_disabled, 0644);

static bool __init can_update_max_rate(struct clk *c, struct dvfs *d)
{
	/* Don't update manual dvfs, non-shared clocks */
	if (!d->auto_dvfs && !c->ops->shared_bus_update)
		return false;

	/*
	 * Don't update EMC shared bus, since EMC dvfs is board dependent: max
	 * rate and EMC scaling frequencies are determined by tegra BCT (flashed
	 * together with the image) and board specific EMC DFS table; we will
	 * check the scaling ladder against nominal core voltage when the table
	 * is loaded (and if on particular board the table is not loaded, EMC
	 * scaling is disabled).
	 */
	if (c->ops->shared_bus_update && (c->flags & PERIPH_EMC_ENB))
		return false;

	/*
	 * Don't update shared cbus, and don't propagate common cbus dvfs
	 * limit down to shared users, but set maximum rate for each user
	 * equal to the respective client limit.
	 */
	if (c->ops->shared_bus_update && (c->flags & PERIPH_ON_CBUS)) {
		struct clk *user;
		unsigned long rate;

		list_for_each_entry(
			user, &c->shared_bus_list, u.shared_bus_user.node) {
			if (user->u.shared_bus_user.client) {
				rate = user->u.shared_bus_user.client->max_rate;
				user->max_rate = rate;
				user->u.shared_bus_user.rate = rate;
			}
		}
		return false;
	}

	/* Other, than EMC and cbus, auto-dvfs clocks can be updated */
	return true;
}

static void __init init_dvfs_one(struct dvfs *d, int max_freq_index)
{
	int ret;
	struct clk *c = tegra_get_clock_by_name(d->clk_name);

	if (!c) {
		pr_debug("tegra21_dvfs: no clock found for %s\n",
			d->clk_name);
		return;
	}

	/* Update max rate for auto-dvfs clocks, with shared bus exceptions */
	if (can_update_max_rate(c, d)) {
		BUG_ON(!d->freqs[max_freq_index]);
		tegra_init_max_rate(
			c, d->freqs[max_freq_index] * d->freqs_mult);
	}
	d->max_millivolts = d->dvfs_rail->nominal_millivolts;

	ret = tegra_enable_dvfs_on_clk(c, d);
	if (ret)
		pr_err("tegra21_dvfs: failed to enable dvfs on %s\n", c->name);
}

static bool __init match_dvfs_one(const char *name,
	int dvfs_speedo_id, int dvfs_process_id,
	int speedo_id, int process_id)
{
	if ((dvfs_process_id != -1 && dvfs_process_id != process_id) ||
		(dvfs_speedo_id != -1 && dvfs_speedo_id != speedo_id)) {
		pr_debug("tegra21_dvfs: rejected %s speedo %d, process %d\n",
			 name, dvfs_speedo_id, dvfs_process_id);
		return false;
	}
	return true;
}

/* cvb_mv = ((c2 * speedo / s_scale + c1) * speedo / s_scale + c0) / v_scale */
static inline int get_cvb_voltage(int speedo, int s_scale,
				  struct cvb_dvfs_parameters *cvb)
{
	/* apply only speedo scale: output mv = cvb_mv * v_scale */
	int mv;
	mv = DIV_ROUND_CLOSEST(cvb->c2 * speedo, s_scale);
	mv = DIV_ROUND_CLOSEST((mv + cvb->c1) * speedo, s_scale) + cvb->c0;
	return mv;
}

/* cvb_t_mv =
   ((c3 * speedo / s_scale + c4 + c5 * T / t_scale) * T / t_scale) / v_scale */
static inline int get_cvb_t_voltage(int speedo, int s_scale, int t, int t_scale,
				    struct cvb_dvfs_parameters *cvb)
{
	/* apply speedo & temperature scales: output mv = cvb_t_mv * v_scale */
	int mv;
	mv = DIV_ROUND_CLOSEST(cvb->c3 * speedo, s_scale) + cvb->c4 +
		DIV_ROUND_CLOSEST(cvb->c5 * t, t_scale);
	mv = DIV_ROUND_CLOSEST(mv * t, t_scale);
	return mv;
}

static int round_cvb_voltage(int mv, int v_scale, struct rail_alignment *align)
{
	/* combined: apply voltage scale and round to cvb alignment step */
	int uv;
	int step = (align->step_uv ? : 1000) * v_scale;
	int offset = align->offset_uv * v_scale;

	uv = max(mv * 1000, offset) - offset;
	uv = DIV_ROUND_UP(uv, step) * align->step_uv + align->offset_uv;
	return uv / 1000;
}

static int round_voltage(int mv, struct rail_alignment *align, bool up)
{
	if (align->step_uv) {
		int uv = max(mv * 1000, align->offset_uv) - align->offset_uv;
		uv = (uv + (up ? align->step_uv - 1 : 0)) / align->step_uv;
		return (uv * align->step_uv + align->offset_uv) / 1000;
	}
	return mv;
}

/*
 * Setup cpu dvfs and dfll tables from cvb data, determine nominal voltage
 * for cpu rail, and cpu maximum frequency. Note that entire frequency range
 * is guaranteed only when dfll is used as cpu clock source. Reaching maximum
 * frequency with pll as cpu clock source * may not be possible within nominal
 * voltage range (dvfs mechanism * would automatically fail frequency request
 * in this case, so that voltage limit is not violated). Error when cpu dvfs
 * table can not be constructed must never happen.
 */
static int __init set_cpu_dvfs_data(unsigned long max_freq,
	struct cpu_cvb_dvfs *d, struct dvfs *cpu_dvfs, int *max_freq_index)
{
	int j, mv, dfll_mv, min_dfll_mv;
	unsigned long fmax_at_vmin = 0;
	unsigned long fmax_pll_mode = 0;
	unsigned long fmin_use_dfll = 0;
	struct cvb_dvfs_table *table = NULL;
	int speedo = tegra_cpu_speedo_value();
	struct dvfs_rail *rail = &tegra21_dvfs_rail_vdd_cpu;
	struct rail_alignment *align = &rail->alignment;

	min_dfll_mv = d->dfll_tune_data.min_millivolts;
	min_dfll_mv =  round_voltage(min_dfll_mv, align, true);
	d->max_mv = round_voltage(d->max_mv, align, false);
	BUG_ON(min_dfll_mv < rail->min_millivolts);

	/*
	 * Use CVB table to fill in CPU dvfs frequencies and voltages. Each
	 * CVB entry specifies CPU frequency and CVB coefficients to calculate
	 * the respective voltage when either DFLL or PLL is used as CPU clock
	 * source.
	 *
	 * Minimum voltage limit is applied only to DFLL source. For PLL source
	 * voltage can go as low as table specifies. Maximum voltage limit is
	 * applied to both sources, but differently: directly clip voltage for
	 * DFLL, and limit maximum frequency for PLL.
	 */
	for (j = 0; j < MAX_DVFS_FREQS; j++) {
		table = &d->cvb_table[j];
		if (!table->freq || (table->freq > max_freq))
			break;

		dfll_mv = get_cvb_voltage(
			speedo, d->speedo_scale, &table->cvb_dfll_param);
		dfll_mv = round_cvb_voltage(dfll_mv, d->voltage_scale, align);

		mv = get_cvb_voltage(
			speedo, d->speedo_scale, &table->cvb_pll_param);
		mv = round_cvb_voltage(mv, d->voltage_scale, align);

		/*
		 * Check maximum frequency at minimum voltage for dfll source;
		 * round down unless all table entries are above Vmin, then use
		 * the 1st entry as is.
		 */
		dfll_mv = max(dfll_mv, min_dfll_mv);
		if (dfll_mv > min_dfll_mv) {
			if (!j)
				fmax_at_vmin = table->freq;
			if (!fmax_at_vmin)
				fmax_at_vmin = cpu_dvfs->freqs[j - 1];
		}

		/* Clip maximum frequency at maximum voltage for pll source */
		if (mv > d->max_mv) {
			if (!j)
				break;	/* 1st entry already above Vmax */
			if (!fmax_pll_mode)
				fmax_pll_mode = cpu_dvfs->freqs[j - 1];
		}

		/* Minimum rate with pll source voltage above dfll Vmin */
		if ((mv >= min_dfll_mv) && (!fmin_use_dfll))
			fmin_use_dfll = table->freq;

		/* fill in dvfs tables */
		cpu_dvfs->freqs[j] = table->freq;
		cpu_dfll_millivolts[j] = min(dfll_mv, d->max_mv);
		cpu_millivolts[j] = mv;
	}

	/* Table must not be empty, must have at least one entry above Vmin */
	if (!j || !fmax_at_vmin) {
		pr_err("tegra21_dvfs: invalid cpu dvfs table\n");
		return -ENOENT;
	}

	/* In the dfll operating range dfll voltage at any rate should be
	   better (below) than pll voltage */
	if (!fmin_use_dfll || (fmin_use_dfll > fmax_at_vmin)) {
		WARN(1, "tegra21_dvfs: pll voltage is below dfll in the dfll"
			" operating range\n");
		fmin_use_dfll = fmax_at_vmin;
	}

	/* dvfs tables are successfully populated - fill in the rest */
	cpu_dvfs->speedo_id = d->speedo_id;
	cpu_dvfs->process_id = d->process_id;
	cpu_dvfs->freqs_mult = d->freqs_mult;
	cpu_dvfs->dvfs_rail->nominal_millivolts = min(d->max_mv,
		max(cpu_millivolts[j - 1], cpu_dfll_millivolts[j - 1]));
	*max_freq_index = j - 1;

	cpu_dvfs->dfll_data = d->dfll_tune_data;
	cpu_dvfs->dfll_data.max_rate_boost = fmax_pll_mode ?
		(cpu_dvfs->freqs[j - 1] - fmax_pll_mode) * d->freqs_mult : 0;
	cpu_dvfs->dfll_data.out_rate_min = fmax_at_vmin * d->freqs_mult;
	cpu_dvfs->dfll_data.use_dfll_rate_min = fmin_use_dfll * d->freqs_mult;
	cpu_dvfs->dfll_data.min_millivolts = min_dfll_mv;

	return 0;
}

static void __init init_cpu_dvfs_table(int *cpu_max_freq_index)
{
	int i, ret;
	int cpu_speedo_id = tegra_cpu_speedo_id();
	int cpu_process_id = tegra_cpu_process_id();

	BUG_ON(cpu_speedo_id >= ARRAY_SIZE(cpu_max_freq));
	for (ret = 0, i = 0; i <  ARRAY_SIZE(cpu_cvb_dvfs_table); i++) {
		struct cpu_cvb_dvfs *d = &cpu_cvb_dvfs_table[i];
		unsigned long max_freq = cpu_max_freq[cpu_speedo_id];
		if (match_dvfs_one("cpu cvb", d->speedo_id, d->process_id,
				   cpu_speedo_id, cpu_process_id)) {
			ret = set_cpu_dvfs_data(max_freq,
				d, &cpu_dvfs, cpu_max_freq_index);
			break;
		}
	}
	BUG_ON((i == ARRAY_SIZE(cpu_cvb_dvfs_table)) || ret);
}

/*
 * Setup gpu dvfs tables from cvb data, determine nominal voltage for gpu rail,
 * and gpu maximum frequency. Error when gpu dvfs table can not be constructed
 * must never happen.
 */
static int __init set_gpu_dvfs_data(unsigned long max_freq,
	struct gpu_cvb_dvfs *d, struct dvfs *gpu_dvfs, int *max_freq_index)
{
	int i, j, thermal_ranges, mv;
	struct cvb_dvfs_table *table = NULL;
	int speedo = tegra_gpu_speedo_value();
	struct dvfs_rail *rail = &tegra21_dvfs_rail_vdd_gpu;
	struct rail_alignment *align = &rail->alignment;

	d->max_mv = round_voltage(d->max_mv, align, false);

	/*
	 * Init thermal trips, find number of thermal ranges; note that the
	 * first trip-point is used for voltage calculations within the lowest
	 * range, but should not be actually set. Hence, at least 2 trip-points
	 * must be specified.
	 */
	if (tegra_dvfs_rail_init_thermal_dvfs_trips(d->vts_trips_table, rail))
		return -ENOENT;
	thermal_ranges = rail->vts_cdev->trip_temperatures_num;
	rail->vts_cdev->trip_temperatures_num--;

	if (thermal_ranges < 2)
		WARN(1, "tegra21_dvfs: %d gpu trip: thermal dvfs is broken\n",
		     thermal_ranges);

	/*
	 * Use CVB table to calculate Vmin for each temperature range
	 */
	mv = get_cvb_voltage(
		speedo, d->speedo_scale, &d->cvb_vmin.cvb_pll_param);
	for (j = 0; j < thermal_ranges; j++) {
		int mvj = mv;
		int t = rail->vts_cdev->trip_temperatures[j];

		/* add Vmin thermal offset for this trip-point */
		mvj += get_cvb_t_voltage(speedo, d->speedo_scale,
			t, d->thermal_scale, &d->cvb_vmin.cvb_pll_param);
		mvj = round_cvb_voltage(mvj, d->voltage_scale, align);
		if (mvj < rail->min_millivolts) {
			pr_debug("tegra21_dvfs: gpu min %dmV below rail min %dmV\n",
			     mvj, rail->min_millivolts);
			mvj = rail->min_millivolts;
		}

		gpu_vmin[j] = mvj;
	}

	/*
	 * Use CVB table to fill in gpu dvfs frequencies and voltages. Each
	 * CVB entry specifies gpu frequency and CVB coefficients to calculate
	 * the respective voltage.
	 */
	for (i = 0; i < MAX_DVFS_FREQS; i++) {
		table = &d->cvb_table[i];
		if (!table->freq || (table->freq > max_freq))
			break;

		mv = get_cvb_voltage(
			speedo, d->speedo_scale, &table->cvb_pll_param);

		for (j = 0; j < thermal_ranges; j++) {
			int mvj = mv;
			int t = rail->vts_cdev->trip_temperatures[j];

			/* get thermal offset for this trip-point */
			mvj += get_cvb_t_voltage(speedo, d->speedo_scale,
				t, d->thermal_scale, &table->cvb_pll_param);
			mvj = round_cvb_voltage(mvj, d->voltage_scale, align);

			/* clip to minimum, abort if above maximum */
			mvj = max(mvj, gpu_vmin[j]);
			if (mvj > d->max_mv)
				break;

			/* update voltage for adjacent ranges bounded by this
			   trip-point (cvb & dvfs are transpose matrices) */
			gpu_millivolts[j][i] = mvj;
			if (j && (gpu_millivolts[j-1][i] < mvj))
				gpu_millivolts[j-1][i] = mvj;
		}
		/* Make sure all voltages for this frequency are below max */
		if (j < thermal_ranges)
			break;

		/* fill in gpu dvfs tables */
		gpu_dvfs->freqs[i] = table->freq;
	}

	/*
	 * Table must not be empty, must have at least one entry in range, and
	 * must specify monotonically increasing voltage on frequency dependency
	 * in each temperature range.
	 */
	if (!i || tegra_dvfs_init_thermal_dvfs_voltages(&gpu_millivolts[0][0],
		gpu_peak_millivolts, i, thermal_ranges, gpu_dvfs)) {
		pr_err("tegra21_dvfs: invalid gpu dvfs table\n");
		return -ENOENT;
	}

	/* Shift out the 1st trip-point */
	for (j = 1; j < thermal_ranges; j++)
		rail->vts_cdev->trip_temperatures[j - 1] =
		rail->vts_cdev->trip_temperatures[j];

	/* dvfs tables are successfully populated - fill in the gpu dvfs */
	gpu_dvfs->speedo_id = d->speedo_id;
	gpu_dvfs->process_id = d->process_id;
	gpu_dvfs->freqs_mult = d->freqs_mult;
	gpu_dvfs->dvfs_rail->nominal_millivolts = d->max_mv;

	*max_freq_index = i - 1;

	/* Init thermal floors */
	if (d->therm_floors_table[0]) /* if table contains at least one entry */
		tegra_dvfs_rail_init_vmin_thermal_profile(d->vmin_trips_table,
			d->therm_floors_table, rail, NULL);

	return 0;
}

static void __init init_gpu_dvfs_table(int *gpu_max_freq_index)
{
	int i, ret;
	int gpu_speedo_id = tegra_gpu_speedo_id();
	int gpu_process_id = tegra_gpu_process_id();

	BUG_ON(gpu_speedo_id >= ARRAY_SIZE(gpu_max_freq));
	for (ret = 0, i = 0; i < ARRAY_SIZE(gpu_cvb_dvfs_table); i++) {
		struct gpu_cvb_dvfs *d = &gpu_cvb_dvfs_table[i];
		unsigned long max_freq = gpu_max_freq[gpu_speedo_id];
		if (match_dvfs_one("gpu cvb", d->speedo_id, d->process_id,
				   gpu_speedo_id, gpu_process_id)) {
			ret = set_gpu_dvfs_data(max_freq,
				d, &gpu_dvfs, gpu_max_freq_index);
			break;
		}
	}
	BUG_ON((i == ARRAY_SIZE(gpu_cvb_dvfs_table)) || ret);
}

/*
 * Clip sku-based core nominal voltage to core DVFS voltage ladder
 */
static int __init get_core_nominal_mv_index(int speedo_id)
{
	int i;
	int mv = tegra_core_speedo_mv();
	int core_edp_voltage = get_core_edp();

	/*
	 * Start with nominal level for the chips with this speedo_id. Then,
	 * make sure core nominal voltage is below edp limit for the board
	 * (if edp limit is set).
	 */
	if (!core_edp_voltage)
		core_edp_voltage = 1150;	/* default 1.15V EDP limit */

	mv = min(mv, core_edp_voltage);

	/* Round nominal level down to the nearest core scaling step */
	for (i = 0; i < MAX_DVFS_FREQS; i++) {
		if ((core_millivolts[i] == 0) || (mv < core_millivolts[i]))
			break;
	}

	if (i == 0) {
		pr_err("tegra21_dvfs: unable to adjust core dvfs table to"
		       " nominal voltage %d\n", mv);
		return -ENOSYS;
	}
	return i - 1;
}

int tegra_cpu_dvfs_alter(int edp_thermal_index, const cpumask_t *cpus,
			 bool before_clk_update, int cpu_event)
{
	/* empty definition for tegra21 */
	return 0;
}

void __init tegra21x_init_dvfs(void)
{
	int soc_speedo_id = tegra_soc_speedo_id();
	int core_process_id = tegra_core_process_id();

	int i, ret;
	int core_nominal_mv_index;
	int gpu_max_freq_index = 0;
	int cpu_max_freq_index = 0;

#ifndef CONFIG_TEGRA_CORE_DVFS
	tegra_dvfs_core_disabled = true;
#endif
#ifndef CONFIG_TEGRA_CPU_DVFS
	tegra_dvfs_cpu_disabled = true;
#endif
#ifndef CONFIG_TEGRA_GPU_DVFS
	tegra_dvfs_gpu_disabled = true;
#endif

	/*
	 * Find nominal voltages for core (1st) and cpu rails before rail
	 * init. Nominal voltage index in core scaling ladder can also be
	 * used to determine max dvfs frequencies for all core clocks. In
	 * case of error disable core scaling and set index to 0, so that
	 * core clocks would not exceed rates allowed at minimum voltage.
	 */
	core_nominal_mv_index = get_core_nominal_mv_index(soc_speedo_id);
	if (core_nominal_mv_index < 0) {
		tegra21_dvfs_rail_vdd_core.disabled = true;
		tegra_dvfs_core_disabled = true;
		core_nominal_mv_index = 0;
	}
	tegra21_dvfs_rail_vdd_core.nominal_millivolts =
		core_millivolts[core_nominal_mv_index];

	/*
	 * Construct CPU DVFS table from CVB data; find CPU maximum frequency,
	 * and nominal voltage.
	 */
	init_cpu_dvfs_table(&cpu_max_freq_index);

	/*
	 * Construct GPU DVFS table from CVB data; find GPU maximum frequency,
	 * and nominal voltage.
	 */
	init_gpu_dvfs_table(&gpu_max_freq_index);

	/* Init core thermal profile */
	if (vdd_core_therm_floors_table[0])
		tegra_dvfs_rail_init_vmin_thermal_profile(
			vdd_core_vmin_trips_table, vdd_core_therm_floors_table,
			&tegra21_dvfs_rail_vdd_core, NULL);
	if (vdd_core_therm_caps_table[0])
		tegra_dvfs_rail_init_vmax_thermal_profile(
			vdd_core_vmax_trips_table, vdd_core_therm_caps_table,
			&tegra21_dvfs_rail_vdd_core, NULL);

	/* Init rail structures and dependencies */
	tegra_dvfs_init_rails(tegra21_dvfs_rails,
		ARRAY_SIZE(tegra21_dvfs_rails));

	/* Search core dvfs table for speedo/process matching entries and
	   initialize dvfs-ed clocks */
	if (!tegra_platform_is_linsim()) {
		for (i = 0; i <  ARRAY_SIZE(core_dvfs_table); i++) {
			struct dvfs *d = &core_dvfs_table[i];
			if (!match_dvfs_one(d->clk_name, d->speedo_id,
				d->process_id, soc_speedo_id, core_process_id))
				continue;
			init_dvfs_one(d, core_nominal_mv_index);
		}
	}

	/* Initialize matching gpu dvfs entry already found when nominal
	   voltage was determined */
	init_dvfs_one(&gpu_dvfs, gpu_max_freq_index);

	/* Initialize matching cpu dvfs entry already found when nominal
	   voltage was determined */
	init_dvfs_one(&cpu_dvfs, cpu_max_freq_index);

	/* Finally disable dvfs on rails if necessary */
	if (tegra_dvfs_core_disabled)
		tegra_dvfs_rail_disable(&tegra21_dvfs_rail_vdd_core);
	if (tegra_dvfs_cpu_disabled)
		tegra_dvfs_rail_disable(&tegra21_dvfs_rail_vdd_cpu);
	if (tegra_dvfs_gpu_disabled)
		tegra_dvfs_rail_disable(&tegra21_dvfs_rail_vdd_gpu);

	pr_info("tegra dvfs: VDD_CPU nominal %dmV, scaling %s\n",
		tegra21_dvfs_rail_vdd_cpu.nominal_millivolts,
		tegra_dvfs_cpu_disabled ? "disabled" : "enabled");
	pr_info("tegra dvfs: VDD_CORE nominal %dmV, scaling %s\n",
		tegra21_dvfs_rail_vdd_core.nominal_millivolts,
		tegra_dvfs_core_disabled ? "disabled" : "enabled");
	pr_info("tegra dvfs: VDD_GPU nominal %dmV, scaling %s\n",
		tegra21_dvfs_rail_vdd_gpu.nominal_millivolts,
		tegra_dvfs_gpu_disabled ? "disabled" : "enabled");
}

int tegra_dvfs_rail_disable_prepare(struct dvfs_rail *rail)
{
	return 0;
}

int tegra_dvfs_rail_post_enable(struct dvfs_rail *rail)
{
	return 0;
}

/* Core voltage and bus cap object and tables */
static struct kobject *cap_kobj;
static struct kobject *gpu_kobj;

static struct core_dvfs_cap_table tegra21_core_cap_table[] = {
	{ .cap_name = "cap.vcore.c2bus" },
	{ .cap_name = "cap.vcore.c3bus" },
	{ .cap_name = "cap.vcore.sclk" },
	{ .cap_name = "cap.vcore.emc" },
	{ .cap_name = "cap.vcore.host1x" },
};

static struct core_bus_limit_table tegra21_gpu_cap_syfs = {
	.limit_clk_name = "cap.profile.gbus",
	.refcnt_attr = {.attr = {.name = "gpu_cap_state", .mode = 0644} },
	.level_attr  = {.attr = {.name = "gpu_cap_rate", .mode = 0644} },
	.pm_qos_class = PM_QOS_GPU_FREQ_MAX,
};

static struct core_bus_limit_table tegra21_gpu_floor_sysfs = {
	.limit_clk_name = "floor.profile.gbus",
	.refcnt_attr = {.attr = {.name = "gpu_floor_state", .mode = 0644} },
	.level_attr  = {.attr = {.name = "gpu_floor_rate", .mode = 0644} },
	.pm_qos_class = PM_QOS_GPU_FREQ_MIN,
};

static struct core_bus_rates_table tegra21_gpu_rates_sysfs = {
	.bus_clk_name = "gbus",
	.rate_attr = {.attr = {.name = "gpu_rate", .mode = 0444} },
	.available_rates_attr = {
		.attr = {.name = "gpu_available_rates", .mode = 0444} },
};


static int __init tegra21_dvfs_init_core_cap(void)
{
	int ret;
	const int hack_core_millivolts = 0;

	cap_kobj = kobject_create_and_add("tegra_cap", kernel_kobj);
	if (!cap_kobj) {
		pr_err("tegra21_dvfs: failed to create sysfs cap object\n");
		return 0;
	}

	/* FIXME: skip core cap init b/c it's too slow on QT */
	if (tegra_platform_is_qt())
		ret = tegra_init_core_cap(
			tegra21_core_cap_table, ARRAY_SIZE(tegra21_core_cap_table),
			&hack_core_millivolts, 1, cap_kobj);
	else
		ret = tegra_init_core_cap(
			tegra21_core_cap_table, ARRAY_SIZE(tegra21_core_cap_table),
			core_millivolts, ARRAY_SIZE(core_millivolts), cap_kobj);

	if (ret) {
		pr_err("tegra21_dvfs: failed to init core cap interface (%d)\n",
		       ret);
		kobject_del(cap_kobj);
		return 0;
	}
	pr_info("tegra dvfs: tegra sysfs cap interface is initialized\n");

	gpu_kobj = kobject_create_and_add("tegra_gpu", kernel_kobj);
	if (!gpu_kobj) {
		pr_err("tegra21_dvfs: failed to create sysfs gpu object\n");
		return 0;
	}

	ret = tegra_init_shared_bus_cap(&tegra21_gpu_cap_syfs,
					1, gpu_kobj);
	if (ret) {
		pr_err("tegra21_dvfs: failed to init gpu cap interface (%d)\n",
		       ret);
		kobject_del(gpu_kobj);
		return 0;
	}

	ret = tegra_init_shared_bus_floor(&tegra21_gpu_floor_sysfs,
					  1, gpu_kobj);
	if (ret) {
		pr_err("tegra21_dvfs: failed to init gpu floor interface (%d)\n",
		       ret);
		kobject_del(gpu_kobj);
		return 0;
	}

	ret = tegra_init_sysfs_shared_bus_rate(&tegra21_gpu_rates_sysfs,
					       1, gpu_kobj);
	if (ret) {
		pr_err("tegra21_dvfs: failed to init gpu rates interface (%d)\n",
		       ret);
		kobject_del(gpu_kobj);
		return 0;
	}

	pr_info("tegra dvfs: tegra sysfs gpu interface is initialized\n");

	return 0;
}
late_initcall(tegra21_dvfs_init_core_cap);
