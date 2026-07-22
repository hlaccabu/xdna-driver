// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/rpmsg.h>
#include <linux/slab.h>
#include <linux/stddef.h>

#include "amdrnpu_internal.h"

static struct device_node *amdrnpu_find_remoteproc_node(struct rpmsg_device *rpdev)
{
	struct device *dev;

	for (dev = rpdev->dev.parent; dev; dev = dev->parent) {
		struct device_node *np = dev_of_node(dev);

		if (np)
			return np;
	}
	return NULL;
}

static struct platform_device *amdrnpu_find_platform_device(struct device_node *rproc_np)
{
	struct device_node *np;
	struct platform_device *pdev = NULL;
	struct platform_device *chosen = NULL;
	int dup = 0;

	if (!rproc_np)
		return NULL;

	for_each_compatible_node(np, NULL, "amd,amdrnpu") {
		struct device_node *peer;

		peer = of_parse_phandle(np, "amd,remoteproc", 0);
		if (!peer)
			continue;
		if (peer != rproc_np) {
			of_node_put(peer);
			continue;
		}
		of_node_put(peer);

		pdev = of_find_device_by_node(np);
		if (!pdev)
			continue;
		if (!platform_get_drvdata(pdev)) {
			put_device(&pdev->dev);
			continue;
		}

		dup++;
		if (chosen)
			put_device(&chosen->dev);
		chosen = pdev;
	}

	if (!chosen)
		return NULL;

	if (dup > 1)
		dev_warn_once(&chosen->dev,
			      "multiple amd,amdrnpu nodes reference rproc node %pOFn; RPMsg bound to DT-last probed instance (fix duplicate nodes)\n",
			      rproc_np);

	return chosen;
}

static int amdrnpu_rpmsg_cb(struct rpmsg_device *rpdev, void *data, int len,
			    void *priv, u32 src)
{
	struct amdrnpu_dev *rnpu = dev_get_drvdata(&rpdev->dev);

	if (!rnpu) {
		/* Channel was unbound while a callback was racing in. */
		dev_warn_ratelimited(&rpdev->dev,
				     "rpmsg msg dropped: channel not bound\n");
		return 0;
	}

	if ((size_t)len < sizeof(struct amdrnpu_wire_rsp)) {
		AMDRNPU_WARN(rnpu,
			     "rpmsg msg too small (%d bytes, need packed rsp hdr %zu), dropping\n",
			     len, sizeof(struct amdrnpu_wire_rsp));
		return 0;
	}

	amdrnpu_cmd_dispatch_response(rnpu, data, (size_t)len);
	return 0;
}

static int amdrnpu_rpmsg_probe(struct rpmsg_device *rpdev)
{
	struct device_node *rproc_np;
	struct platform_device *pdev;
	struct amdrnpu_dev *rnpu;
	struct device_link *link;

	rproc_np = amdrnpu_find_remoteproc_node(rpdev);
	if (!rproc_np) {
		dev_err(&rpdev->dev, "cannot find owning remoteproc DT node\n");
		return -ENODEV;
	}

	pdev = amdrnpu_find_platform_device(rproc_np);
	if (!pdev) {
		dev_dbg(&rpdev->dev,
			"no amdrnpu platform device for rproc %pOFn yet, deferring\n",
			rproc_np);
		return -EPROBE_DEFER;
	}

	rnpu = platform_get_drvdata(pdev);
	if (!rnpu) {
		put_device(&pdev->dev);
		return -EPROBE_DEFER;
	}

	link = device_link_add(&rpdev->dev, rnpu->dev,
			       DL_FLAG_AUTOREMOVE_CONSUMER);
	if (!link) {
		put_device(&pdev->dev);
		return -ENOMEM;
	}

	mutex_lock(&rnpu->rpmsg_lock);
	rnpu->rpdev = rpdev;
	dev_set_drvdata(&rpdev->dev, rnpu);
	mutex_unlock(&rnpu->rpmsg_lock);

	AMDRNPU_INFO(rnpu,
		     "rpmsg channel %s attached (rproc node %pOFn)\n",
		     dev_name(&rpdev->dev), rproc_np);

	put_device(&pdev->dev);
	return 0;
}

static void amdrnpu_rpmsg_remove(struct rpmsg_device *rpdev)
{
	struct amdrnpu_dev *rnpu = dev_get_drvdata(&rpdev->dev);

	if (!rnpu)
		return;

	mutex_lock(&rnpu->rpmsg_lock);
	if (rnpu->rpdev == rpdev)
		rnpu->rpdev = NULL;
	dev_set_drvdata(&rpdev->dev, NULL);
	mutex_unlock(&rnpu->rpmsg_lock);

	amdrnpu_cmd_drain_rpmsg_detach(rnpu, -ENOTCONN);

	AMDRNPU_INFO(rnpu, "rpmsg channel %s detached\n",
		     dev_name(&rpdev->dev));
}

static const struct rpmsg_device_id amdrnpu_rpmsg_id_table[] = {
	{ .name = AMDRNPU_RPMSG_NAME },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(rpmsg, amdrnpu_rpmsg_id_table);

static struct rpmsg_driver amdrnpu_rpmsg_driver = {
	.drv.name	= "amdrnpu_rpmsg",
	.id_table	= amdrnpu_rpmsg_id_table,
	.probe		= amdrnpu_rpmsg_probe,
	.callback	= amdrnpu_rpmsg_cb,
	.remove		= amdrnpu_rpmsg_remove,
};

int amdrnpu_rpmsg_register(void)
{
	return register_rpmsg_driver(&amdrnpu_rpmsg_driver);
}

void amdrnpu_rpmsg_unregister(void)
{
	unregister_rpmsg_driver(&amdrnpu_rpmsg_driver);
}

int amdrnpu_rpmsg_send_cmd(struct amdrnpu_dev *rnpu, u32 seq,
			   u32 command_op, u32 flags, const __le64 *args,
			   u32 num_args)
{
	struct rpmsg_device *rpdev;
	struct amdrnpu_wire_cmd *msg;
	size_t hdr_off = offsetof(struct amdrnpu_wire_cmd, args);
	size_t msg_len = hdr_off + (size_t)num_args * sizeof(__le64);
	int ret;

	if (msg_len > 4096)
		return -E2BIG;

	mutex_lock(&rnpu->rpmsg_lock);
	rpdev = rnpu->rpdev;
	if (!rpdev) {
		mutex_unlock(&rnpu->rpmsg_lock);
		return -ENOTCONN;
	}

	msg = kmalloc(msg_len, GFP_KERNEL);
	if (!msg) {
		mutex_unlock(&rnpu->rpmsg_lock);
		return -ENOMEM;
	}

	msg->seq         = cpu_to_le32(seq);
	msg->command_op  = cpu_to_le32(command_op);
	msg->flags       = cpu_to_le32(flags);
	msg->num_args    = cpu_to_le32(num_args);
	if (num_args)
		memcpy(msg->args, args, num_args * sizeof(__le64));

	ret = rpmsg_send(rpdev->ept, msg, msg_len);
	mutex_unlock(&rnpu->rpmsg_lock);

	kfree(msg);
	return ret;
}
