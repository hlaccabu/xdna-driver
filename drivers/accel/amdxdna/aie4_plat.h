/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 *
 * aie4-over-platform device ops.
 *
 * The aie4 message protocol runs over a platform (non-PCI) transport on
 * aie2ps-class SoC parts.  It reuses the same struct amdxdna_dev_hdl as the PCI
 * path, so the shared aie4 command/query/error flows in aie4.c/aie4_ctx.c work
 * unchanged; only hardware bring-up differs.  This file provides the aie4
 * transport hooks (doorbell/cert notification) and the platform device
 * lifecycle ops (aie4_plat_ops).
 */

#ifndef _AIE4_PLAT_H_
#define _AIE4_PLAT_H_

struct amdxdna_dev;
struct amdxdna_dev_ops;

extern const struct amdxdna_dev_ops aie4_plat_ops;

/*
 * Platform-only runtime PM bring-up. The platform bus does not enable RPM
 * the way PCI does; these replace amdxdna_pm_init/fini on the npu12 path so
 * shared PCI PM stays unchanged. Call allow_idle only after ops->init has
 * dropped dev_lock (end of amdxdna_plat_probe).
 */
void aie4_plat_pm_init(struct amdxdna_dev *xdna);
void aie4_plat_pm_allow_idle(struct amdxdna_dev *xdna);
void aie4_plat_pm_fini(struct amdxdna_dev *xdna);

#endif /* _AIE4_PLAT_H_ */
