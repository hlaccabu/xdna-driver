/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 *
 * Data-driven buffer lifecycle for AIE PCI devices (probe/remove DMA, and
 * optionally FW register/unregister around hw_start/hw_stop). Work buffer and
 * async error buffers use per-device probe_ops tables.
 *
 * Device differences are expressed as tables of function pointers and masks in
 * each generation's PCI module — not as if/else in this file (same idea as
 * PSP_SET_CMD + per-device conf.arg2_mask in aie_psp.c).
 */

#ifndef _AIE_BUFFER_H_
#define _AIE_BUFFER_H_

struct amdxdna_dev_hdl;

/**
 * One logical buffer family (e.g. DRAM work buffer, async error buffers).
 * @alloc / @free are paired for probe-order alloc and reverse-order free.
 */
struct amdxdna_buffer_probe_ops {
	int (*alloc)(struct amdxdna_dev_hdl *ndev);
	void (*free)(struct amdxdna_dev_hdl *ndev);
};

/**
 * FW-facing steps that run when management firmware is up (e.g. async
 * register after mailbox exists). Order: register[] forward on hw_start,
 * unregister[] reverse on hw_stop.
 */
struct amdxdna_buffer_hw_ops {
	int (*register_fw)(struct amdxdna_dev_hdl *ndev);
	void (*unregister_fw)(struct amdxdna_dev_hdl *ndev);
};

int aie_probe_buffers_alloc(struct amdxdna_dev_hdl *ndev,
			    const struct amdxdna_buffer_probe_ops *ops,
			    unsigned int count);

void aie_probe_buffers_free(struct amdxdna_dev_hdl *ndev,
			    const struct amdxdna_buffer_probe_ops *ops,
			    unsigned int count);

int aie_hw_buffers_register(struct amdxdna_dev_hdl *ndev,
			    const struct amdxdna_buffer_hw_ops *ops,
			    unsigned int count);

void aie_hw_buffers_unregister(struct amdxdna_dev_hdl *ndev,
			       const struct amdxdna_buffer_hw_ops *ops,
			       unsigned int count);

#endif /* _AIE_BUFFER_H_ */
