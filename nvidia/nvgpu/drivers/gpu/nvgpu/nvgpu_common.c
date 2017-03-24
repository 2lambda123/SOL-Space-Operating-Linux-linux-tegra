/*
 * Copyright (c) 2016, NVIDIA CORPORATION.  All rights reserved.
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

#include <linux/dma-mapping.h>
#include <linux/firmware.h>

#include "nvgpu_common.h"
#include "gk20a/gk20a_scale.h"
#include "gk20a/gk20a.h"
#include "nvgpu_common.h"

#define EMC3D_DEFAULT_RATIO 750

static void nvgpu_init_vars(struct gk20a *g)
{
	struct gk20a_platform *platform = dev_get_drvdata(g->dev);

	init_waitqueue_head(&g->sw_irq_stall_last_handled_wq);
	init_waitqueue_head(&g->sw_irq_nonstall_last_handled_wq);
	gk20a_init_gr(g);

	init_rwsem(&g->busy_lock);

	spin_lock_init(&g->mc_enable_lock);

	mutex_init(&platform->railgate_lock);
	mutex_init(&g->dbg_sessions_lock);
	mutex_init(&g->client_lock);
	mutex_init(&g->ch_wdt_lock);
	mutex_init(&g->poweroff_lock);

	g->regs_saved = g->regs;
	g->bar1_saved = g->bar1;

	g->emc3d_ratio = EMC3D_DEFAULT_RATIO;

	/* Set DMA parameters to allow larger sgt lists */
	g->dev->dma_parms = &g->dma_parms;
	dma_set_max_seg_size(g->dev, UINT_MAX);

}

static void nvgpu_init_timeout(struct gk20a *g)
{
	g->gr_idle_timeout_default = CONFIG_GK20A_DEFAULT_TIMEOUT;
	if (tegra_platform_is_silicon())
		g->timeouts_enabled = true;
}

static void nvgpu_init_timeslice(struct gk20a *g)
{
	g->runlist_interleave = true;

	g->timeslice_low_priority_us = 1300;
	g->timeslice_medium_priority_us = 2600;
	g->timeslice_high_priority_us = 5200;
}

static void nvgpu_init_pm_vars(struct gk20a *g)
{
	struct gk20a_platform *platform = dev_get_drvdata(g->dev);

	/*
	 * Set up initial power settings. For non-slicon platforms, disable
	 * power features and for silicon platforms, read from platform data
	 */
	g->slcg_enabled =
		tegra_platform_is_silicon() ? platform->enable_slcg : false;
	g->blcg_enabled =
		tegra_platform_is_silicon() ? platform->enable_blcg : false;
	g->elcg_enabled =
		tegra_platform_is_silicon() ? platform->enable_elcg : false;
	g->elpg_enabled =
		tegra_platform_is_silicon() ? platform->enable_elpg : false;
	g->aelpg_enabled =
		tegra_platform_is_silicon() ? platform->enable_aelpg : false;

	/* set default values to aelpg parameters */
	g->pmu.aelpg_param[0] = APCTRL_SAMPLING_PERIOD_PG_DEFAULT_US;
	g->pmu.aelpg_param[1] = APCTRL_MINIMUM_IDLE_FILTER_DEFAULT_US;
	g->pmu.aelpg_param[2] = APCTRL_MINIMUM_TARGET_SAVING_DEFAULT_US;
	g->pmu.aelpg_param[3] = APCTRL_POWER_BREAKEVEN_DEFAULT_US;
	g->pmu.aelpg_param[4] = APCTRL_CYCLES_PER_SAMPLE_MAX_DEFAULT;
}

static void nvgpu_init_mm_vars(struct gk20a *g)
{
	struct gk20a_platform *platform = dev_get_drvdata(g->dev);

	g->mm.bypass_smmu = platform->bypass_smmu;
	g->mm.disable_bigpage = platform->disable_bigpage;
	g->mm.vidmem_is_vidmem = platform->vidmem_is_vidmem;
}

int nvgpu_probe(struct gk20a *g,
		const char *debugfs_symlink,
		const char *interface_name,
		struct class *class)
{
	struct gk20a_platform *platform = dev_get_drvdata(g->dev);
	int err = 0;

	nvgpu_init_vars(g);
	nvgpu_init_timeout(g);
	nvgpu_init_timeslice(g);
	nvgpu_init_pm_vars(g);

	err = gk20a_user_init(g->dev, interface_name, class);
	if (err)
		return err;

	/* Initialize the platform interface. */
	err = platform->probe(g->dev);
	if (err) {
		dev_err(g->dev, "platform probe failed");
		return err;
	}

	/* Initialise scaling */
	if (IS_ENABLED(CONFIG_GK20A_DEVFREQ))
		gk20a_scale_init(g->dev);

	err = gk20a_secure_page_alloc(g->dev);
	if (err)
		dev_err(g->dev,
			"failed to allocate secure buffer %d\n", err);

	if (platform->late_probe) {
		err = platform->late_probe(g->dev);
		if (err) {
			dev_err(g->dev, "late probe failed");
			return err;
		}
	}

	nvgpu_init_mm_vars(g);

	gk20a_create_sysfs(g->dev);
	gk20a_debug_init(g->dev, debugfs_symlink);

	g->remove_support = gk20a_remove_support;

	return 0;
}

static const struct firmware *do_request_firmware(struct device *dev,
		const char *prefix, const char *fw_name, int flags)
{
	const struct firmware *fw;
	char *fw_path = NULL;
	int path_len, err;

	if (prefix) {
		path_len = strlen(prefix) + strlen(fw_name);
		path_len += 2; /* for the path separator and zero terminator*/

		fw_path = kzalloc(sizeof(*fw_path) * path_len, GFP_KERNEL);
		if (!fw_path)
			return NULL;

		sprintf(fw_path, "%s/%s", prefix, fw_name);
		fw_name = fw_path;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,18,0)
	err = request_firmware(&fw, fw_name, dev);
#else
	if (flags & NVGPU_REQUEST_FIRMWARE_NO_WARN)
		err = request_firmware_direct(&fw, fw_name, dev);
	else
		err = request_firmware(&fw, fw_name, dev);
#endif

	kfree(fw_path);
	if (err)
		return NULL;
	return fw;
}

/* This is a simple wrapper around request_firmware that takes 'fw_name' and
 * applies an IP specific relative path prefix to it. The caller is
 * responsible for calling release_firmware later. */
const struct firmware *nvgpu_request_firmware(struct gk20a *g,
					      const char *fw_name,
					      int flags)
{
	struct device *dev = g->dev;
	const struct firmware *fw;

	/* current->fs is NULL when calling from SYS_EXIT.
	   Add a check here to prevent crash in request_firmware */
	if (!current->fs || !fw_name)
		return NULL;

	BUG_ON(!g->ops.name);
	fw = do_request_firmware(dev, g->ops.name, fw_name, flags);

#ifdef CONFIG_TEGRA_GK20A
	/* TO BE REMOVED - Support loading from legacy SOC specific path. */
	if (!fw && !(flags & NVGPU_REQUEST_FIRMWARE_NO_SOC)) {
		struct gk20a_platform *platform = gk20a_get_platform(dev);
		fw = do_request_firmware(dev,
				platform->soc_name, fw_name, flags);
	}
#endif

	return fw;
}

