/*
 * dev.h
 *
 * A header file for Host driver for ADSP and APE
 *
 * Copyright (C) 2014 NVIDIA Corporation. All rights reserved.
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

#ifndef __TEGRA_NVADSP_DEV_H
#define __TEGRA_NVADSP_DEV_H

#include <linux/tegra_nvadsp.h>
#include <linux/ioport.h>

#include "hwmailbox.h"

/*
 * Note: These enums should be aligned to the regs mentioned in the
 * device tree
*/
enum {
	AMC,
	AMISC,
	APE_MAX_REG
};

enum {
	ADSP_DRAM1,
	ADSP_DRAM2,
	ADSP_MAX_DRAM_MAP
};

struct nvadsp_drv_data {
	void __iomem **base_regs;
	struct resource *dram_region[ADSP_MAX_DRAM_MAP];
	struct hwmbox_queue hwmbox_send_queue;
	int hwmbox_send_virq;
	int hwmbox_recv_virq;

	struct nvadsp_mbox **mboxes;
	unsigned long *mbox_ids;
	spinlock_t mbox_lock;
};

#endif /* __TEGRA_NVADSP_DEV_H */
