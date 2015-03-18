/*
 * A Header file for managing ADSP/APE
 *
 * Copyright (c) 2014, NVIDIA CORPORATION.  All rights reserved.
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

#ifndef __LINUX_TEGRA_NVADSP_H
#define __LINUX_TEGRA_NVADSP_H

#include <linux/types.h>
#include <linux/timer.h>
#include <linux/device.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/list.h>

struct nvadsp_platform_data {
	phys_addr_t co_pa;
	unsigned long co_size;
};

typedef int status_t;

/*
 * Shared Semaphores
 */
typedef struct {
	int magic; /* 'ssem' */
	uint8_t id;
	wait_queue_head_t wait;
	struct timer_list timer;
} nvadsp_shared_sema_t;

nvadsp_shared_sema_t *
nvadsp_shared_sema_init(uint8_t nvadsp_shared_sema_id);
status_t nvadsp_shared_sema_destroy(nvadsp_shared_sema_t *);
status_t nvadsp_shared_sema_acquire(nvadsp_shared_sema_t *);
status_t nvadsp_shared_sema_release(nvadsp_shared_sema_t *);

/*
 * Arbitrated Semaphores
 */
typedef struct {
	int magic; /* 'asem' */
	uint8_t  id;
	wait_queue_head_t wait;
	struct completion comp;
} nvadsp_arb_sema_t;

nvadsp_arb_sema_t *nvadsp_arb_sema_init(uint8_t nvadsp_arb_sema_id);
status_t nvadsp_arb_sema_destroy(nvadsp_arb_sema_t *);
status_t nvadsp_arb_sema_acquire(nvadsp_arb_sema_t *);
status_t nvadsp_arb_sema_release(nvadsp_arb_sema_t *);

/*
 * Mailbox Queue
 */
#define NVADSP_MBOX_QUEUE_SIZE		32
#define NVADSP_MBOX_QUEUE_SIZE_MASK	(NVADSP_MBOX_QUEUE_SIZE - 1)
struct nvadsp_mbox_queue {
	uint32_t array[NVADSP_MBOX_QUEUE_SIZE];
	uint16_t head;
	uint16_t tail;
	uint16_t count;
	struct completion comp;
	spinlock_t lock;
};

status_t nvadsp_mboxq_enqueue(struct nvadsp_mbox_queue *, uint32_t);

/*
 * Mailbox
 */
#define NVADSP_MBOX_NAME_MAX (16 + 1)

typedef status_t (*nvadsp_mbox_handler_t)(uint32_t, void *);

struct nvadsp_mbox {
	uint16_t id;
	char name[NVADSP_MBOX_NAME_MAX];
	struct nvadsp_mbox_queue recv_queue;
	nvadsp_mbox_handler_t handler;
	void *hdata;
};

#define NVADSP_MBOX_SMSG       0x1
#define NVADSP_MBOX_LMSG       0x2

status_t nvadsp_mbox_open(struct nvadsp_mbox *mbox, uint16_t *mid,
			  const char *name, nvadsp_mbox_handler_t handler,
			  void *hdata);
status_t nvadsp_mbox_send(struct nvadsp_mbox *mbox, uint32_t data,
			  uint32_t flags, bool block, unsigned int timeout);
status_t nvadsp_mbox_recv(struct nvadsp_mbox *mbox, uint32_t *data, bool block,
			  unsigned int timeout);
status_t nvadsp_mbox_close(struct nvadsp_mbox *mbox);

status_t nvadsp_hwmbox_send_data(uint16_t, uint32_t, uint32_t);

/*
 * Circular Message Queue
 */
typedef struct _msgq_message_t {
	int32_t size;		/* size of payload in words */
	int32_t payload[1];	/* variable length payload */
} msgq_message_t;

#define MSGQ_MESSAGE_HEADER_SIZE \
	(sizeof(msgq_message_t) - sizeof(((msgq_message_t *)0)->payload))
#define MSGQ_MESSAGE_HEADER_WSIZE \
	(MSGQ_MESSAGE_HEADER_SIZE / sizeof(int32_t))

typedef struct _msgq_t {
	int32_t size;		/* queue size in words */
	int32_t write_index;	/* queue write index */
	int32_t read_index;	/* queue read index */
	int32_t queue[1];	/* variable length queue */
} msgq_t;

#define MSGQ_HEADER_SIZE	(sizeof(msgq_t) - sizeof(((msgq_t *)0)->queue))
#define MSGQ_HEADER_WSIZE	(MSGQ_HEADER_SIZE / sizeof(int32_t))
#define MSGQ_MAX_QUEUE_WSIZE	(8192 - MSGQ_HEADER_WSIZE)
#define MSGQ_MSG_SIZE(x) \
	(((sizeof(x) + sizeof(int32_t) - 1) & (~(sizeof(int32_t)-1))) >> 2)

void msgq_init(msgq_t *msgq, int32_t size);
int32_t msgq_queue_message(msgq_t *msgq, const msgq_message_t *message);
int32_t msgq_dequeue_message(msgq_t *msgq, msgq_message_t *message);
#define msgq_discard_message(msgq) msgq_dequeue_message(msgq, NULL)

/*
 * DRAM Sharing
 */
typedef dma_addr_t nvadsp_iova_addr_t;
typedef enum dma_data_direction nvadsp_data_direction_t;

nvadsp_iova_addr_t
nvadsp_dram_map_single(struct device *nvadsp_dev,
		       void *cpu_addr, size_t size,
		       nvadsp_data_direction_t direction);
void
nvadsp_dram_unmap_single(struct device *nvadsp_dev,
			 nvadsp_iova_addr_t iova_addr, size_t size,
			 nvadsp_data_direction_t direction);

nvadsp_iova_addr_t
nvadsp_dram_map_page(struct device *nvadsp_dev,
		     struct page *page, unsigned long offset, size_t size,
		     nvadsp_data_direction_t direction);
void
nvadsp_dram_unmap_page(struct device *nvadsp_dev,
		       nvadsp_iova_addr_t iova_addr, size_t size,
		       nvadsp_data_direction_t direction);

void
nvadsp_dram_sync_single_for_cpu(struct device *nvadsp_dev,
				nvadsp_iova_addr_t iova_addr, size_t size,
				nvadsp_data_direction_t direction);
void
nvadsp_dram_sync_single_for_device(struct device *nvadsp_dev,
				   nvadsp_iova_addr_t iova_addr, size_t size,
				   nvadsp_data_direction_t direction);

/*
 * ARAM Bookkeeping
 */
bool nvadsp_aram_request(char *start, size_t size, char *id);
void nvadsp_aram_release(char *start, size_t size);

/*
 * ADSP OS
 */
void nvadsp_adsp_init(void);
int nvadsp_os_load(void);
int nvadsp_os_start(void);
void nvadsp_os_stop(void);

/*
 * ADSP OS App
 */
#define NVADSP_NAME_SZ	64

#define ARGV_SIZE_IN_WORDS         128

typedef const void *nvadsp_app_handle_t;

typedef struct adsp_app_mem {
	/* DRAM segment*/
	void      *dram;
	/* DRAM in shared memory segment. uncached */
	void      *shared;
	/* DRAM in shared memory segment. write combined */
	void      *shared_wc;
	/*  ARAM if available, DRAM OK */
	void      *aram;
	/* ARAM Segment. exclusively */
	void      *aram_x;
	/* set to 1 if ARAM allocation succeeded */
	uint32_t   aram_x_flag;
} adsp_app_mem_t;


typedef struct nvadsp_app_args {
	 /* number of arguments passed in */
	int32_t  argc;
	/* binary representation of arguments,*/
	int32_t  argv[ARGV_SIZE_IN_WORDS];
} nvadsp_app_args_t;

typedef struct {
	const char *name;
	const uint32_t token;
	const int state;
	adsp_app_mem_t mem;
	struct list_head node;
	uint32_t stack_size;
} nvadsp_app_info_t;

nvadsp_app_handle_t __must_check
nvadsp_app_load(const char *, const char *);
nvadsp_app_info_t __must_check
*nvadsp_app_init(nvadsp_app_handle_t, nvadsp_app_args_t *);
void nvadsp_app_unload(nvadsp_app_handle_t);
int __must_check nvadsp_app_start(nvadsp_app_info_t *);
int nvadsp_app_stop(nvadsp_app_info_t *);
void *nvadsp_alloc_coherent(size_t, dma_addr_t *, gfp_t);
void nvadsp_free_coherent(size_t, void *, dma_addr_t);
void *nvadsp_da_to_va_mappings(u64, int);

#endif /* __LINUX_TEGRA_NVADSP_H */
