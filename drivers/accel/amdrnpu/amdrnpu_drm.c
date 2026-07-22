// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#include "drm/amdrnpu_accel.h"
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_mode.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/iosys-map.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#include "amdrnpu_internal.h"

static const struct drm_gem_object_funcs amdrnpu_gem_funcs;

static struct amdrnpu_gem *amdrnpu_gem_alloc(struct amdrnpu_dev *rnpu,
					     struct amdrnpu_bank *bank,
					     size_t size)
{
	struct amdrnpu_gem *bo;
	int ret;

	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo)
		return ERR_PTR(-ENOMEM);

	bo->bank = bank;
	bo->size = PAGE_ALIGN(size);
	bo->base.funcs = &amdrnpu_gem_funcs;

	ret = drm_gem_object_init(&rnpu->ddev, &bo->base, bo->size);
	if (ret) {
		kfree(bo);
		return ERR_PTR(ret);
	}
	return bo;
}

static void amdrnpu_gem_free(struct drm_gem_object *obj)
{
	struct amdrnpu_gem *bo = to_amdrnpu_gem(obj);

	if (bo->kvaddr)
		dma_free_noncoherent(bo->bank->dev, bo->size,
				     bo->kvaddr, bo->dma_addr, bo->dma_dir);

	drm_gem_object_release(obj);
	kfree(bo);
}

static int amdrnpu_gem_obj_mmap(struct drm_gem_object *obj,
				struct vm_area_struct *vma)
{
	struct amdrnpu_gem *bo = to_amdrnpu_gem(obj);
	unsigned long pgoff = vma->vm_pgoff - drm_vma_node_start(&obj->vma_node);
	unsigned long vpages = vma_pages(vma);
	unsigned long bopages = bo->size >> PAGE_SHIFT;

	if (pgoff >= bopages || vpages > bopages - pgoff)
		return -EINVAL;

	vm_flags_clear(vma, VM_PFNMAP);
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_pgoff = pgoff;

	return dma_mmap_pages(bo->bank->dev, vma, bo->size, bo->page);
}

static const struct vm_operations_struct amdrnpu_gem_vm_ops = {
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

static int amdrnpu_gem_vmap(struct drm_gem_object *obj, struct iosys_map *map)
{
	struct amdrnpu_gem *bo = to_amdrnpu_gem(obj);

	if (!bo->kvaddr)
		return -ENOMEM;
	iosys_map_set_vaddr(map, bo->kvaddr);
	return 0;
}

static struct sg_table *amdrnpu_gem_get_sg_table(struct drm_gem_object *obj)
{
	struct amdrnpu_gem *bo = to_amdrnpu_gem(obj);
	struct sg_table *sgt;
	int ret;

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	ret = dma_get_sgtable(bo->bank->dev, sgt, bo->kvaddr,
			      bo->dma_addr, bo->size);
	if (ret) {
		kfree(sgt);
		return ERR_PTR(ret);
	}
	return sgt;
}

static const struct drm_gem_object_funcs amdrnpu_gem_funcs = {
	.free		= amdrnpu_gem_free,
	.mmap		= amdrnpu_gem_obj_mmap,
	.vm_ops		= &amdrnpu_gem_vm_ops,
	.vmap		= amdrnpu_gem_vmap,
	.get_sg_table	= amdrnpu_gem_get_sg_table,
};

static int amdrnpu_buf_alloc(struct drm_device *ddev, struct amdrnpu_buf_alloc *args,
			     struct drm_file *file)
{
	struct amdrnpu_dev *rnpu = to_amdrnpu_dev(ddev);
	struct amdrnpu_bank *bank;
	struct amdrnpu_gem *bo;
	u32 handle;
	int ret;

	if (args->flags || args->reserved0)
		return -EINVAL;
	if (!args->size || args->size > SZ_2G)
		return -EINVAL;

	switch (args->memory_type) {
	case AMDRNPU_MEMORY_RPU:
	case AMDRNPU_MEMORY_DEVICE:
		break;
	default:
		return -EINVAL;
	}

	bank = amdrnpu_bank_memory_type(rnpu, args->memory_type);
	if (!bank)
		return -ENODEV;

	if (args->memory_type == AMDRNPU_MEMORY_RPU && !bank->has_reserved_mem) {
		AMDRNPU_ERR(rnpu,
			    "AMDRNPU_MEMORY_RPU needs a memory-region phandle on bank@0\n");
		return -ENXIO;
	}

	bo = amdrnpu_gem_alloc(rnpu, bank, args->size);
	if (IS_ERR(bo))
		return PTR_ERR(bo);

	bo->dma_dir = DMA_BIDIRECTIONAL;
	/*
	 * __GFP_NOWARN: legitimate large requests (e.g. userspace probing
	 * "give me 1 GiB on a 512 MiB carveout") fail cleanly with -ENOMEM
	 * which is already reported to the caller. __GFP_NORETRY keeps the OOM killer from
	 * thrashing for an allocation the caller can already gracefully
	 * recover from.
	 */
	bo->kvaddr = dma_alloc_noncoherent(bank->dev, bo->size,
					   &bo->dma_addr, bo->dma_dir,
					   GFP_KERNEL | __GFP_NOWARN |
					   __GFP_NORETRY);
	if (!bo->kvaddr) {
		ret = -ENOMEM;
		goto err_release;
	}
	bo->page = virt_to_page(bo->kvaddr);

	ret = drm_gem_create_mmap_offset(&bo->base);
	if (ret)
		goto err_free_dma;

	ret = drm_gem_handle_create(file, &bo->base, &handle);
	drm_gem_object_put(&bo->base);
	if (ret)
		return ret;

	args->drm_handle = handle;
	return 0;

err_free_dma:
	dma_free_noncoherent(bank->dev, bo->size, bo->kvaddr, bo->dma_addr,
			     bo->dma_dir);
	bo->kvaddr = NULL;
	bo->page = NULL;
err_release:
	drm_gem_object_put(&bo->base);
	return ret;
}

static int amdrnpu_buf_sync(struct drm_device *ddev, struct amdrnpu_buf_sync *args,
			    struct drm_file *file)
{
	struct amdrnpu_dev *rnpu = to_amdrnpu_dev(ddev);
	struct drm_gem_object *obj;
	struct amdrnpu_gem *bo;
	u64 offset = args->offset;
	u64 length = args->length;
	u64 end;

	if (args->flags) {
		AMDRNPU_ERR(rnpu, "flags %#x not supported\n",
			    args->flags);
		return -EINVAL;
	}
	if (args->direction > AMDRNPU_BUF_SYNC_FROM_DEVICE) {
		AMDRNPU_ERR(rnpu, "invalid direction %u\n",
			    args->direction);
		return -EINVAL;
	}

	obj = drm_gem_object_lookup(file, args->drm_handle);
	if (!obj) {
		AMDRNPU_ERR(rnpu, "unknown handle %u\n",
			    args->drm_handle);
		return -ENOENT;
	}

	bo = to_amdrnpu_gem(obj);
	if (obj->import_attach) {
		AMDRNPU_ERR(rnpu,
			    "handle %u is imported (use exporter sync)\n",
			    args->drm_handle);
		drm_gem_object_put(obj);
		return -EOPNOTSUPP;
	}

	if (offset > bo->size) {
		AMDRNPU_ERR(rnpu,
			    "offset %llu exceeds BO size %zu (handle %u)\n",
			    offset, bo->size, args->drm_handle);
		drm_gem_object_put(obj);
		return -EINVAL;
	}

	if (!length)
		length = bo->size - offset;
	end = offset + length;
	if (end > bo->size || end < offset) {
		AMDRNPU_ERR(rnpu,
			    "range [%llu, %llu) invalid for BO size %zu (handle %u)\n",
			    offset, end, bo->size, args->drm_handle);
		drm_gem_object_put(obj);
		return -EINVAL;
	}

	switch (args->direction) {
	case AMDRNPU_BUF_SYNC_TO_DEVICE:
		dma_sync_single_range_for_device(bo->bank->dev, bo->dma_addr,
						 offset, length, bo->dma_dir);
		break;
	case AMDRNPU_BUF_SYNC_FROM_DEVICE:
		dma_sync_single_range_for_cpu(bo->bank->dev, bo->dma_addr,
					      offset, length, bo->dma_dir);
		break;
	default:
		AMDRNPU_ERR(rnpu, "invalid direction %u\n",
			    args->direction);
		drm_gem_object_put(obj);
		return -EINVAL;
	}

	drm_gem_object_put(obj);
	return 0;
}

static long amdrnpu_drm_ioctl(struct file *filp, unsigned int cmd,
			      unsigned long arg)
{
	struct drm_file *file = filp->private_data;
	struct drm_device *ddev = file->minor->dev;
	void __user *uarg = (void __user *)arg;
	int ret;

	if (_IOC_TYPE(cmd) != AMDRNPU_IOC_MAGIC)
		return drm_ioctl(filp, cmd, arg);

	if (drm_dev_is_unplugged(ddev))
		return -ENODEV;

	switch (cmd) {
	case AMDRNPU_BUF_ALLOC: {
		struct amdrnpu_buf_alloc args;

		if (copy_from_user(&args, uarg, sizeof(args)))
			return -EFAULT;
		ret = amdrnpu_buf_alloc(ddev, &args, file);
		if (!ret && copy_to_user(uarg, &args, sizeof(args)))
			ret = -EFAULT;
		return ret;
	}
	case AMDRNPU_BUF_SYNC: {
		struct amdrnpu_buf_sync args;

		if (copy_from_user(&args, uarg, sizeof(args)))
			return -EFAULT;
		return amdrnpu_buf_sync(ddev, &args, file);
	}
	case AMDRNPU_CMD_SUBMIT:
		return amdrnpu_ioctl_cmd_submit(ddev, uarg, file);

	default:
		return -ENOTTY;
	}
}

static int amdrnpu_dumb_create(struct drm_file *file, struct drm_device *dev,
			       struct drm_mode_create_dumb *args)
{
	return -EOPNOTSUPP;
}

static const struct file_operations amdrnpu_drm_fops = {
	.fop_flags	= FOP_UNSIGNED_OFFSET,
	.owner		= THIS_MODULE,
	.open		= drm_open,
	.release	= drm_release,
	.unlocked_ioctl	= amdrnpu_drm_ioctl,
	.compat_ioctl	= amdrnpu_drm_ioctl,
	.poll		= drm_poll,
	.read		= drm_read,
	.llseek		= noop_llseek,
	.mmap		= drm_gem_mmap,
};

static void amdrnpu_drm_postclose(struct drm_device *ddev, struct drm_file *file)
{
	struct amdrnpu_dev *rnpu = to_amdrnpu_dev(ddev);

	amdrnpu_cmd_release_owner(rnpu, file);
}

const struct drm_driver amdrnpu_drm_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_RENDER | DRIVER_SYNCOBJ,
	.fops			= &amdrnpu_drm_fops,
	.postclose		= amdrnpu_drm_postclose,

	.dumb_create		= amdrnpu_dumb_create,
	.dumb_map_offset	= drm_gem_dumb_map_offset,

	.name			= AMDRNPU_DRV_NAME,
	.desc			= "AMD RNPU DRM implementation",
	.major			= 1,
	.minor			= 0,
};
