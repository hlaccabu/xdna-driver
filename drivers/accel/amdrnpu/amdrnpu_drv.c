// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#include <drm/drm_drv.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/version.h>

#include "amdrnpu_internal.h"

static int amdrnpu_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *rproc_np;
	struct amdrnpu_dev *rnpu;
	int ret;

	rproc_np = of_parse_phandle(dev->of_node, "amd,remoteproc", 0);
	if (!rproc_np) {
		dev_err(dev, "missing 'amd,remoteproc' phandle\n");
		return -EINVAL;
	}

	rnpu = devm_drm_dev_alloc(dev, &amdrnpu_drm_driver, struct amdrnpu_dev, ddev);
	if (IS_ERR(rnpu)) {
		of_node_put(rproc_np);
		return PTR_ERR(rnpu);
	}

	rnpu->linked_rproc = rproc_np;
	rnpu->dev = dev;
	mutex_init(&rnpu->rpmsg_lock);

	ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		dev_warn(dev, "64-bit DMA mask on primary device failed: %d\n", ret);

	ret = amdrnpu_cmd_init(rnpu);
	if (ret) {
		of_node_put(rnpu->linked_rproc);
		rnpu->linked_rproc = NULL;
		return ret;
	}

	platform_set_drvdata(pdev, rnpu);

	ret = devm_of_platform_populate(dev);
	if (ret) {
		dev_err(dev, "devm_of_platform_populate failed (%d)\n", ret);
		goto err_cmd_fini;
	}

	ret = amdrnpu_bank_check_all_present(rnpu);
	if (ret)
		goto err_cmd_fini;

	ret = drm_dev_register(&rnpu->ddev, 0);
	if (ret)
		goto err_cmd_fini;

	dev_info(dev, "amdrnpu ready\n");
	return 0;

err_cmd_fini:
	platform_set_drvdata(pdev, NULL);
	amdrnpu_cmd_fini(rnpu);
	if (rnpu->linked_rproc) {
		of_node_put(rnpu->linked_rproc);
		rnpu->linked_rproc = NULL;
	}
	return ret;
}

static void amdrnpu_platform_remove(struct platform_device *pdev)
{
	struct amdrnpu_dev *rnpu = platform_get_drvdata(pdev);

	drm_dev_unregister(&rnpu->ddev);
	amdrnpu_cmd_fini(rnpu);
	of_node_put(rnpu->linked_rproc);
	rnpu->linked_rproc = NULL;
}

static const struct of_device_id amdrnpu_of_match[] = {
	{ .compatible = "amd,amdrnpu" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, amdrnpu_of_match);

static struct platform_driver amdrnpu_platform_driver = {
	.probe	= amdrnpu_platform_probe,
	.remove = amdrnpu_platform_remove,
	.driver	= {
		.name		= AMDRNPU_DRV_NAME,
		.of_match_table	= amdrnpu_of_match,
	},
};

static int __init amdrnpu_module_init(void)
{
	int ret;

	ret = amdrnpu_bank_driver_register();
	if (ret)
		return ret;

	ret = platform_driver_register(&amdrnpu_platform_driver);
	if (ret) {
		amdrnpu_bank_driver_unregister();
		return ret;
	}

	ret = amdrnpu_rpmsg_register();
	if (ret) {
		platform_driver_unregister(&amdrnpu_platform_driver);
		amdrnpu_bank_driver_unregister();
		return ret;
	}
	return 0;
}

static void __exit amdrnpu_module_exit(void)
{
	amdrnpu_rpmsg_unregister();
	platform_driver_unregister(&amdrnpu_platform_driver);
	amdrnpu_bank_driver_unregister();
}

module_init(amdrnpu_module_init);
module_exit(amdrnpu_module_exit);

MODULE_AUTHOR("AMD");
MODULE_DESCRIPTION("AMD amdrnpu RPU management DRM/RPMsg driver");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("0.1.0");
/*
 * dma_buf_* helpers were moved into the DMA_BUF symbol namespace in
 * Linux 6.6; importing it here keeps modpost happy on 6.12 and newer.
 * The namespace argument became a string literal in Linux 6.13.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
MODULE_IMPORT_NS("DMA_BUF");
#else
MODULE_IMPORT_NS(DMA_BUF);
#endif
