/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#ifndef _AMDRNPU_INTERNAL_H_
#define _AMDRNPU_INTERNAL_H_

#include "drm/amdrnpu_accel.h"
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_print.h>
#include <drm/drm_syncobj.h>
#include <linux/atomic.h>
#include <linux/dma-direction.h>
#include <linux/dma-fence.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/rpmsg.h>
#include <linux/spinlock.h>
#include <linux/stddef.h>
#include <linux/types.h>
#include <linux/xarray.h>

#include "amdrnpu_cmd.h"

#define AMDRNPU_DRV_NAME	AMDRNPU_DRIVER_NAME
#define AMDRNPU_MAX_ARGS	64
#define AMDRNPU_RPMSG_NAME	"amdrnpu"

#define AMDRNPU_INFO(rnpu, fmt, args...)	drm_info(&(rnpu)->ddev, fmt, ##args)
#define AMDRNPU_WARN(rnpu, fmt, args...)	drm_warn(&(rnpu)->ddev, "%s: "fmt, __func__, ##args)
#define AMDRNPU_ERR(rnpu, fmt, args...)	drm_err(&(rnpu)->ddev, "%s: "fmt, __func__, ##args)
#define AMDRNPU_DBG(rnpu, fmt, args...)	drm_dbg(&(rnpu)->ddev, fmt, ##args)

#define to_amdrnpu_dev(drm_dev) \
	((struct amdrnpu_dev *)container_of(drm_dev, struct amdrnpu_dev, ddev))

enum amdrnpu_bank_index {
	AMDRNPU_BANK_RPU	= 0,
	AMDRNPU_BANK_DEV	= 1,
	AMDRNPU_NUM_BANKS,
};

struct amdrnpu_dev;
struct amdrnpu_cmd;

#define AMDRNPU_SUBMIT_HDR_SZ	offsetof(struct amdrnpu_cmd_submit, args)
#define AMDRNPU_CMD_SEQ_NONE	((u32)~0U)

struct amdrnpu_bank {
	struct device		*dev;
	struct platform_device	*pdev;
	struct device_node	*np;
	u32			region_id;
	const char		*label;
	bool			has_reserved_mem;
};

struct amdrnpu_dev {
	struct drm_device	ddev;
	struct device		*dev;
	struct device_node	*linked_rproc;

	struct amdrnpu_bank	banks[AMDRNPU_NUM_BANKS];

	struct rpmsg_device	*rpdev;
	struct mutex		rpmsg_lock;	/* rpmsg send path */

	struct mutex		cmd_lock;	/* cmd_xa alloc/erase */
	struct xarray		cmd_xa;		/* in-flight cmds; id via xa_alloc_cyclic */
	u32			cmd_xa_next;	/* xa_alloc_cyclic cursor */
	atomic_t		fence_seqno;
	u64			fence_context;
};

struct amdrnpu_gem {
	struct drm_gem_object	base;
	struct amdrnpu_bank	*bank;
	void			*kvaddr;
	dma_addr_t		dma_addr;
	size_t			size;
	enum dma_data_direction	dma_dir;
	struct page		*page;
	struct sg_table		*sgt;
};

static inline struct amdrnpu_gem *to_amdrnpu_gem(struct drm_gem_object *obj)
{
	return container_of(obj, struct amdrnpu_gem, base);
}

enum amdrnpu_cmd_state {
	AMDRNPU_CMD_SUBMITTED,
	AMDRNPU_CMD_TERMINATED,
};

struct amdrnpu_cmd {
	struct amdrnpu_dev		*rnpu;
	struct drm_file		*owner;
	u32			seq;		/* wire cmd/rsp seq */
	bool			in_cmd_xa;
	u32			command_op;

	struct mutex		state_lock;	/* state, term_status, bo_refs */
	enum amdrnpu_cmd_state	state;
	int			term_status;

	struct drm_syncobj	*syncobj;
	spinlock_t		fence_lock;	/* pending_fence */
	struct dma_fence	*pending_fence;

	struct drm_gem_object	**bo_refs;
	u32			num_bo_refs;

	struct kref		ref;
};

/* amdrnpu_bank.c */
int  amdrnpu_bank_driver_register(void);
void amdrnpu_bank_driver_unregister(void);
int  amdrnpu_bank_check_all_present(struct amdrnpu_dev *rnpu);
struct amdrnpu_bank *amdrnpu_bank_by_region(struct amdrnpu_dev *rnpu, u32 region_id);
struct amdrnpu_bank *amdrnpu_bank_memory_type(struct amdrnpu_dev *rnpu, u32 mem_type);

/* amdrnpu_drm.c */
extern const struct drm_driver amdrnpu_drm_driver;

/* amdrnpu_cmd.c */
long amdrnpu_ioctl_cmd_submit(struct drm_device *ddev, void __user *uarg, struct drm_file *file);
void amdrnpu_cmd_release_owner(struct amdrnpu_dev *rnpu, struct drm_file *file);
void amdrnpu_cmd_dispatch_response(struct amdrnpu_dev *rnpu,
				   const void *payload, size_t len);
int  amdrnpu_cmd_init(struct amdrnpu_dev *rnpu);
void amdrnpu_cmd_fini(struct amdrnpu_dev *rnpu);
void amdrnpu_cmd_drain_rpmsg_detach(struct amdrnpu_dev *rnpu, int err);

/* amdrnpu_rpmsg.c */
int  amdrnpu_rpmsg_register(void);
void amdrnpu_rpmsg_unregister(void);
int  amdrnpu_rpmsg_send_cmd(struct amdrnpu_dev *rnpu, u32 seq,
			    u32 command_op, u32 flags, const __le64 *args,
			    u32 num_args);

#endif /* _AMDRNPU_INTERNAL_H_ */
