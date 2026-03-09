// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#include <linux/iommu.h>
#include <linux/iova.h>
#ifdef HAVE_iommu_paging_domain_alloc_flags
/* IOMMU_HWPT_ALLOC_PASID is defined in uiommufd.h */
#include <uapi/linux/iommufd.h>
#endif
/* used GENMASK() to define iommu iova upper limit when geometry not available */
#include <linux/bits.h>
#include <linux/sizes.h>

#include "amdxdna_gem.h"
#include "amdxdna_pci_drv.h"

static bool force_iova;
module_param(force_iova, bool, 0600);
MODULE_PARM_DESC(force_iova, "Force use IOVA (Default false)");

static struct iova *amdxdna_iommu_alloc_iova(struct amdxdna_dev *xdna,
					     size_t size,
					     dma_addr_t *dma_addr,
					     bool prefer_low)
{
	unsigned long shift, end, limit_pfn, start_pfn;
	struct iova *iova;

#ifdef HAVE_iommu_paging_domain_alloc_flags
	end = xdna->domain->geometry.aperture_end;
#else
	end = GENMASK(46, 0);
#endif
	shift = iova_shift(&xdna->iovad);
	size = iova_align(&xdna->iovad, size);
	limit_pfn = end >> shift;
	if (prefer_low) {
		/* Allocator is top-down; cap limit so we get a low IOVA (e.g. work buffer). */
		start_pfn = xdna->iovad.start_pfn;
		limit_pfn = min(limit_pfn, start_pfn + (size >> shift));
	}

	iova = alloc_iova(&xdna->iovad, size >> shift, limit_pfn, true);
	if (!iova)
		return ERR_PTR(-ENOMEM);

	*dma_addr = iova_dma_addr(&xdna->iovad, iova);

	return iova;
}

int amdxdna_iommu_map_bo(struct amdxdna_dev *xdna, struct amdxdna_gem_obj *abo)
{
	struct sg_table *sgt;
	dma_addr_t dma_addr;
	struct iova *iova;
	ssize_t size;

	if (abo->mem.dma_addr != AMDXDNA_INVALID_ADDR)
		return 0;

	sgt = drm_gem_shmem_get_pages_sgt(&abo->base);
	if (IS_ERR(sgt)) {
		XDNA_ERR(xdna, "Get sgt failed, ret %ld", PTR_ERR(sgt));
		return PTR_ERR(sgt);
	}

	iova = amdxdna_iommu_alloc_iova(xdna, abo->mem.size, &dma_addr, false);
	if (IS_ERR(iova)) {
		XDNA_ERR(xdna, "Alloc iova failed, ret %ld", PTR_ERR(iova));
		return PTR_ERR(iova);
	}

	size = iommu_map_sgtable(xdna->domain, dma_addr, sgt,
				 IOMMU_READ | IOMMU_WRITE);
	if (size < 0) {
		XDNA_ERR(xdna, "iommu_map_sgtable failed: %zd", size);
		__free_iova(&xdna->iovad, iova);
		return size;
	}
	if (size < abo->mem.size) {
		XDNA_ERR(xdna, "iommu_map_sgtable mapped incomplete size: 0x%lx, expected: 0x%lx",
			 size, abo->mem.size);
		__free_iova(&xdna->iovad, iova);
		return -ENXIO;
	}

	abo->mem.dma_addr = dma_addr;

	return 0;
}

void amdxdna_iommu_unmap_bo(struct amdxdna_dev *xdna, struct amdxdna_gem_obj *abo)
{
	size_t size;

	if (abo->mem.dma_addr == AMDXDNA_INVALID_ADDR)
		return;

	size = iova_align(&xdna->iovad, abo->mem.size);
	iommu_unmap(xdna->domain, abo->mem.dma_addr, size);
	free_iova(&xdna->iovad, iova_pfn(&xdna->iovad, abo->mem.dma_addr));
	abo->mem.dma_addr = AMDXDNA_INVALID_ADDR;
}

void *amdxdna_iommu_alloc(struct amdxdna_dev *xdna, size_t size, dma_addr_t *dma_addr)
{
	return amdxdna_iommu_alloc_prefer(xdna, size, dma_addr, false);
}

void *amdxdna_iommu_alloc_prefer(struct amdxdna_dev *xdna, size_t size, dma_addr_t *dma_addr,
				 bool prefer_low)
{
	struct iova *iova;
	void *cpu_addr;
	size_t aligned_size;
	int ret;

	if (!xdna->domain)
		return ERR_PTR(-EINVAL);

	aligned_size = iova_align(&xdna->iovad, size);

	/* Use reserved low IOVA for DRAM work buffer if size matches. */
	if (prefer_low && xdna->reserved_work_iova && xdna->reserved_work_size == aligned_size) {
		*dma_addr = xdna->reserved_work_iova;
		xdna->reserved_work_iova = 0;
		xdna->reserved_work_size = 0;
	} else {
		iova = amdxdna_iommu_alloc_iova(xdna, size, dma_addr, prefer_low);
		if (IS_ERR(iova)) {
			XDNA_ERR(xdna, "Alloc iova failed, ret %ld", PTR_ERR(iova));
			return iova;
		}
	}

	cpu_addr = (void *)__get_free_pages(GFP_KERNEL, get_order(size));
	if (!cpu_addr) {
		ret = -ENOMEM;
		if (prefer_low && !xdna->reserved_work_iova) {
			/* We consumed the reserved IOVA; put it back by re-allocating. */
			xdna->reserved_work_iova = *dma_addr;
			xdna->reserved_work_size = aligned_size;
		}
		goto free_iova;
	}

	ret = iommu_map(xdna->domain, *dma_addr, virt_to_phys(cpu_addr),
			aligned_size, IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
	if (ret) {
		if (prefer_low && !xdna->reserved_work_iova) {
			xdna->reserved_work_iova = *dma_addr;
			xdna->reserved_work_size = aligned_size;
		}
		goto free_iova;
	}

	return cpu_addr;

free_iova:
	if (prefer_low && xdna->reserved_work_iova) {
		/* Reserved path: no iova to free here, already in tree. */
		free_pages((unsigned long)cpu_addr, get_order(size));
		return ERR_PTR(ret);
	}
	free_iova(&xdna->iovad, iova_pfn(&xdna->iovad, *dma_addr));
	free_pages((unsigned long)cpu_addr, get_order(size));
	return ERR_PTR(ret);
}

void amdxdna_iommu_free(struct amdxdna_dev *xdna, size_t size,
			void *cpu_addr, dma_addr_t dma_addr)
{
	iommu_unmap(xdna->domain, dma_addr, iova_align(&xdna->iovad, size));
	free_iova(&xdna->iovad, iova_pfn(&xdna->iovad, dma_addr));
	free_pages((unsigned long)cpu_addr, get_order(size));
}

int amdxdna_iommu_init(struct amdxdna_dev *xdna)
{
	unsigned long order, shift, npages, limit_pfn, start_pfn;
	struct iova *iova;
	int ret;

	if (!force_iova)
		return 0;

	xdna->reserved_work_iova = 0;
	xdna->reserved_work_size = 0;

	xdna->group = iommu_group_get(xdna->ddev.dev);
	if (!xdna->group) {
		XDNA_ERR(xdna, "Failed getting iommu group");
		return 0;
	}

#ifdef HAVE_iommu_paging_domain_alloc_flags
	/* Force IOVA: single domain for IOVA only (no PASID). Use 0, not IOMMU_HWPT_ALLOC_PASID. */
	xdna->domain = iommu_paging_domain_alloc_flags(xdna->ddev.dev, 0);
	if (IS_ERR(xdna->domain) && PTR_ERR(xdna->domain) == -EOPNOTSUPP) {
		/* IOMMU doesn't support paging domain with flags; try alloc without flags. */
#ifdef HAVE_iommu_paging_domain_alloc
		xdna->domain = iommu_paging_domain_alloc(xdna->ddev.dev);
#endif
	}
#elif defined(HAVE_iommu_paging_domain_alloc)
	xdna->domain = iommu_paging_domain_alloc(xdna->ddev.dev);
#else
	xdna->domain = ERR_PTR(-EOPNOTSUPP);
#endif
	if (IS_ERR(xdna->domain)) {
		ret = PTR_ERR(xdna->domain);
		XDNA_ERR(xdna, "Failed to alloc iommu domain, ret %d", ret);
		goto put_group;
	}

	ret = iova_cache_get();
	if (ret)
		goto free_domain;

	order = __ffs(xdna->domain->pgsize_bitmap);
	/* Start IOVA at 4MB so first allocation (e.g. DRAM work buffer) is not at 0. */
	init_iova_domain(&xdna->iovad, 1UL << order, SZ_4M >> order);

	ret = iommu_attach_group(xdna->domain, xdna->group);
	if (ret)
		goto put_iova;

	/* Reserve low IOVA for DRAM work buffer so it always gets a firmware-accepted address. */
	shift = iova_shift(&xdna->iovad);
	xdna->reserved_work_size = iova_align(&xdna->iovad, SZ_4M);
	npages = xdna->reserved_work_size >> shift;
	start_pfn = xdna->iovad.start_pfn;
	limit_pfn = start_pfn + npages;
	iova = alloc_iova(&xdna->iovad, npages, limit_pfn, true);
	if (iova) {
		xdna->reserved_work_iova = iova_dma_addr(&xdna->iovad, iova);
		XDNA_DBG(xdna, "Reserved work buffer IOVA 0x%llx size 0x%zx",
			 (u64)xdna->reserved_work_iova, xdna->reserved_work_size);
	}

	return 0;

put_iova:
	put_iova_domain(&xdna->iovad);
	iova_cache_put();
free_domain:
	iommu_domain_free(xdna->domain);
	xdna->domain = NULL;
put_group:
	iommu_group_put(xdna->group);

	return ret;
}

void amdxdna_iommu_fini(struct amdxdna_dev *xdna)
{
	if (!xdna->domain)
		return;

	iommu_detach_group(xdna->domain, xdna->group);
	put_iova_domain(&xdna->iovad);
	iova_cache_put();

	if (!IS_ERR_OR_NULL(xdna->domain))
		iommu_domain_free(xdna->domain);
	if (xdna->group)
		iommu_group_put(xdna->group);
}
