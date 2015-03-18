/*
 * drivers/video/tegra/host/gk20a/gk20a_sysfs.c
 *
 * GK20A Graphics
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

#include <linux/platform_device.h>
#include <linux/kernel.h>
#include <linux/fb.h>

#include "../dev.h"
#include "gk20a.h"
#include "gr_gk20a.h"
#include "fifo_gk20a.h"
#include "gk20a_gating_reglist.h"
#include "nvhost_acm.h"

static ssize_t elcg_enable_store(struct device *device,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct platform_device *ndev = to_platform_device(device);
	struct gk20a *g = get_gk20a(ndev);
	unsigned long val = 0;

	if (kstrtoul(buf, 10, &val) < 0)
		return -EINVAL;

	nvhost_module_busy(g->dev);
	if (val) {
		g->elcg_enabled = true;
		gr_gk20a_init_elcg_mode(g, ELCG_AUTO, ENGINE_GR_GK20A);
		gr_gk20a_init_elcg_mode(g, ELCG_AUTO, ENGINE_CE2_GK20A);
	} else {
		g->elcg_enabled = false;
		gr_gk20a_init_elcg_mode(g, ELCG_RUN, ENGINE_GR_GK20A);
		gr_gk20a_init_elcg_mode(g, ELCG_RUN, ENGINE_CE2_GK20A);
	}
	nvhost_module_idle(g->dev);

	dev_info(device, "ELCG is %s.\n", g->elcg_enabled ? "enabled" :
			"disabled");

	return count;
}

static ssize_t elcg_enable_read(struct device *device,
	struct device_attribute *attr, char *buf)
{
	struct platform_device *ndev = to_platform_device(device);
	struct gk20a *g = get_gk20a(ndev);

	return sprintf(buf, "%d\n", g->elcg_enabled ? 1 : 0);
}

static DEVICE_ATTR(elcg_enable, S_IRWXUGO, elcg_enable_read, elcg_enable_store);

static ssize_t blcg_enable_store(struct device *device,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct platform_device *ndev = to_platform_device(device);
	struct gk20a *g = get_gk20a(ndev);
	unsigned long val = 0;

	if (kstrtoul(buf, 10, &val) < 0)
		return -EINVAL;

	if (val)
		g->blcg_enabled = true;
	else
		g->blcg_enabled = false;

	nvhost_module_busy(g->dev);
	gr_gk20a_blcg_gr_load_gating_prod(g, g->blcg_enabled);
	nvhost_module_idle(g->dev);

	dev_info(device, "BLCG is %s.\n", g->blcg_enabled ? "enabled" :
			"disabled");

	return count;
}

static ssize_t blcg_enable_read(struct device *device,
	struct device_attribute *attr, char *buf)
{
	struct platform_device *ndev = to_platform_device(device);
	struct gk20a *g = get_gk20a(ndev);

	return sprintf(buf, "%d\n", g->blcg_enabled ? 1 : 0);
}

static DEVICE_ATTR(blcg_enable, S_IRWXUGO, blcg_enable_read, blcg_enable_store);

static ssize_t slcg_enable_store(struct device *device,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct platform_device *ndev = to_platform_device(device);
	struct gk20a *g = get_gk20a(ndev);
	unsigned long val = 0;

	if (kstrtoul(buf, 10, &val) < 0)
		return -EINVAL;

	if (val)
		g->slcg_enabled = true;
	else
		g->slcg_enabled = false;

	/*
	 * TODO: slcg_therm_load_gating is not enabled anywhere during
	 * init. Therefore, it would be incongruous to add it here. Once
	 * it is added to init, we should add it here too.
	 */
	nvhost_module_busy(g->dev);
	gr_gk20a_slcg_gr_load_gating_prod(g, g->slcg_enabled);
	gr_gk20a_slcg_perf_load_gating_prod(g, g->slcg_enabled);
	nvhost_module_idle(g->dev);

	dev_info(device, "SLCG is %s.\n", g->slcg_enabled ? "enabled" :
			"disabled");

	return count;
}

static ssize_t slcg_enable_read(struct device *device,
	struct device_attribute *attr, char *buf)
{
	struct platform_device *ndev = to_platform_device(device);
	struct gk20a *g = get_gk20a(ndev);

	return sprintf(buf, "%d\n", g->slcg_enabled ? 1 : 0);
}

static DEVICE_ATTR(slcg_enable, S_IRWXUGO, slcg_enable_read, slcg_enable_store);

void gk20a_remove_sysfs(struct device *dev)
{
	device_remove_file(dev, &dev_attr_elcg_enable);
	device_remove_file(dev, &dev_attr_blcg_enable);
	device_remove_file(dev, &dev_attr_slcg_enable);
}

void gk20a_create_sysfs(struct platform_device *dev)
{
	int error = 0;

	error |= device_create_file(&dev->dev, &dev_attr_elcg_enable);
	error |= device_create_file(&dev->dev, &dev_attr_blcg_enable);
	error |= device_create_file(&dev->dev, &dev_attr_slcg_enable);

	if (error)
		dev_err(&dev->dev, "Failed to create sysfs attributes!\n");
}
