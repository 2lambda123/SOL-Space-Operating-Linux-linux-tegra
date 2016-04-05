/*
 * drivers/video/tegra/nvmap/nvmap_fault.c
 *
 * Copyright (c) 2011-2015, NVIDIA CORPORATION. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#define pr_fmt(fmt)	"%s: " fmt, __func__

#include <trace/events/nvmap.h>

#include "nvmap_priv.h"

static void nvmap_vma_close(struct vm_area_struct *vma);
static int nvmap_vma_fault(struct vm_area_struct *vma, struct vm_fault *vmf);

struct vm_operations_struct nvmap_vma_ops = {
	.open		= nvmap_vma_open,
	.close		= nvmap_vma_close,
	.fault		= nvmap_vma_fault,
};

int is_nvmap_vma(struct vm_area_struct *vma)
{
	return vma->vm_ops == &nvmap_vma_ops;
}

/* to ensure that the backing store for the VMA isn't freed while a fork'd
 * reference still exists, nvmap_vma_open increments the reference count on
 * the handle, and nvmap_vma_close decrements it. alternatively, we could
 * disallow copying of the vma, or behave like pmem and zap the pages. FIXME.
*/
void nvmap_vma_open(struct vm_area_struct *vma)
{
	struct nvmap_vma_priv *priv;
	struct nvmap_handle *h;
	struct nvmap_vma_list *vma_list, *tmp;
	struct list_head *tmp_head = NULL;
	pid_t current_pid = task_tgid_nr(current);
	bool vma_pos_found = false;
	int nr_page, i;
	ulong vma_open_count;

	priv = vma->vm_private_data;
	BUG_ON(!priv);
	BUG_ON(!priv->handle);

	h = priv->handle;
	nvmap_umaps_inc(h);

	mutex_lock(&h->lock);
	vma_open_count = atomic_inc_return(&priv->count);
	if (vma_open_count == 1 && h->heap_pgalloc) {
		nr_page = PAGE_ALIGN(h->size) >> PAGE_SHIFT;
		for (i = 0; i < nr_page; i++) {
			struct page *page = nvmap_to_page(h->pgalloc.pages[i]);
			/* This is necessry to avoid page being accounted
			 * under NR_FILE_MAPPED. This way NR_FILE_MAPPED would
			 * be fully accounted under NR_FILE_PAGES. This allows
			 * Android low mem killer detect low memory condition
			 * precisely.
			 * This has a side effect of inaccurate pss accounting
			 * for NvMap memory mapped into user space. Android
			 * procrank and NvMap Procrank both would have same
			 * issue. Subtracting NvMap_Procrank pss from
			 * procrank pss would give non-NvMap pss held by process
			 * and adding NvMap memory used by process represents
			 * entire memroy consumption by the process.
			 */
			atomic_inc(&page->_mapcount);
		}
	}
	mutex_unlock(&h->lock);

	vma_list = kmalloc(sizeof(*vma_list), GFP_KERNEL);
	if (vma_list) {
		mutex_lock(&h->lock);
		tmp_head = &h->vmas;

		/* insert vma into handle's vmas list in the increasing order of
		 * handle offsets
		 */
		list_for_each_entry(tmp, &h->vmas, list) {
			BUG_ON(tmp->vma == vma);

			if (!vma_pos_found && (current_pid == tmp->pid)) {
				if (vma->vm_pgoff < tmp->vma->vm_pgoff) {
					tmp_head = &tmp->list;
					vma_pos_found = true;
				} else {
					tmp_head = tmp->list.next;
				}
			}
		}

		vma_list->vma = vma;
		vma_list->pid = current_pid;
		list_add_tail(&vma_list->list, tmp_head);
		mutex_unlock(&h->lock);
	} else {
		WARN(1, "vma not tracked");
	}
}

static void nvmap_vma_close(struct vm_area_struct *vma)
{
	struct nvmap_vma_priv *priv = vma->vm_private_data;
	struct nvmap_vma_list *vma_list;
	struct nvmap_handle *h;
	bool vma_found = false;
	int nr_page, i;

	if (!priv)
		return;

	BUG_ON(!priv->handle);

	h = priv->handle;
	nr_page = PAGE_ALIGN(h->size) >> PAGE_SHIFT;

	mutex_lock(&h->lock);
	list_for_each_entry(vma_list, &h->vmas, list) {
		if (vma_list->vma != vma)
			continue;
		list_del(&vma_list->list);
		kfree(vma_list);
		vma_found = true;
		break;
	}
	BUG_ON(!vma_found);
	nvmap_umaps_dec(h);

	if (__atomic_add_unless(&priv->count, -1, 0) == 1) {
		if (h->heap_pgalloc) {
			for (i = 0; i < nr_page; i++) {
				struct page *page;
				page = nvmap_to_page(h->pgalloc.pages[i]);
				atomic_dec(&page->_mapcount);
			}
		}
		mutex_unlock(&h->lock);
		if (priv->handle)
			nvmap_handle_put(priv->handle);
		vma->vm_private_data = NULL;
		kfree(priv);
	} else {
		mutex_unlock(&h->lock);
	}
}

static int nvmap_vma_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	struct page *page;
	struct nvmap_vma_priv *priv;
	unsigned long offs;

	offs = (unsigned long)(vmf->virtual_address - vma->vm_start);
	priv = vma->vm_private_data;
	if (!priv || !priv->handle || !priv->handle->alloc)
		return VM_FAULT_SIGBUS;

	offs += priv->offs;
	/* if the VMA was split for some reason, vm_pgoff will be the VMA's
	 * offset from the original VMA */
	offs += (vma->vm_pgoff << PAGE_SHIFT);

	if (offs >= priv->handle->size)
		return VM_FAULT_SIGBUS;

	if (!priv->handle->heap_pgalloc) {
		unsigned long pfn;
		BUG_ON(priv->handle->carveout->base & ~PAGE_MASK);
		pfn = ((priv->handle->carveout->base + offs) >> PAGE_SHIFT);
		if (!pfn_valid(pfn)) {
			vm_insert_pfn(vma,
				(unsigned long)vmf->virtual_address, pfn);
			return VM_FAULT_NOPAGE;
		}
		/* CMA memory would get here */
		page = pfn_to_page(pfn);
	} else {
		offs >>= PAGE_SHIFT;
		if (nvmap_page_reserved(priv->handle->pgalloc.pages[offs]))
			return VM_FAULT_SIGBUS;
		page = nvmap_to_page(priv->handle->pgalloc.pages[offs]);

		if (nvmap_handle_track_dirty(priv->handle)) {
			mutex_lock(&priv->handle->lock);
			if (nvmap_page_mkdirty(&priv->handle->pgalloc.pages[offs]))
				atomic_inc(&priv->handle->pgalloc.ndirty);
			mutex_unlock(&priv->handle->lock);
		}
	}

	if (page)
		get_page(page);
	vmf->page = page;
	return (page) ? 0 : VM_FAULT_SIGBUS;
}
