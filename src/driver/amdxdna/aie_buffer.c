// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 *
 * Table-driven probe DMA alloc/free and FW register/unregister for work and
 * async error buffers.
 */

#include "aie_buffer.h"

/**
 * aie_probe_buffers_alloc - Run @count alloc callbacks in order.
 * On failure, run free for stages that succeeded, in reverse order.
 */
int aie_probe_buffers_alloc(struct amdxdna_dev_hdl *ndev,
			    const struct amdxdna_buffer_probe_ops *ops,
			    unsigned int count)
{
	unsigned int i;
	int ret;

	for (i = 0; i < count; i++) {
		ret = ops[i].alloc(ndev);
		if (ret)
			goto unwind;
	}
	return 0;

unwind:
	while (i > 0) {
		i--;
		ops[i].free(ndev);
	}
	return ret;
}

void aie_probe_buffers_free(struct amdxdna_dev_hdl *ndev,
			    const struct amdxdna_buffer_probe_ops *ops,
			    unsigned int count)
{
	while (count > 0) {
		count--;
		ops[count].free(ndev);
	}
}

int aie_hw_buffers_register(struct amdxdna_dev_hdl *ndev,
			    const struct amdxdna_buffer_hw_ops *ops,
			    unsigned int count)
{
	unsigned int i;
	int ret;

	for (i = 0; i < count; i++) {
		ret = ops[i].register_fw(ndev);
		if (ret)
			goto unwind;
	}
	return 0;

unwind:
	while (i > 0) {
		i--;
		if (ops[i].unregister_fw)
			ops[i].unregister_fw(ndev);
	}
	return ret;
}

void aie_hw_buffers_unregister(struct amdxdna_dev_hdl *ndev,
			       const struct amdxdna_buffer_hw_ops *ops,
			       unsigned int count)
{
	while (count > 0) {
		count--;
		if (ops[count].unregister_fw)
			ops[count].unregister_fw(ndev);
	}
}
