/*
 * Copyright (c) 2015, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#ifndef __ISC_DEV_PRIV_H__
#define __ISC_DEV_PRIV_H__

struct isc_dev_info {
	struct i2c_client *i2c_client;
	struct device *dev;
	struct miscdevice miscdev;
	struct regmap_config regmap_cfg;
	struct regmap *regmap;
	struct isc_dev_platform_data *pdata;
	atomic_t in_use;
	struct mutex mutex;
	struct isc_dev_package package;
	struct isc_dev_pkg rw_pkg;
	struct dentry *d_entry;
	u32 reg_off;
	char devname[32];
	u8 power_is_on;
#if (defined(DEBUG) || defined(DEBUG_I2C_TRAFFIC))
	char dump_buf[32 + 3 * 16];
#endif
};

int isc_dev_raw_rd(struct isc_dev_info *, unsigned int, void *, size_t);
int isc_dev_raw_wr(struct isc_dev_info *, unsigned int, void *, size_t);

int isc_dev_debugfs_init(struct isc_dev_info *isc_dev);
int isc_dev_debugfs_remove(struct isc_dev_info *isc_dev);

#endif  /* __ISC_DEV_PRIV_H__ */
