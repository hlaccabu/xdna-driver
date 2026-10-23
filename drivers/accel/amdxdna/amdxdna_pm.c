// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025, Advanced Micro Devices, Inc.
 */

#include "drm/amdxdna_accel.h"
#include <drm/drm_drv.h>
#include <linux/pm_runtime.h>

#include "amdxdna_dpt.h"
#include "amdxdna_pm.h"

#define AMDXDNA_AUTOSUSPEND_DELAY	5000 /* milliseconds */

int amdxdna_pm_suspend(struct device *dev)
{
	struct amdxdna_dev *xdna = to_xdna_dev(dev_get_drvdata(dev));
	int ret = -EOPNOTSUPP;

	guard(mutex)(&xdna->dev_lock);
	if (xdna->dev_info->ops->suspend)
		ret = xdna->dev_info->ops->suspend(xdna);

	/*
	 * Drain and pause firmware DPT (log/trace) after the device has
	 * quiesced. Common to every generation/config; a safe no-op when no
	 * DPT channel is active. A drain/pause failure must not change the
	 * device suspend result (logging/tracing is auxiliary), but surface
	 * it so it is visible rather than silently swallowed.
	 */
	if (!ret) {
		int dpt_ret = amdxdna_dpt_suspend(xdna);

		if (dpt_ret)
			XDNA_WARN(xdna, "DPT drain/pause on suspend failed: %d", dpt_ret);
	}

	XDNA_DBG(xdna, "Suspend done ret %d", ret);
	return ret;
}

int amdxdna_pm_resume(struct device *dev)
{
	struct amdxdna_dev *xdna = to_xdna_dev(dev_get_drvdata(dev));
	int ret = -EOPNOTSUPP;

	guard(mutex)(&xdna->dev_lock);
	if (xdna->dev_info->ops->resume)
		ret = xdna->dev_info->ops->resume(xdna);

	/*
	 * Re-arm firmware DPT only after a successful device resume. A DPT
	 * re-arm failure must not fail the device resume (logging/tracing is
	 * auxiliary), but surface it so it is visible in the resume logs
	 * rather than silently leaving DPT paused.
	 */
	if (!ret) {
		int dpt_ret = amdxdna_dpt_resume(xdna);

		if (dpt_ret)
			XDNA_WARN(xdna, "DPT re-arm on resume failed: %d", dpt_ret);
	}

	XDNA_DBG(xdna, "Resume done ret %d", ret);
	return ret;
}

int amdxdna_pm_runtime_suspend(struct device *dev)
{
	struct amdxdna_dev *xdna = to_xdna_dev(dev_get_drvdata(dev));
	int ret = -EOPNOTSUPP;

	guard(mutex)(&xdna->dev_lock);
	if (xdna->dev_info->ops->runtime_suspend)
		ret = xdna->dev_info->ops->runtime_suspend(xdna);

	if (!ret) {
		int dpt_ret = amdxdna_dpt_suspend(xdna);

		if (dpt_ret)
			XDNA_WARN(xdna, "DPT drain/pause on suspend failed: %d", dpt_ret);
	}

	XDNA_DBG(xdna, "Runtime suspend done ret %d", ret);
	return ret;
}

int amdxdna_pm_runtime_resume(struct device *dev)
{
	struct amdxdna_dev *xdna = to_xdna_dev(dev_get_drvdata(dev));
	int ret = -EOPNOTSUPP;

	guard(mutex)(&xdna->dev_lock);
	if (xdna->dev_info->ops->runtime_resume)
		ret = xdna->dev_info->ops->runtime_resume(xdna);

	if (!ret) {
		int dpt_ret = amdxdna_dpt_resume(xdna);

		if (dpt_ret)
			XDNA_WARN(xdna, "DPT re-arm on resume failed: %d", dpt_ret);
	}

	XDNA_DBG(xdna, "Runtime resume done ret %d", ret);
	return ret;
}

int amdxdna_pm_resume_get(struct amdxdna_dev *xdna)
{
	struct device *dev = xdna->ddev.dev;
	int ret;

	if (!pm_runtime_enabled(dev))
		return 0;

	ret = pm_runtime_resume_and_get(dev);
	if (ret) {
		XDNA_ERR(xdna, "Resume failed: %d", ret);
		pm_runtime_set_suspended(dev);
	}

	return ret;
}

void amdxdna_pm_suspend_put(struct amdxdna_dev *xdna)
{
	struct device *dev = xdna->ddev.dev;

	if (!pm_runtime_enabled(dev))
		return;

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
}

void amdxdna_pm_init(struct amdxdna_dev *xdna)
{
	struct device *dev = xdna->ddev.dev;
	bool need_enable = !pm_runtime_enabled(dev);

	/*
	 * PCI core enables RPM before probe and holds a usage ref from
	 * local_pci_probe()'s get_sync (dropped by the deferred put in
	 * amdxdna_pm_autosuspend()). Platform does not enable RPM, so
	 * without pm_runtime_enable() sysfs reports unsupported and
	 * autosuspend never fires.
	 *
	 * ops->init() runs under xdna->dev_lock, and runtime_suspend takes
	 * that same lock. set_autosuspend_delay / allow / put can all enter
	 * rpm_idle -> runtime_suspend from this context (seen hanging
	 * insmod in D-state on amdxdna_pm_runtime_suspend). On platform,
	 * take a temporary usage ref across setup so idle cannot run; on
	 * PCI the core's probe ref already covers that. Do not get on PCI
	 * or the deferred put would only cancel our get and leave the
	 * probe ref held forever (no autosuspend).
	 */
	if (need_enable)
		pm_runtime_get_noresume(dev);

	pm_runtime_set_active(dev);
	if (need_enable)
		pm_runtime_enable(dev);
	xdna->rpm_enabled_by_drv = need_enable;

	pm_runtime_set_autosuspend_delay(dev, AMDXDNA_AUTOSUSPEND_DELAY);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_allow(dev);
	pm_runtime_mark_last_busy(dev);
}

void amdxdna_pm_autosuspend(struct amdxdna_dev *xdna)
{
	struct device *dev = xdna->ddev.dev;

	if (!pm_runtime_enabled(dev))
		return;

	/*
	 * Drop the probe-time usage ref: PCI local_pci_probe() get_sync, or
	 * the get_noresume taken in amdxdna_pm_init() for platform.
	 */
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
}

void amdxdna_pm_fini(struct amdxdna_dev *xdna)
{
	struct device *dev = xdna->ddev.dev;

	pm_runtime_get_noresume(dev);
	pm_runtime_forbid(dev);
	pm_runtime_dont_use_autosuspend(dev);
	if (xdna->rpm_enabled_by_drv)
		pm_runtime_disable(dev);
}
