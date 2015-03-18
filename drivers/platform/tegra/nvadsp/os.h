/*
 * os.h
 *
 * A header file containing data structures shared with ADSP OS
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
#ifndef __TEGRA_NVADSP_OS_H
#define __TEGRA_NVADSP_OS_H
#include <linux/firmware.h>

#define CONFIG_USE_STATIC_APP_LOAD	0

struct app_mem_size {
	uint64_t dram;
	uint64_t dram_shared;
	uint64_t dram_shared_wc;
	uint64_t aram;
};

struct adsp_module {
	const char *name;
	void *module_ptr;
	uint32_t adsp_module_ptr;
	size_t size;
	const struct app_mem_size mem_size;
};

int nvadsp_os_probe(struct platform_device *);
int nvadsp_app_module_probe(struct platform_device *);
int adsp_add_load_mappings(phys_addr_t, void *, int);
void *get_mailbox_shared_region(void);
struct elf32_shdr *nvadsp_get_section(const struct firmware *fw,
						char *sec_name);
uint32_t find_global_symbol(const char *);
void update_nvadsp_app_shared_ptr(void *);
struct adsp_module
*load_adsp_module(const char *, const char *, struct device *);
int allocate_memory_from_adsp(void **, unsigned int);
#endif /* __TEGRA_NVADSP_OS_H */
