/*
 * Samsung Exynos5 SoC series FIMC-IS driver
 *
 * exynos5 fimc-is video functions
 *
 * Copyright (c) 2011 Samsung Electronics Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <video/videonode.h>
#include <media/exynos_mc.h>
#include <asm/cacheflush.h>
#include <asm/pgtable.h>
#include <linux/firmware.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/videodev2.h>
#include <linux/videodev2_exynos_camera.h>
#include <linux/v4l2-mediabus.h>
#include <linux/bug.h>
#include <linux/i2c.h>

#include "fimc-is-core.h"
#include "fimc-is-cmd.h"
#include "fimc-is-regs.h"
#include "fimc-is-err.h"
#include "fimc-is-hw.h"
#include "fimc-is-video.h"
#include "fimc-is-dt.h"
#include "fimc-is-debug.h"

#include "fimc-is-device-sensor.h"
#include "fimc-is-interface-wrap.h"
#ifdef CONFIG_OIS_USE
#include "fimc-is-device-ois.h"
#endif

struct pm_qos_request exynos_sensor_qos_int;
struct pm_qos_request exynos_sensor_qos_cam;
struct pm_qos_request exynos_sensor_qos_mem;

int fimc_is_sensor_runtime_suspend(struct device *dev);
int fimc_is_sensor_runtime_resume(struct device *dev);

static int fimc_is_sensor_back_stop(void *qdevice,
	struct fimc_is_queue *queue);

int fimc_is_search_sensor_module(struct fimc_is_device_sensor *device,
	u32 sensor_id, struct fimc_is_module_enum **module)
{
	int ret = 0;
	u32 mindex, mmax;
	struct fimc_is_module_enum *module_enum;
	struct fimc_is_resourcemgr *resourcemgr;

	resourcemgr = device->resourcemgr;
	module_enum = device->module_enum;
	*module = NULL;

	if (resourcemgr == NULL) {
		mwarn("resourcemgr is NULL", device);
		return -EINVAL;
	}

	mmax = atomic_read(&resourcemgr->rsccount_module);
	for (mindex = 0; mindex < mmax; mindex++) {
		if (module_enum[mindex].sensor_id == sensor_id) {
			*module = &module_enum[mindex];
			break;
		}
	}

	if (mindex >= mmax) {
		merr("module(%d) is not found", device, sensor_id);
		ret = -EINVAL;
	}

	return ret;
}

int fimc_is_sensor_g_module(struct fimc_is_device_sensor *device,
	struct fimc_is_module_enum **module)
{
	int ret = 0;

	BUG_ON(!device);

	if (!device->subdev_module) {
		merr("sub module is NULL", device);
		ret = -EINVAL;
		goto p_err;
	}

	*module = (struct fimc_is_module_enum *)v4l2_get_subdevdata(device->subdev_module);
	if (!*module) {
		merr("module is NULL", device);
		ret = -EINVAL;
		goto p_err;
	}

	if (!(*module)->pdata) {
		merr("module->pdata is NULL", device);
		ret = -EINVAL;
		goto p_err;
	}

p_err:
	return ret;
}

#ifndef ENABLE_IS_CORE
#ifdef CONFIG_OIS_USE
int fimc_is_sensor_g_ois_mode(struct fimc_is_device_ischain *device,
		struct fimc_is_queue *queue, u32 index)
{
	int ret = 0;
	struct fimc_is_core *core;
	struct fimc_is_device_sensor *sensor;
	struct fimc_is_groupmgr *groupmgr;
	struct fimc_is_framemgr *framemgr;
	struct fimc_is_frame *frame;

	groupmgr = device->groupmgr;
	core = container_of(groupmgr, struct fimc_is_core, groupmgr);
	sensor = device->sensor;

	BUG_ON(!sensor);

	framemgr = &queue->framemgr;
	BUG_ON(!framemgr);

	frame = &framemgr->frames[index];
	if (!frame) {
		err("frame is null\n");
		ret = -EINVAL;
	}

	sensor->ois_mode = frame->shot->ctl.lens.opticalStabilizationMode;

	return ret;
}
#endif
#endif

int fimc_is_sensor_deinit_module(struct fimc_is_module_enum *module)
{
	int ret = 0;
	int i;
	struct fimc_is_core *core;
	struct fimc_is_device_sensor *sensor;
	struct fimc_is_sensor_cfg cfg;

	BUG_ON(!module->subdev);

	sensor = v4l2_get_subdev_hostdata(module->subdev);

	core = sensor->private_data;
	for (i = 0; i < FIMC_IS_SENSOR_COUNT; i++) {
		if (!test_bit(FIMC_IS_SENSOR_OPEN, &core->sensor[i].state))
			continue;

		if (!test_bit(FIMC_IS_SENSOR_S_INPUT, &core->sensor[i].state))
			continue;

		if (core->sensor[i].position == module->position) {
			sensor = &core->sensor[i];
			break;
		}
	}

	if (i >= FIMC_IS_SENSOR_COUNT) {
		warn("There is no valid sensor");
		goto p_err;
	}

	if (!sensor->ischain) {
		warn("sensor is not initialized");
		goto p_err;
	}

	if (!test_bit(FIMC_IS_ISCHAIN_INIT, &sensor->ischain->state)) {
		warn("ischain is not initialized");
		goto p_err;
	}

	minfo("Deinit sensor module(%d)\n", sensor, module->sensor_id);

	cfg.mode = SENSOR_MODE_DEINIT;
	ret = fimc_is_itf_sensor_mode_wrap(sensor->ischain, &cfg);
	if (ret)
		goto p_err;

p_err:
	return ret;
}

static int fimc_is_sensor_g_device(struct platform_device *pdev,
	struct fimc_is_device_sensor **device)
{
	int ret = 0;

	BUG_ON(!pdev);

	*device = (struct fimc_is_device_sensor *)platform_get_drvdata(pdev);
	if (!*device) {
		err("device is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	if (!(*device)->pdata) {
		merr("device->pdata is NULL", (*device));
		ret = -EINVAL;
		goto p_err;
	}

p_err:
	return ret;
}

struct fimc_is_sensor_cfg * fimc_is_sensor_g_mode(struct fimc_is_device_sensor *device)
{
	struct fimc_is_sensor_cfg *select = NULL;
	u32 approximate_value = UINT_MAX;
	struct fimc_is_sensor_cfg *cfg_table;
	u32 cfgs, i;
	u32 width, height, framerate;
	struct fimc_is_module_enum *module;
	int deviation;

	BUG_ON(!device);

	module = (struct fimc_is_module_enum *)v4l2_get_subdevdata(device->subdev_module);
	if (!module) {
		merr("module is NULL", device);
		goto p_err;
	}

	if (test_bit(FIMC_IS_SENSOR_S_CONFIG, &device->state) && device->cfg) {
		select = device->cfg;
		goto p_err;
	}

	cfg_table = module->cfg;
	cfgs = module->cfgs;
	width = device->image.window.width;
	height = device->image.window.height;
	framerate = device->image.framerate;

	/* find sensor mode by w/h and fps range */
	for (i = 0; i < cfgs; i++) {
		if ((cfg_table[i].width == width) && (cfg_table[i].height == height)) {
			deviation = cfg_table[i].framerate - framerate;
			if (deviation == 0) {
				/* You don't need to find another sensor mode */
				select = &cfg_table[i];
				break;
			} else if ((deviation > 0) && approximate_value > abs(deviation)) {
				/* try to find framerate smaller than previous */
				approximate_value = abs(deviation);
				select = &cfg_table[i];
			}
		}
	}

	if (!select) {
		merr("sensor mode(%dx%d@%dfps) is not found", device, width, height, framerate);
		goto p_err;
	}

	minfo("sensor mode(%dx%d@%d) = %d (lane:%d)\n", device,
		select->width,
		select->height,
		select->framerate,
		select->mode,
		(select->lanes + 1));


p_err:
	return select;
}

int fimc_is_sensor_mclk_on(struct fimc_is_device_sensor *device, u32 scenario, u32 channel)
{
	int ret = 0;
	struct exynos_platform_fimc_is_sensor *pdata;

	BUG_ON(!device);
	BUG_ON(!device->pdev);
	BUG_ON(!device->pdata);

	pdata = device->pdata;

	if (scenario == SENSOR_SCENARIO_STANDBY) {
		minfo("%s: skip standby mode", device, __func__);
		goto p_err;
	}

	if (test_and_set_bit(FIMC_IS_SENSOR_MCLK_ON, &device->state)) {
		minfo("%s : already clk on", device, __func__);
		goto p_err;
	}

	if (!pdata->mclk_on) {
		merr("mclk_on is NULL", device);
		ret = -EINVAL;
		clear_bit(FIMC_IS_SENSOR_MCLK_ON, &device->state);
		goto p_err;
	}

	ret = pdata->mclk_on(&device->pdev->dev, scenario, channel);
	if (ret) {
		merr("mclk_on is fail(%d)", device, ret);
		clear_bit(FIMC_IS_SENSOR_MCLK_ON, &device->state);
		goto p_err;
	}

p_err:
	return ret;
}

int fimc_is_sensor_mclk_off(struct fimc_is_device_sensor *device, u32 scenario, u32 channel)
{
	int ret = 0;
	struct exynos_platform_fimc_is_sensor *pdata;

	BUG_ON(!device);
	BUG_ON(!device->pdev);
	BUG_ON(!device->pdata);

	pdata = device->pdata;

	if (!test_and_clear_bit(FIMC_IS_SENSOR_MCLK_ON, &device->state)) {
		minfo("%s : already clk off", device, __func__);
		goto p_err;
	}

	if (!pdata->mclk_off) {
		merr("mclk_off is NULL", device);
		ret = -EINVAL;
		set_bit(FIMC_IS_SENSOR_MCLK_ON, &device->state);
		goto p_err;
	}

	ret = pdata->mclk_off(&device->pdev->dev, scenario, channel);
	if (ret) {
		merr("mclk_off is fail(%d)", device, ret);
		set_bit(FIMC_IS_SENSOR_MCLK_ON, &device->state);
		goto p_err;
	}

p_err:
	return ret;
}

static int fimc_is_sensor_iclk_on(struct fimc_is_device_sensor *device)
{
	int ret = 0;
	struct exynos_platform_fimc_is_sensor *pdata;
	struct fimc_is_core *core;

	BUG_ON(!device);
	BUG_ON(!device->pdev);
	BUG_ON(!device->pdata);
	BUG_ON(!device->private_data);

	core = device->private_data;
	pdata = device->pdata;

	if (test_bit(FIMC_IS_SENSOR_ICLK_ON, &device->state)) {
		merr("%s : already clk on", device, __func__);
		goto p_err;
	}

	if (!pdata->iclk_cfg) {
		merr("iclk_cfg is NULL", device);
		ret = -EINVAL;
		goto p_err;
	}

	if (!pdata->iclk_on) {
		merr("iclk_on is NULL", device);
		ret = -EINVAL;
		goto p_err;
	}

	ret = pdata->iclk_cfg(&core->pdev->dev, pdata->scenario, pdata->csi_ch);
	if (ret) {
		merr("iclk_cfg is fail(%d)", device, ret);
		goto p_err;
	}

	ret = pdata->iclk_on(&core->pdev->dev, pdata->scenario, pdata->csi_ch);
	if (ret) {
		merr("iclk_on is fail(%d)", device, ret);
		goto p_err;
	}

	set_bit(FIMC_IS_SENSOR_ICLK_ON, &device->state);

p_err:
	return ret;
}

int fimc_is_sensor_iclk_off(struct fimc_is_device_sensor *device)
{
	int ret = 0;
	struct exynos_platform_fimc_is_sensor *pdata;
	struct fimc_is_core *core;

	BUG_ON(!device);
	BUG_ON(!device->pdev);
	BUG_ON(!device->pdata);
	BUG_ON(!device->private_data);

	core = device->private_data;
	pdata = device->pdata;

	if (!test_bit(FIMC_IS_SENSOR_ICLK_ON, &device->state)) {
		merr("%s : already clk off", device, __func__);
		goto p_err;
	}

	if (!pdata->iclk_off) {
		merr("iclk_off is NULL", device);
		ret = -EINVAL;
		goto p_err;
	}

	ret = pdata->iclk_off(&core->pdev->dev, pdata->scenario, pdata->csi_ch);
	if (ret) {
		merr("iclk_off is fail(%d)", device, ret);
		goto p_err;
	}

	clear_bit(FIMC_IS_SENSOR_ICLK_ON, &device->state);

p_err:
	return ret;
}

int fimc_is_sensor_gpio_on(struct fimc_is_device_sensor *device)
{
	int ret = 0;
	u32 scenario, gpio_scenario;
	struct fimc_is_core *core;
	struct fimc_is_module_enum *module;
	struct fimc_is_vender *vender;

	BUG_ON(!device);
	BUG_ON(!device->pdev);
	BUG_ON(!device->pdata);
	BUG_ON(!device->private_data);

	module = NULL;
	gpio_scenario = GPIO_SCENARIO_ON;
	core = device->private_data;
	scenario = device->pdata->scenario;
	vender = &core->vender;

	if (scenario == SENSOR_SCENARIO_STANDBY) {
		minfo("%s: skip standby mode", device, __func__);
		goto p_err;
	}

	if (test_bit(FIMC_IS_SENSOR_GPIO_ON, &device->state)) {
		merr("%s : already gpio on", device, __func__);
		goto p_err;
	}

	ret = fimc_is_sensor_g_module(device, &module);
	if (ret) {
		merr("fimc_is_sensor_g_module is fail(%d)", device, ret);
		goto p_err;
	}

	ret = fimc_is_vender_module_sel(vender, module);
	if (ret) {
		merr("fimc_is_vender_module_sel is fail(%d)", device, ret);
		goto p_err;
	}

	if (!test_and_set_bit(FIMC_IS_MODULE_GPIO_ON, &module->state)) {
		struct exynos_platform_fimc_is_module *pdata;

		ret = fimc_is_vender_sensor_gpio_on_sel(vender,
			scenario, &gpio_scenario);
		if (ret) {
			clear_bit(FIMC_IS_MODULE_GPIO_ON, &module->state);
			merr("fimc_is_vender_sensor_gpio_on_sel is fail(%d)",
				device, ret);
			goto p_err;
		}

		pdata = module->pdata;
		if (!pdata) {
			clear_bit(FIMC_IS_MODULE_GPIO_ON, &module->state);
			merr("pdata is NULL", device);
			ret = -EINVAL;
			goto p_err;
		}

#ifdef CONFIG_CAMERA_USE_SOC_SENSOR
		if (scenario == SENSOR_SCENARIO_EXTERNAL) { /* i2c client */
			if (!pdata->gpio_soc_cfg) {
				clear_bit(FIMC_IS_MODULE_GPIO_ON, &module->state);
				merr("gpio_soc_cfg is NULL", device);
				ret = -EINVAL;
				goto p_err;
			}

			ret = pdata->gpio_soc_cfg(module->client, scenario, GPIO_SCENARIO_ON);
			if (ret) {
				clear_bit(FIMC_IS_MODULE_GPIO_ON, &module->state);
				merr("gpio_soc_cfg is fail(%d)", device, ret);
				goto p_err;
			}
		} else
#endif
		{
			if (!pdata->gpio_cfg) {
				clear_bit(FIMC_IS_MODULE_GPIO_ON, &module->state);
				merr("gpio_cfg is NULL", device);
				ret = -EINVAL;
				goto p_err;
			}

			ret = pdata->gpio_cfg(module->dev, scenario, gpio_scenario);
			if (ret) {
				clear_bit(FIMC_IS_MODULE_GPIO_ON, &module->state);
				merr("gpio_cfg is fail(%d)", device, ret);
				goto p_err;
			}
		}

		ret = fimc_is_vender_sensor_gpio_on(vender, scenario, gpio_scenario);
		if (ret) {
			merr("fimc_is_vender_sensor_gpio_on is fail(%d)", device, ret);
			goto p_err;
		}
	}

	set_bit(FIMC_IS_SENSOR_GPIO_ON, &device->state);

p_err:
	return ret;
}

int fimc_is_sensor_gpio_off(struct fimc_is_device_sensor *device)
{
	int ret = 0;
	u32 scenario, gpio_scenario;
	struct fimc_is_core *core;
	struct fimc_is_module_enum *module;
	struct fimc_is_vender *vender;
	struct fimc_is_device_preproc *device_preproc = NULL;
	struct fimc_is_module_enum *module_preproc = NULL;

	BUG_ON(!device);
	BUG_ON(!device->pdev);
	BUG_ON(!device->pdata);
	BUG_ON(!device->private_data);

	module = NULL;
	gpio_scenario = GPIO_SCENARIO_OFF;
	core = device->private_data;
	scenario = device->pdata->scenario;
	vender = &core->vender;

	if (!test_bit(FIMC_IS_SENSOR_GPIO_ON, &device->state)) {
		merr("%s : already gpio off", device, __func__);
		goto p_err;
	}

	device_preproc = &core->preproc;
	module_preproc = device_preproc->module;

	if (module_preproc != NULL && module_preproc->position == device->position) {
		if (test_bit(FIMC_IS_PREPROC_GPIO_ON, &device_preproc->state)) {
			info("%s : we will gpio off on device-preprocessor\n", __func__);
			clear_bit(FIMC_IS_SENSOR_GPIO_ON, &device->state);
			goto p_err;
		}
	}

	ret = fimc_is_sensor_g_module(device, &module);
	if (ret) {
		merr("fimc_is_sensor_g_module is fail(%d)", device, ret);
		goto p_err;
	}

	if (test_and_clear_bit(FIMC_IS_MODULE_GPIO_ON, &module->state)) {
		struct exynos_platform_fimc_is_module *pdata;

		ret = fimc_is_vender_sensor_gpio_off_sel(vender,
			scenario, &gpio_scenario);
		if (ret) {
			set_bit(FIMC_IS_MODULE_GPIO_ON, &module->state);
			merr("fimc_is_vender_sensor_gpio_off_sel is fail(%d)",
				device, ret);
			goto p_err;
		}

		pdata = module->pdata;
		if (!pdata) {
			set_bit(FIMC_IS_MODULE_GPIO_ON, &module->state);
			merr("pdata is NULL", device);
			ret = -EINVAL;
			goto p_err;
		}

#ifdef CONFIG_CAMERA_USE_SOC_SENSOR
		/* i2c client */
		if (scenario == SENSOR_SCENARIO_EXTERNAL) {
			if (!pdata->gpio_soc_cfg) {
				set_bit(FIMC_IS_MODULE_GPIO_ON, &module->state);
				merr("gpio_soc_cfg is NULL", device);
				ret = -EINVAL;
				goto p_err;
			}

			ret = pdata->gpio_soc_cfg(module->client, scenario, GPIO_SCENARIO_OFF);
			if (ret) {
				set_bit(FIMC_IS_MODULE_GPIO_ON, &module->state);
				merr("gpio_soc_cfg is fail(%d)", device, ret);
				goto p_err;
			}
		} else
#endif
		{
			if (!pdata->gpio_cfg) {
				set_bit(FIMC_IS_MODULE_GPIO_ON, &module->state);
				merr("gpio_cfg is NULL", device);
				ret = -EINVAL;
				goto p_err;
			}

			ret = pdata->gpio_cfg(module->dev, scenario, gpio_scenario);
			if (ret) {
				set_bit(FIMC_IS_MODULE_GPIO_ON, &module->state);
				merr("gpio_cfg is fail(%d)", device, ret);
				goto p_err;
			}
		}

		ret = fimc_is_vender_sensor_gpio_off(vender, scenario, gpio_scenario);
		if (ret) {
			merr("fimc_is_vender_sensor_gpio_off is fail(%d)", device, ret);
			goto p_err;
		}
	}

	clear_bit(FIMC_IS_SENSOR_GPIO_ON, &device->state);

p_err:
	if (module != NULL) {
		fimc_is_vender_module_del(vender, module);
	}

	return ret;
}

int fimc_is_sensor_gpio_dbg(struct fimc_is_device_sensor *device)
{
	int ret = 0;
	u32 scenario;
	struct fimc_is_module_enum *module;
	struct exynos_platform_fimc_is_module *pdata;

	BUG_ON(!device);
	BUG_ON(!device->pdev);
	BUG_ON(!device->pdata);

	ret = fimc_is_sensor_g_module(device, &module);
	if (ret) {
		merr("fimc_is_sensor_g_module is fail(%d)", device, ret);
		goto p_err;
	}

	scenario = device->pdata->scenario;
	pdata = module->pdata;
	if (!pdata) {
		merr("pdata is NULL", device);
		ret = -EINVAL;
		goto p_err;
	}

	if (!pdata->gpio_dbg) {
		merr("gpio_dbg is NULL", device);
		ret = -EINVAL;
		goto p_err;
	}

	ret = pdata->gpio_dbg(module->dev, scenario, GPIO_SCENARIO_ON);
	if (ret) {
		merr("gpio_dbg is fail(%d)", device, ret);
		goto p_err;
	}

p_err:
	return ret;
}

#ifndef ENABLE_IS_CORE
static void fimc_is_sensor_dump(struct fimc_is_device_sensor *device)
{
	int ret = 0;
	struct v4l2_subdev *subdev_module;

	subdev_module = device->subdev_module;

	ret = v4l2_subdev_call(subdev_module, core, log_status);
	if (ret) {
		err("module cannot support log_status(%d)", ret);
	}
}
#endif

#ifdef ENABLE_DTP
static void fimc_is_sensor_dtp(unsigned long data)
{
	struct fimc_is_video_ctx *vctx;
	struct fimc_is_device_sensor *device = (struct fimc_is_device_sensor *)data;
	struct fimc_is_framemgr *framemgr;
	struct fimc_is_frame *frame;
	unsigned long flags;
	u32 i;

	BUG_ON(!device);

	if (!device->force_stop && !device->dtp_check) {
		mwarn("Invalid DTP timer interrupt(force_stop: 0x%08lx, dtp_check: %d)",
			device, device->force_stop, device->dtp_check);
		return;
	}

	err("forcely reset due to 0x%08lx", device->force_stop);
	device->dtp_check = false;
	device->force_stop = 0;

#ifndef ENABLE_IS_CORE
	fimc_is_sensor_dump(device);
#endif

	set_bit(FIMC_IS_SENSOR_FRONT_DTP_STOP, &device->state);
	set_bit(FIMC_IS_SENSOR_BACK_NOWAIT_STOP, &device->state);

	vctx = device->vctx;
	if (!vctx) {
		err("vctx is NULL");
		return;
	}

	framemgr = GET_FRAMEMGR(vctx);
	framemgr_e_barrier_irqs(framemgr, FMGR_IDX_1, flags);

	for (i = 0; i < framemgr->num_frames; i++) {
		frame = &framemgr->frames[i];
		if (frame->state == FS_REQUEST) {
			err("buffer done1!!!! %d", i);
			trans_frame(framemgr, frame, FS_COMPLETE);
			CALL_VOPS(vctx, done, i, VB2_BUF_STATE_ERROR);
		} else if (frame->state == FS_PROCESS) {
			err("buffer done1!!!! %d", i);
			trans_frame(framemgr, frame, FS_COMPLETE);
			CALL_VOPS(vctx, done, i, VB2_BUF_STATE_ERROR);
		}
	}

	framemgr_x_barrier_irqr(framemgr, FMGR_IDX_1, flags);
}
#endif

static int fimc_is_sensor_start(struct fimc_is_device_sensor *device)
{
	int ret = 0;
	struct fimc_is_core *core;

	BUG_ON(!device);
	BUG_ON(!device->private_data);

	if (test_bit(FIMC_IS_SENSOR_DRIVING, &device->state)) {
		struct v4l2_subdev *subdev;

		subdev = device->subdev_module;
		if (!subdev) {
			merr("subdev is NULL", device);
			ret = -EINVAL;
			goto p_err;
		}
		ret = v4l2_subdev_call(subdev, video, s_stream, true);
		if (ret) {
			merr("v4l2_subdev_call(s_stream) is fail(%d)", device, ret);
			goto p_err;
		}
	} else {
		struct fimc_is_device_ischain *ischain;

		ischain = device->ischain;
		if (!ischain) {
			merr("ischain is NULL", device);
			ret = -EINVAL;
			goto p_err;
		}

		ret = fimc_is_itf_stream_on(ischain);
		if (ret) {
			merr("fimc_is_itf_stream_on is fail(%d)", device, ret);
			goto p_err;
		}

		core = device->private_data;
#ifndef ENABLE_IS_CORE
#ifdef CONFIG_OIS_USE
		ret = fimc_is_ois_mode_change(core, device->ois_mode);
		if (ret) {
			merr("fimc_is_ois_mode_change is fail(%d)", device, ret);
		}
#endif
		if (!device->subdev_module) {
			merr("subdev is NULL", device);
			ret = -EINVAL;
			goto p_err;
		}

		ret = v4l2_subdev_call(device->subdev_module, video, s_stream, true);
		if (ret) {
			merr("v4l2_subdev_call(s_stream) is fail(%d)", device, ret);
			goto p_err;
		}
#endif
	}

p_err:
	return ret;
}

static int fimc_is_sensor_stop(struct fimc_is_device_sensor *device)
{
	int ret = 0;

	BUG_ON(!device);

	if (test_bit(FIMC_IS_SENSOR_DRIVING, &device->state)) {
		struct v4l2_subdev *subdev;

		subdev = device->subdev_module;
		if (!subdev) {
			merr("subdev is NULL", device);
			ret = -EINVAL;
			goto p_err;
		}
		ret = v4l2_subdev_call(subdev, video, s_stream, false);
		if (ret) {
			merr("v4l2_subdev_call(s_stream) is fail(%d)", device, ret);
			goto p_err;
		}
	} else {
		struct fimc_is_device_ischain *ischain;

#ifndef ENABLE_IS_CORE
		if (device->subdev_module) {
		    ret = v4l2_subdev_call(device->subdev_module, video, s_stream, false);
		    if (ret)
			merr("v4l2_subdev_call(s_stream) is fail(%d)", device, ret);
		} else {
			merr("subdev is NULL", device);
		}
#endif

		ischain = device->ischain;
		if (!ischain) {
			merr("ischain is NULL", device);
			ret = -EINVAL;
			goto p_err;
		}
		ret = fimc_is_itf_stream_off(ischain);
		if (ret) {
			merr("fimc_is_itf_stream_off is fail(%d)", device, ret);
			goto p_err;
		}
	}

p_err:
	return ret;
}

int fimc_is_sensor_subdev_tag(struct fimc_is_device_sensor *device,
	struct fimc_is_frame *frame)
{
	int ret = 0;

	BUG_ON(!device);
	BUG_ON(!frame);

	if (frame->stream) {
		frame->stream->fcount = frame->fcount;
		frame->stream->rcount = 0;
		frame->stream->findex = 0;
		frame->stream->fvalid = 0;
	} else {
		merr("[F%d] subdev tag stream is null", device, frame->fcount);
	}

	return ret;
}

int fimc_is_sensor_tag(struct fimc_is_device_sensor *device,
	struct fimc_is_frame *frame)
{
	int ret = 0;
	u32 hashkey;

	BUG_ON(!device);
	BUG_ON(!frame);

	hashkey = frame->fcount % FIMC_IS_TIMESTAMP_HASH_KEY;
	frame->shot->dm.request.frameCount = frame->fcount;
	frame->shot->dm.sensor.timeStamp = device->timestamp[hashkey];
	frame->shot->udm.sensor.timeStampBoot = device->timestampboot[hashkey];
#ifdef DBG_JITTER
	fimc_is_jitter(frame->shot->dm.sensor.timeStamp);
#endif

	return ret;
}

static void fimc_is_sensor_control(struct work_struct *data)
{
/*
 * HAL can't send meta data for vision
 * We accepted vision control by s_ctrl
 */
#if 0
	struct v4l2_subdev *subdev_module;
	struct fimc_is_module_enum *module;
	struct camera2_sensor_ctl *rsensor_ctl;
	struct camera2_sensor_ctl *csensor_ctl;
	struct fimc_is_device_sensor *device;

	device = container_of(data, struct fimc_is_device_sensor, control_work);
	subdev_module = device->subdev_module;
	if (!subdev_module) {
		err("subdev_module is NULL");
		return;
	}

	module = v4l2_get_subdevdata(subdev_module);
	rsensor_ctl = &device->control_frame->shot->ctl.sensor;
	csensor_ctl = &device->sensor_ctl;

	if (rsensor_ctl->exposureTime != csensor_ctl->exposureTime) {
		CALL_MOPS(module, s_exposure, subdev_module, rsensor_ctl->exposureTime);
		csensor_ctl->exposureTime = rsensor_ctl->exposureTime;
	}

	if (rsensor_ctl->frameDuration != csensor_ctl->frameDuration) {
		CALL_MOPS(module, s_duration, subdev_module, rsensor_ctl->frameDuration);
		csensor_ctl->frameDuration = rsensor_ctl->frameDuration;
	}

	if (rsensor_ctl->sensitivity != csensor_ctl->sensitivity) {
		CALL_MOPS(module, s_again, subdev_module, rsensor_ctl->sensitivity);
		csensor_ctl->sensitivity = rsensor_ctl->sensitivity;
	}
#endif
}

static int fimc_is_sensor_notify_by_fstr(struct fimc_is_device_sensor *device, void *arg,
	unsigned int notification)
{
	int i = 0;
	int ret = 0;
	struct fimc_is_framemgr *framemgr;
	struct fimc_is_frame *frame;
	u32 hashkey;
	struct fimc_is_device_csi *csi;
	struct fimc_is_subdev *dma_subdev;

	BUG_ON(!device);
	BUG_ON(!arg);

	device->fcount = *(u32 *)arg;
	framemgr = GET_FRAMEMGR(device->vctx);

	if (device->instant_cnt) {
		device->instant_cnt--;
		if (device->instant_cnt == 0)
			wake_up(&device->instant_wait);
	}

	hashkey = device->fcount % FIMC_IS_TIMESTAMP_HASH_KEY;
	device->timestamp[hashkey] = fimc_is_get_timestamp();
	device->timestampboot[hashkey] = fimc_is_get_timestamp_boot();

	framemgr_e_barrier(framemgr, FMGR_IDX_28);

	frame = peek_frame(framemgr, FS_PROCESS);
	if (frame) {
#ifdef MEASURE_TIME
#ifdef EXTERNAL_TIME
		do_gettimeofday(&frame->tzone[TM_FLITE_STR]);
#endif
#endif
		frame->fcount = device->fcount;
#ifndef CONFIG_CAMERA_USE_SOC_SENSOR // TEMP_SOC
		fimc_is_sensor_tag(device, frame);
#endif
	}

#ifdef TASKLET_MSG
	if (!frame) {
		merr("[SEN] process is empty", device);
		frame_manager_print_queues(framemgr);
	}
#endif

	framemgr_x_barrier(framemgr, FMGR_IDX_28);

	switch (notification) {
	case CSIS_NOTIFY_FSTART:
		csi = v4l2_get_subdevdata(device->subdev_csi);
		if (!csi) {
			merr("CSI is NULL", device);
			return -EINVAL;
		}

		/* tagging for dma subdevs */
		for (i = ENTRY_SSVC1; i <= ENTRY_SSVC3; i++) {
			dma_subdev = csi->dma_subdev[i];

			if (!dma_subdev ||
				!test_bit(FIMC_IS_SUBDEV_START, &dma_subdev->state))
				continue;

			framemgr = GET_SUBDEV_FRAMEMGR(dma_subdev);

			framemgr_e_barrier(framemgr, 0);

			frame = peek_frame(framemgr, FS_PROCESS);
			if (frame) {
				frame->fcount = device->fcount;
				fimc_is_sensor_subdev_tag(device, frame);
			}

			framemgr_x_barrier(framemgr, 0);
		}
		break;
	default:
		break;
	}

	return ret;
}

static int fimc_is_sensor_notify_by_fend(struct fimc_is_device_sensor *device, void *arg,
	unsigned int notification)
{
	int ret = 0;
	u32 done_state = 0;
	struct fimc_is_video_ctx *vctx;
	struct fimc_is_frame *frame;

	BUG_ON(!device);

#ifdef ENABLE_DTP
	if (device->dtp_check) {
		device->dtp_check = false;
		del_timer(&device->dtp_timer);
	}

	if (device->force_stop)
		fimc_is_sensor_dtp((unsigned long)device);
#endif

	frame = (struct fimc_is_frame *)arg;
	if (frame) {
#ifdef MEASURE_TIME
#ifdef EXTERNAL_TIME
		do_gettimeofday(&frame->tzone[TM_FLITE_END]);
#endif
#endif
		/* get video context */
		vctx = (frame->subdev) ?
			((struct fimc_is_subdev *)frame->subdev)->vctx : device->vctx;
		BUG_ON(!vctx);

		/* for debug */
		DBG_DIGIT_TAG(GROUP_SLOT_MAX, 0, GET_QUEUE(vctx), frame, frame->fcount);

		done_state = (frame->result) ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE;
		CALL_VOPS(vctx, done, frame->index, done_state);

		/* device driving */
		if (!(frame->subdev) && test_bit(FIMC_IS_SENSOR_DRIVING, &device->state)) {
			device->control_frame = frame;
			schedule_work(&device->control_work);
		}
	}

	return ret;
}

static void fimc_is_sensor_notify(struct v4l2_subdev *subdev,
	unsigned int notification,
	void *arg)
{
	int ret = 0;
	struct fimc_is_device_sensor *device;

	BUG_ON(!subdev);

	device = v4l2_get_subdev_hostdata(subdev);

	switch(notification) {
	case FLITE_NOTIFY_FSTART:
	case CSIS_NOTIFY_FSTART:
		ret = fimc_is_sensor_notify_by_fstr(device, arg, notification);
		if (ret)
			merr("fimc_is_sensor_notify_by_fstr is fail(%d)", device, ret);
		break;
	case FLITE_NOTIFY_FEND:
	case CSIS_NOTIFY_FEND:
		ret = fimc_is_sensor_notify_by_fend(device, arg, notification);
		if (ret)
			merr("fimc_is_sensor_notify_by_fend is fail(%d)", device, ret);
		break;
#ifndef ENABLE_IS_CORE
	case CSI_NOTIFY_VSYNC:
		ret = v4l2_subdev_call(device->subdev_module, core, ioctl, V4L2_CID_SENSOR_NOTIFY_VSYNC, arg);
		if (ret)
			merr("fimc is sensor notify vsync is fail", device);
		break;
	case CSI_NOTIFY_VBLANK:
		ret = v4l2_subdev_call(device->subdev_module, core, ioctl, V4L2_CID_SENSOR_NOTIFY_VBLANK, arg);
 		if (ret)
			merr("fimc is sensor notify vblank is fail", device);
 		break;
#endif
	}
}

static void fimc_is_sensor_instanton(struct work_struct *data)
{
	int ret = 0;
	u32 instant_cnt;
	struct fimc_is_device_sensor *device;

	BUG_ON(!data);

	device = container_of(data, struct fimc_is_device_sensor, instant_work);
	instant_cnt = device->instant_cnt;

	clear_bit(FIMC_IS_SENSOR_FRONT_DTP_STOP, &device->state);
	clear_bit(FIMC_IS_SENSOR_BACK_NOWAIT_STOP, &device->state);

	ret = fimc_is_sensor_start(device);
	if (ret) {
		struct v4l2_subdev *subdev_csi;
		int csi_ret;
		subdev_csi = device->subdev_csi;

		merr("fimc_is_sensor_start is fail(%d)\n", device, ret);
		/* csi disable when error occured */
		csi_ret = v4l2_subdev_call(subdev_csi, video, s_stream, IS_DISABLE_STREAM);
		if (csi_ret)
			merr("v4l2_csi_call(s_stream) is fail(%d)", device, csi_ret);
		goto p_err;
	}
	set_bit(FIMC_IS_SENSOR_FRONT_START, &device->state);

#ifdef ENABLE_DTP
	if (device->dtp_check) {
		setup_timer(&device->dtp_timer, fimc_is_sensor_dtp, (unsigned long)device);
		mod_timer(&device->dtp_timer, jiffies +  msecs_to_jiffies(300));
		info("DTP checking...\n");
	}
#endif

#ifdef FW_SUSPEND_RESUME
	if (!instant_cnt) {
		/* save launch state */
		struct fimc_is_interface *itf;
		itf = device->ischain->interface;

		if (test_bit(IS_IF_SUSPEND, &itf->fw_boot)) {
			set_bit(IS_IF_LAUNCH_FIRST, &itf->launch_state);
			set_bit(IS_IF_LAUNCH_SUCCESS, &itf->launch_state);
		}
	}
#endif

	if (instant_cnt) {
		ulong timetowait, timetoelapse, timeout;

		timeout = FIMC_IS_FLITE_STOP_TIMEOUT + msecs_to_jiffies(instant_cnt*60);
		timetowait = wait_event_timeout(device->instant_wait,
			(device->instant_cnt == 0),
			timeout);
		if (!timetowait) {
			merr("wait_event_timeout is invalid", device);
			ret = -ETIME;
		}

		fimc_is_sensor_front_stop(device);

		timetoelapse = (jiffies_to_msecs(timeout) - jiffies_to_msecs(timetowait));
		info("[FRT:D:%d] instant off(fcount : %d, time : %ldms)\n", device->instance,
			device->instant_cnt,
			timetoelapse);
	}

p_err:
	device->instant_ret = ret;
}

static int fimc_is_sensor_probe(struct platform_device *pdev)
{
	int ret = 0;
	u32 instance = -1;
	atomic_t device_id;
	struct fimc_is_core *core;
	struct fimc_is_device_sensor *device;
	void *pdata;

	BUG_ON(!pdev);

	if (fimc_is_dev == NULL) {
		warn("fimc_is_dev is not yet probed(sensor)");
		pdev->dev.init_name = FIMC_IS_SENSOR_DEV_NAME;
		return -EPROBE_DEFER;
	}

	core = (struct fimc_is_core *)dev_get_drvdata(fimc_is_dev);
	if (!core) {
		err("core is NULL");
		return -EINVAL;
	}

#ifdef CONFIG_OF
	ret = fimc_is_sensor_parse_dt(pdev);
	if (ret) {
		err("parsing device tree is fail(%d)", ret);
		goto p_err;
	}
#endif

	pdata = dev_get_platdata(&pdev->dev);
	if (!pdata) {
		err("pdata is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	/* 1. get device */
	atomic_set(&device_id, pdev->id);
	device = &core->sensor[pdev->id];

	/* v4l2 device and device init */
	memset(&device->v4l2_dev, 0, sizeof(struct v4l2_device));
	instance = v4l2_device_set_name(&device->v4l2_dev, FIMC_IS_SENSOR_DEV_NAME, &device_id);
	device->v4l2_dev.notify = fimc_is_sensor_notify;
	device->instance = instance;
	device->position = SENSOR_POSITION_REAR;
	device->resourcemgr = &core->resourcemgr;
	device->pdev = pdev;
	device->private_data = core;
	device->pdata = pdata;
	platform_set_drvdata(pdev, device);
	init_waitqueue_head(&device->instant_wait);
	INIT_WORK(&device->instant_work, fimc_is_sensor_instanton);
	INIT_WORK(&device->control_work, fimc_is_sensor_control);
	spin_lock_init(&device->slock_state);
	device_init_wakeup(&pdev->dev, true);

	/* 3. state init*/
	clear_bit(FIMC_IS_SENSOR_PROBE, &device->state);
	clear_bit(FIMC_IS_SENSOR_OPEN, &device->state);
	clear_bit(FIMC_IS_SENSOR_MCLK_ON, &device->state);
	clear_bit(FIMC_IS_SENSOR_ICLK_ON, &device->state);
	clear_bit(FIMC_IS_SENSOR_GPIO_ON, &device->state);
	clear_bit(FIMC_IS_SENSOR_S_INPUT, &device->state);
	clear_bit(FIMC_IS_SENSOR_S_CONFIG, &device->state);
	clear_bit(FIMC_IS_SENSOR_DRIVING, &device->state);
	clear_bit(FIMC_IS_SENSOR_FRONT_START, &device->state);
	clear_bit(FIMC_IS_SENSOR_FRONT_DTP_STOP, &device->state);
	clear_bit(FIMC_IS_SENSOR_BACK_START, &device->state);
	clear_bit(FIMC_IS_SENSOR_BACK_NOWAIT_STOP, &device->state);

#ifdef ENABLE_DTP
	device->dtp_check = false;
#endif

	ret = fimc_is_mem_init(&device->mem, device->pdev);
	if (ret) {
		merr("fimc_is_mem_probe is fail(%d)", device, ret);
		goto p_err;
	}

#if defined(CONFIG_PM_RUNTIME)
	pm_runtime_enable(&pdev->dev);
#endif
#if defined(SUPPORTED_EARLYBUF_DONE_SW)
	hrtimer_init(&device->early_buf_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
#endif
	ret = v4l2_device_register(&pdev->dev, &device->v4l2_dev);
	if (ret) {
		merr("v4l2_device_register is fail(%d)", device, ret);
		goto p_err;
	}

	ret = fimc_is_csi_probe(device, device->pdata->csi_ch);
	if (ret) {
		merr("fimc_is_csi_probe is fail(%d)", device, ret);
		goto p_err;
	}

	ret = fimc_is_flite_probe(device, device->pdata->flite_ch);
	if (ret) {
		merr("fimc_is_flite_probe is fail(%d)", device, ret);
		goto p_err;
	}

	ret = fimc_is_ssx_video_probe(device);
	if (ret) {
		merr("fimc_is_sensor_video_probe is fail(%d)", device, ret);
		goto p_err;
	}

#ifdef SOC_SSVC0
	/* this configure can be enabled, if sensor group created */
	ret = fimc_is_ssxvc0_video_probe(device);
	if (ret) {
		merr("fimc_is_ssxvc0_video_probe is fail(%d)", device, ret);
		goto p_err;
	}

	fimc_is_subdev_probe(&device->ssvc0, instance, ENTRY_SSVC0, "SSXVC0", NULL);
	clear_bit(FIMC_IS_SUBDEV_OPEN, &device->ssvc0.state);
	clear_bit(FIMC_IS_SUBDEV_START, &device->ssvc0.state);
#endif

#ifdef SOC_SSVC1
	ret = fimc_is_ssxvc1_video_probe(device);
	if (ret) {
		merr("fimc_is_ssxvc1_video_probe is fail(%d)", device, ret);
		goto p_err;
	}

	fimc_is_subdev_probe(&device->ssvc1, instance, ENTRY_SSVC1, "SSXVC1", NULL);
	clear_bit(FIMC_IS_SUBDEV_OPEN, &device->ssvc1.state);
	clear_bit(FIMC_IS_SUBDEV_START, &device->ssvc1.state);
#endif

#ifdef SOC_SSVC2
	ret = fimc_is_ssxvc2_video_probe(device);
	if (ret) {
		merr("fimc_is_ssxvc2_video_probe is fail(%d)", device, ret);
		goto p_err;
	}

	fimc_is_subdev_probe(&device->ssvc2, instance, ENTRY_SSVC2, "SSXVC2", NULL);
	clear_bit(FIMC_IS_SUBDEV_OPEN, &device->ssvc2.state);
	clear_bit(FIMC_IS_SUBDEV_START, &device->ssvc2.state);
#endif

#ifdef SOC_SSVC3
	ret = fimc_is_ssxvc3_video_probe(device);
	if (ret) {
		merr("fimc_is_ssxvc3_video_probe is fail(%d)", device, ret);
		goto p_err;
	}

	fimc_is_subdev_probe(&device->ssvc3, instance, ENTRY_SSVC3, "SSXVC3", NULL);
	clear_bit(FIMC_IS_SUBDEV_OPEN, &device->ssvc3.state);
	clear_bit(FIMC_IS_SUBDEV_START, &device->ssvc3.state);
#endif

	set_bit(FIMC_IS_SENSOR_PROBE, &device->state);

p_err:
	info("[%d][SEN:D] %s(%d)\n", instance, __func__, ret);
	return ret;
}

static int fimc_is_sensor_remove(struct platform_device *pdev)
{
	int ret = 0;

	info("%s\n", __func__);

	return ret;
}

int fimc_is_sensor_open(struct fimc_is_device_sensor *device,
	struct fimc_is_video_ctx *vctx)
{
	int ret = 0;

	BUG_ON(!device);
	BUG_ON(!device->subdev_csi);

	if (test_bit(FIMC_IS_SENSOR_OPEN, &device->state)) {
		merr("already open", device);
		ret = -EMFILE;
		goto p_err;
	}

	/*
	 * Sensor's mclk can be accessed by other ips(ex. preprocessor)
	 * There's a problem that mclk_on can be called twice due to clear_bit.
	 *  - clear_bit(FIMC_IS_SENSOR_MCLK_ON, &device->state);
	 * Clear_bit of MCLK_ON should be skiped.
	 */
	clear_bit(FIMC_IS_SENSOR_S_INPUT, &device->state);
	clear_bit(FIMC_IS_SENSOR_S_CONFIG, &device->state);
	clear_bit(FIMC_IS_SENSOR_DRIVING, &device->state);
	clear_bit(FIMC_IS_SENSOR_FRONT_START, &device->state);
	clear_bit(FIMC_IS_SENSOR_FRONT_DTP_STOP, &device->state);
	clear_bit(FIMC_IS_SENSOR_BACK_START, &device->state);
	set_bit(FIMC_IS_SENSOR_BACK_NOWAIT_STOP, &device->state);

	device->vctx = vctx;
	device->fcount = 0;
	device->instant_cnt = 0;
	device->instant_ret = 0;
	device->ischain = NULL;
	device->exposure_time = 0;
	device->frame_duration = 0;
	device->force_stop = 0;
	device->early_buf_done_mode = 0;
	memset(&device->sensor_ctl, 0, sizeof(struct camera2_sensor_ctl));
	memset(&device->lens_ctl, 0, sizeof(struct camera2_lens_ctl));
	memset(&device->flash_ctl, 0, sizeof(struct camera2_flash_ctl));

	/* get camif ip for wdma */
	ret = fimc_is_hw_camif_open((void *)device);
	if (ret) {
		merr("fimc_is_hw_camif_open is fail", device);
		goto p_err;
	}

	ret = fimc_is_csi_open(device->subdev_csi, GET_FRAMEMGR(vctx));
	if (ret) {
		merr("fimc_is_csi_open is fail(%d)", device, ret);
		goto p_err;
	}

	ret = fimc_is_flite_open(device->subdev_flite, GET_FRAMEMGR(vctx));
	if (ret) {
		merr("fimc_is_flite_open is fail(%d)", device, ret);
		goto p_err;
	}

	/* for mediaserver force close */
	ret = fimc_is_resource_get(device->resourcemgr, device->instance);
	if (ret) {
		merr("fimc_is_resource_get is fail", device);
		goto p_err;
	}

#ifdef ENABLE_DTP
	device->dtp_check = true;
#endif

	set_bit(FIMC_IS_SENSOR_OPEN, &device->state);

p_err:
	minfo("[SEN:D] %s():%d\n", device, __func__, ret);
	return ret;
}

int fimc_is_sensor_close(struct fimc_is_device_sensor *device)
{
	int ret = 0;
	struct fimc_is_device_ischain *ischain;
	struct fimc_is_group *group_3aa;

	BUG_ON(!device);
	BUG_ON(!device->vctx);

	if (!test_bit(FIMC_IS_SENSOR_OPEN, &device->state)) {
		merr("already close", device);
		ret = -EMFILE;
		goto p_err;
	}

	/* for mediaserver force close */
	ischain = device->ischain;
	if (ischain) {
		group_3aa = &ischain->group_3aa;
		if (test_bit(FIMC_IS_GROUP_START, &group_3aa->state)) {
			info("media server is dead, 3ax forcely done\n");
			set_bit(FIMC_IS_GROUP_REQUEST_FSTOP, &group_3aa->state);
		}
	}

	ret = fimc_is_sensor_back_stop(device, GET_QUEUE(device->vctx));
	if (ret)
		merr("fimc_is_sensor_back_stop is fail(%d)", device, ret);

	ret = fimc_is_sensor_front_stop(device);
	if (ret)
		merr("fimc_is_sensor_front_stop is fail(%d)", device, ret);

	ret = fimc_is_csi_close(device->subdev_csi);
	if (ret)
		merr("fimc_is_flite_close is fail(%d)", device, ret);

	ret = fimc_is_flite_close(device->subdev_flite);
	if (ret)
		merr("fimc_is_flite_close is fail(%d)", device, ret);

	/* cancel a work and wait for it to finish */
	cancel_work_sync(&device->control_work);
	cancel_work_sync(&device->instant_work);

#ifndef ENABLE_IS_CORE
	if (device->subdev_module) {
		ret = v4l2_subdev_call(device->subdev_module, core, ioctl, V4L2_CID_SENSOR_DEINIT, device);
		if (ret)
			merr("fimc is sensor deinit is fail, ret(%d)", device, ret);
	}
#endif

	/* for mediaserver force close */
	ret = fimc_is_resource_put(device->resourcemgr, device->instance);
	if (ret)
		merr("fimc_is_resource_put is fail", device);

	clear_bit(FIMC_IS_SENSOR_OPEN, &device->state);
	clear_bit(FIMC_IS_SENSOR_S_INPUT, &device->state);
	clear_bit(FIMC_IS_SENSOR_S_CONFIG, &device->state);
	set_bit(FIMC_IS_SENSOR_BACK_NOWAIT_STOP, &device->state);

p_err:
	minfo("[SEN:D] %s():%d\n", device, __func__, ret);
	return ret;
}

int fimc_is_sensor_s_input(struct fimc_is_device_sensor *device,
	u32 input,
	u32 scenario)
{
	int ret = 0;
	struct fimc_is_core *core = (struct fimc_is_core *)dev_get_drvdata(fimc_is_dev);
	struct v4l2_subdev *subdev_module;
	struct v4l2_subdev *subdev_csi;
	struct v4l2_subdev *subdev_flite;
	struct fimc_is_module_enum *module_enum, *module;
	u32 sensor_index;

	BUG_ON(!device);
	BUG_ON(!device->pdata);
	BUG_ON(!device->subdev_csi);
	BUG_ON(input >= SENSOR_NAME_END);

	if (test_bit(FIMC_IS_SENSOR_S_INPUT, &device->state)) {
		merr("already s_input", device);
		ret = -EINVAL;
		goto p_err;
	}

	module = NULL;
	module_enum = device->module_enum;

	for (sensor_index = 0; sensor_index < SENSOR_MAX_ENUM; sensor_index++) {
		if (module_enum[sensor_index].sensor_id == input) {
			module = &module_enum[sensor_index];
			break;
		}
	}

	if (!module) {
		merr("module is not probed, sensor_index = %d", device, sensor_index);
		ret = -EINVAL;
		goto p_err;
	}

	subdev_module = module->subdev;
	if (!subdev_module) {
		merr("subdev module is not probed", device);
		ret = -EINVAL;
		goto p_err;
	}

	subdev_csi = device->subdev_csi;
	subdev_flite = device->subdev_flite;
	device->position = module->position;
	device->image.framerate = min_t(u32, SENSOR_DEFAULT_FRAMERATE, module->max_framerate);
	device->image.window.width = module->pixel_width;
	device->image.window.height = module->pixel_height;
	device->image.window.o_width = device->image.window.width;
	device->image.window.o_height = device->image.window.height;

	core->current_position = module->position;

	/* send csi chennel to FW */
	module->ext.sensor_con.csi_ch = device->pdata->csi_ch;
	module->ext.sensor_con.csi_ch |= 0x0100;

	device->pdata->scenario = scenario;
	if (scenario == SENSOR_SCENARIO_NORMAL || scenario == SENSOR_SCENARIO_STANDBY)
		clear_bit(FIMC_IS_SENSOR_DRIVING, &device->state);
	else
		set_bit(FIMC_IS_SENSOR_DRIVING, &device->state);

	if (device->subdev_module) {
		mwarn("subdev_module is already registered", device);
		v4l2_device_unregister_subdev(device->subdev_module);
	}

	ret = v4l2_device_register_subdev(&device->v4l2_dev, subdev_module);
	if (ret) {
		merr("v4l2_device_register_subdev is fail(%d)", device, ret);
		goto p_err;
	}

	device->subdev_module = subdev_module;

#if defined(CONFIG_PM_DEVFREQ)
	if (test_bit(FIMC_IS_SENSOR_DRIVING, &device->state)) {
		pm_qos_add_request(&exynos_sensor_qos_cam, PM_QOS_DEVICE_THROUGHPUT, 500000);
		pm_qos_add_request(&exynos_sensor_qos_mem, PM_QOS_BUS_THROUGHPUT, 632000);
	}
#endif

	ret = fimc_is_sensor_mclk_on(device, scenario, module->pdata->mclk_ch);
	if (ret) {
		merr("fimc_is_sensor_mclk_on is fail(%d)", device, ret);
		goto p_err;
	}

	/* Sensor power on */
	ret = fimc_is_sensor_gpio_on(device);
	if (ret) {
		merr("fimc_is_sensor_gpio_on is fail(%d)", device, ret);
		goto p_err;
	}

	ret = v4l2_subdev_call(subdev_flite, core, init, device->pdata->csi_ch);
	if (ret) {
		merr("v4l2_flite_call(init) is fail(%d)", device, ret);
		goto p_err;
	}

	ret = v4l2_subdev_call(subdev_csi, core, init, sensor_index);
	if (ret) {
		merr("v4l2_csi_call(init) is fail(%d)", device, ret);
		goto p_err;
	}

#ifdef ENABLE_IS_CORE
	if (test_bit(FIMC_IS_SENSOR_DRIVING, &device->state))
#endif
	{
		ret = v4l2_subdev_call(subdev_module, core, init, 0);
		if (ret) {
			merr("v4l2_module_call(init) is fail(%d)", device, ret);
			goto p_err;
		}
	}

	set_bit(FIMC_IS_SENSOR_S_INPUT, &device->state);

p_err:
	minfo("[SEN:D] %s(%d, %d):%d\n", device, __func__, input, scenario, ret);
	return ret;
}

static int fimc_is_sensor_reqbufs(void *qdevice,
	struct fimc_is_queue *queue,
	u32 count)
{
	return 0;
}

static int fimc_is_sensor_s_format(void *qdevice,
	struct fimc_is_queue *queue)
{
	int ret = 0;
	struct fimc_is_device_sensor *device = qdevice;
	struct v4l2_subdev *subdev_module;
	struct v4l2_subdev *subdev_csi;
	struct v4l2_subdev *subdev_flite;
	struct v4l2_mbus_framefmt subdev_format;
	struct fimc_is_fmt *format;
	u32 width;
	u32 height;

	BUG_ON(!device);
	BUG_ON(!device->subdev_module);
	BUG_ON(!device->subdev_csi);
	BUG_ON(!device->subdev_flite);
	BUG_ON(!queue);

	subdev_module = device->subdev_module;
	subdev_csi = device->subdev_csi;
	subdev_flite = device->subdev_flite;
	format = queue->framecfg.format;
	width = queue->framecfg.width;
	height = queue->framecfg.height;

	memcpy(&device->image.format, format, sizeof(struct fimc_is_fmt));
	device->image.window.offs_h = 0;
	device->image.window.offs_v = 0;
	device->image.window.width = width;
	device->image.window.o_width = width;
	device->image.window.height = height;
	device->image.window.o_height = height;

	subdev_format.code = format->pixelformat;
	subdev_format.field = format->field;
	subdev_format.width = width;
	subdev_format.height = height;
	subdev_format.reserved[0] = format->bitwidth;

#ifdef ENABLE_IS_CORE
	if (test_bit(FIMC_IS_SENSOR_DRIVING, &device->state))
#endif
	{
		device->cfg = fimc_is_sensor_g_mode(device);
		if (!device->cfg) {
			merr("sensor cfg is invalid", device);
			ret = -EINVAL;
			goto p_err;
		}
	}

#ifdef ENABLE_IS_CORE
	if (test_bit(FIMC_IS_SENSOR_DRIVING, &device->state))
#endif
	{
		ret = v4l2_subdev_call(subdev_module, video, s_mbus_fmt, &subdev_format);
		if (ret) {
			merr("v4l2_module_call(s_format) is fail(%d)", device, ret);
			goto p_err;
		}
	}

	ret = v4l2_subdev_call(subdev_csi, video, s_mbus_fmt, &subdev_format);
	if (ret) {
		merr("v4l2_csi_call(s_format) is fail(%d)", device, ret);
		goto p_err;
	}

	ret = v4l2_subdev_call(subdev_flite, video, s_mbus_fmt, &subdev_format);
	if (ret) {
		merr("v4l2_flite_call(s_format) is fail(%d)", device, ret);
		goto p_err;
	}

p_err:
	return ret;
}

int fimc_is_sensor_s_framerate(struct fimc_is_device_sensor *device,
	struct v4l2_streamparm *param)
{
	int ret = 0;
	struct v4l2_subdev *subdev_module;
	struct v4l2_subdev *subdev_csi;
	struct fimc_is_module_enum *module;
	struct v4l2_captureparm *cp;
	struct v4l2_fract *tpf;
	u32 framerate = 0;

	BUG_ON(!device);
	BUG_ON(!device->subdev_module);
	BUG_ON(!device->subdev_csi);
	BUG_ON(!param);

	cp = &param->parm.capture;
	tpf = &cp->timeperframe;

	if (!tpf->numerator) {
		merr("numerator is 0", device);
		ret = -EINVAL;
		goto p_err;
	}

	framerate = tpf->denominator / tpf->numerator;
	subdev_module = device->subdev_module;
	subdev_csi = device->subdev_csi;

	if (framerate == 0) {
		mwarn("frame rate 0 request is ignored", device);
		goto p_err;
	}

	module = (struct fimc_is_module_enum *)v4l2_get_subdevdata(subdev_module);
	if (!module) {
		merr("module is NULL", device);
		ret = -EINVAL;
		goto p_err;
	}

	if (test_bit(FIMC_IS_SENSOR_FRONT_START, &device->state)) {
		merr("front is already stream on", device);
		ret = -EINVAL;
		goto p_err;
	}

	if (param->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		merr("type is invalid(%d)", device, param->type);
		ret = -EINVAL;
		goto p_err;
	}

	if (framerate > module->max_framerate) {
		merr("framerate is invalid(%d > %d)", device, framerate, module->max_framerate);
		ret = -EINVAL;
		goto p_err;
	}

	ret = v4l2_subdev_call(subdev_csi, video, s_parm, param);
	if (ret) {
		merr("v4l2_csi_call(s_param) is fail(%d)", device, ret);
		goto p_err;
	}

	if (test_bit(FIMC_IS_SENSOR_DRIVING, &device->state)) {
		ret = v4l2_subdev_call(subdev_module, video, s_parm, param);
		if (ret) {
			merr("v4l2_module_call(s_param) is fail(%d)", device, ret);
			goto p_err;
		}
	}

	device->image.framerate = framerate;
	device->max_target_fps = framerate;

	minfo("[SEN:D] framerate: req@%dfps, cur@%dfps\n", device,
		framerate, device->image.framerate);

p_err:
	return ret;
}

int fimc_is_sensor_s_ctrl(struct fimc_is_device_sensor *device,
	struct v4l2_control *ctrl)
{
	int ret = 0;
	struct v4l2_subdev *subdev_module;

	BUG_ON(!device);
	BUG_ON(!device->subdev_module);
	BUG_ON(!device->subdev_csi);
	BUG_ON(!ctrl);

	subdev_module = device->subdev_module;

	ret = v4l2_subdev_call(subdev_module, core, s_ctrl, ctrl);
	if (ret) {
		err("s_ctrl is fail(%d)", ret);
		goto p_err;
	}

p_err:
	return ret;
}

int fimc_is_sensor_s_bns(struct fimc_is_device_sensor *device,
	u32 ratio)
{
	int ret = 0;
	struct v4l2_subdev *subdev_flite;
	u32 sensor_width, sensor_height;

	BUG_ON(!device);
	BUG_ON(!device->subdev_flite);

	subdev_flite = device->subdev_flite;

	sensor_width = fimc_is_sensor_g_width(device);
	sensor_height = fimc_is_sensor_g_height(device);
	if (!sensor_width || !sensor_height) {
		merr("Sensor size is zero. Sensor set_format first.\n", device);
		ret = -EINVAL;
		goto p_err;
	}

	device->image.window.otf_width
		= rounddown((sensor_width * 1000 / ratio), 4);
	device->image.window.otf_height
		= rounddown((sensor_height * 1000 / ratio), 2);

p_err:
	return ret;
}

int fimc_is_sensor_s_frame_duration(struct fimc_is_device_sensor *device,
	u32 framerate)
{
	int ret = 0;
	u64 frame_duration;
	struct v4l2_subdev *subdev_module;
	struct fimc_is_module_enum *module;

	BUG_ON(!device);

	subdev_module = device->subdev_module;
	if (!subdev_module) {
		err("subdev_module is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	module = v4l2_get_subdevdata(subdev_module);
	if (!module) {
		err("module is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	/* unit : nano */
	frame_duration = (1000 * 1000 * 1000) / framerate;
	if (frame_duration <= 0) {
		err("it is wrong frame duration(%lld)", frame_duration);
		ret = -EINVAL;
		goto p_err;
	}

	if (device->frame_duration != frame_duration) {
		CALL_MOPS(module, s_duration, subdev_module, frame_duration);
		device->frame_duration = frame_duration;
	}

p_err:
	return ret;
}

int fimc_is_sensor_s_exposure_time(struct fimc_is_device_sensor *device,
	u32 exposure_time)
{
	int ret = 0;
	struct v4l2_subdev *subdev_module;
	struct fimc_is_module_enum *module;

	BUG_ON(!device);

	subdev_module = device->subdev_module;
	if (!subdev_module) {
		err("subdev_module is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	module = v4l2_get_subdevdata(subdev_module);
	if (!module) {
		err("module is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	if (exposure_time <= 0) {
		err("it is wrong exposure time (%d)", exposure_time);
		ret = -EINVAL;
		goto p_err;
	}

	if (device->exposure_time != exposure_time) {
		CALL_MOPS(module, s_exposure, subdev_module, exposure_time);
		device->exposure_time = exposure_time;
	}
p_err:
	return ret;
}

int fimc_is_sensor_s_fcount(struct fimc_is_device_sensor *device)
{
	int ret = 0;
	struct fimc_is_device_csi *csi;

	csi = v4l2_get_subdevdata(device->subdev_csi);

	switch (csi->instance) {
	case CSI_ID_A:
		if (device->fcount != *notify_fcount_sen0)
			writel(device->fcount, notify_fcount_sen0);
		break;
	case CSI_ID_B:
		if (device->fcount != *notify_fcount_sen1)
			writel(device->fcount, notify_fcount_sen1);
		break;
	case CSI_ID_C:
		if (device->fcount != *notify_fcount_sen2)
			writel(device->fcount, notify_fcount_sen2);
		break;
	case CSI_ID_D:
		if (device->fcount != *notify_fcount_sen3)
			writel(device->fcount, notify_fcount_sen3);
		break;
	default:
		merr("[SEN:D] CSI ch%d is invlid", device, csi->instance);
		ret = -EINVAL;
		break;
	}

	return ret;
}

int fimc_is_sensor_g_ctrl(struct fimc_is_device_sensor *device,
	struct v4l2_control *ctrl)
{
	int ret = 0;
	struct v4l2_subdev *subdev_module;

	BUG_ON(!device);
	BUG_ON(!device->subdev_module);
	BUG_ON(!device->subdev_csi);
	BUG_ON(!ctrl);

	subdev_module = device->subdev_module;

	ret = v4l2_subdev_call(subdev_module, core, g_ctrl, ctrl);
	if (ret) {
		err("g_ctrl is fail(%d)", ret);
		goto p_err;
	}

p_err:
	return ret;
}


int fimc_is_sensor_g_instance(struct fimc_is_device_sensor *device)
{
	BUG_ON(!device);
	return device->instance;
}

int fimc_is_sensor_g_fcount(struct fimc_is_device_sensor *device)
{
	BUG_ON(!device);
	return device->fcount;
}

int fimc_is_sensor_g_framerate(struct fimc_is_device_sensor *device)
{
	BUG_ON(!device);
	return device->image.framerate;
}

int fimc_is_sensor_g_width(struct fimc_is_device_sensor *device)
{
	BUG_ON(!device);
	return device->image.window.width;
}

int fimc_is_sensor_g_height(struct fimc_is_device_sensor *device)
{
	BUG_ON(!device);
	return device->image.window.height;
}

int fimc_is_sensor_g_bns_width(struct fimc_is_device_sensor *device)
{
	BUG_ON(!device);

	if (device->image.window.otf_width)
		return device->image.window.otf_width;

	return device->image.window.width;
}

int fimc_is_sensor_g_bns_height(struct fimc_is_device_sensor *device)
{
	BUG_ON(!device);
	if (device->image.window.otf_height)
		return device->image.window.otf_height;

	return device->image.window.height;
}

int fimc_is_sensor_g_bns_ratio(struct fimc_is_device_sensor *device)
{
	int binning = 0;
	u32 sensor_width, sensor_height;
	u32 bns_width, bns_height;

	BUG_ON(!device);

	sensor_width = fimc_is_sensor_g_width(device);
	sensor_height = fimc_is_sensor_g_height(device);
	bns_width = fimc_is_sensor_g_bns_width(device);
	bns_height = fimc_is_sensor_g_bns_height(device);

	binning = min(BINNING(sensor_width, bns_width),
		BINNING(sensor_height, bns_height));

	return binning;
}

int fimc_is_sensor_g_bratio(struct fimc_is_device_sensor *device)
{
	int binning = 0;
	struct fimc_is_module_enum *module;

	BUG_ON(!device);
	BUG_ON(!device->subdev_module);

	module = (struct fimc_is_module_enum *)v4l2_get_subdevdata(device->subdev_module);
	if (!module) {
		merr("module is NULL", device);
		goto p_err;
	}

	binning = min(BINNING(module->active_width, device->image.window.width),
		BINNING(module->active_height, device->image.window.height));
	/* sensor binning only support natural number */
	binning = (binning / 1000) * 1000;

p_err:
	return binning;
}

int fimc_is_sensor_g_position(struct fimc_is_device_sensor *device)
{
	return device->position;
}

int fimc_is_sensor_subdev_buffer_queue(struct fimc_is_device_sensor *device,
	enum fimc_is_sensor_subdev_id subdev_id,
	u32 index)
{
	int ret = 0;
	unsigned long flags;
	struct fimc_is_frame *frame;
	struct fimc_is_framemgr *framemgr;
	struct v4l2_subdev *subdev_csi;
	struct fimc_is_subdev *subdev = NULL;

	BUG_ON(!device);

	subdev_csi = device->subdev_csi;
	if (unlikely(!subdev_csi)) {
		merr("subdev_csi is NULL", device);
		ret = -EINVAL;
		goto p_err;
	}

	switch (subdev_id) {
	case ENTRY_SSVC0:
		subdev = &device->ssvc0;
		break;
	case ENTRY_SSVC1:
		subdev = &device->ssvc1;
		break;
	case ENTRY_SSVC2:
		subdev = &device->ssvc2;
		break;
	case ENTRY_SSVC3:
		subdev = &device->ssvc3;
		break;
	default:
		merr("subdev entry(%d) is not supported", device, subdev_id);
		ret = -EINVAL;
		goto p_err;
	}

	if (!test_bit(FIMC_IS_SUBDEV_OPEN, &subdev->state)) {
		merr("[SUB%d] is not opened", device, subdev->vid);
		ret = -EINVAL;
		goto p_err;
	}

	framemgr = GET_SUBDEV_FRAMEMGR(subdev);
	if (unlikely(!framemgr)) {
		merr("framemgr is null", device);
		ret = -EINVAL;
		goto p_err;
	}

	frame = &framemgr->frames[index];
	framemgr_e_barrier_irqs(framemgr, 0, flags);
	trans_frame(framemgr, frame, FS_REQUEST);
	framemgr_x_barrier_irqr(framemgr, 0, flags);

	ret = v4l2_subdev_call(subdev_csi, video, s_rx_buffer, (void *)frame, NULL);
	if (ret) {
		merr("[CSI] v4l2_subdev_call(s_rx_buffer) is fail(%d)", device, ret);
		goto p_err;
	}

p_err:
	return ret;
}


int fimc_is_sensor_buffer_queue(struct fimc_is_device_sensor *device,
	u32 index)
{
	int ret = 0;
	unsigned long flags;
	struct fimc_is_frame *frame;
	struct fimc_is_framemgr *framemgr;
	struct v4l2_subdev *subdev_flite, *subdev_csi;

	BUG_ON(!device);

	subdev_flite = device->subdev_flite;
	if (unlikely(!subdev_flite)) {
		merr("subdev_flite is NULL", device);
		ret = -EINVAL;
		goto p_err;
	}

	subdev_csi = device->subdev_csi;
	if (unlikely(!subdev_csi)) {
		merr("subdev_csi is NULL", device);
		ret = -EINVAL;
		goto p_err;
	}

	framemgr = GET_FRAMEMGR(device->vctx);
	if (unlikely(!framemgr)) {
		merr("framemgr is null", device);
		ret = -EINVAL;
		goto p_err;
	}

	if (index >= framemgr->num_frames) {
		merr("index(%d) is invalid", device, index);
		ret = -EINVAL;
		goto p_err;
	}

	frame = &framemgr->frames[index];
	if (unlikely(!test_bit(FRAME_MEM_INIT, &frame->mem_state))) {
		merr("frame %d is NOT init", device, index);
		ret = -EINVAL;
		goto p_err;
	}

	framemgr_e_barrier_irqs(framemgr, FMGR_IDX_2, flags);
	trans_frame(framemgr, frame, FS_REQUEST);
	framemgr_x_barrier_irqr(framemgr, FMGR_IDX_2, flags);

	ret = v4l2_subdev_call(subdev_csi, video, s_rx_buffer, NULL, NULL);
	if (ret) {
		merr("[CSI] v4l2_subdev_call(s_rx_buffer) is fail(%d)", device, ret);
		goto p_err;
	}

	ret = v4l2_subdev_call(subdev_flite, video, s_rx_buffer, NULL, NULL);
	if (ret) {
		merr("[FLITE] v4l2_subdev_call(s_rx_buffer) is fail(%d)", device, ret);
		goto p_err;
	}

p_err:
	return ret;
}

int fimc_is_sensor_buffer_finish(struct fimc_is_device_sensor *device,
	u32 index)
{
	int ret = 0;
	unsigned long flags;
	struct fimc_is_frame *frame;
	struct fimc_is_framemgr *framemgr;

	if (unlikely(!device || !test_bit(FIMC_IS_SENSOR_OPEN, &device->state))) {
		warn("sensor device was closed..(%d)", index);
		return ret;
	}

	framemgr = GET_FRAMEMGR(device->vctx);

	if (index >= framemgr->num_frames) {
		err("index(%d) is invalid", index);
		ret = -EINVAL;
		goto exit;
	}

	frame = &framemgr->frames[index];

	framemgr_e_barrier_irqs(framemgr, FMGR_IDX_3, flags);

	if (frame->state == FS_COMPLETE) {
		trans_frame(framemgr, frame, FS_FREE);
#ifndef CONFIG_CAMERA_USE_SOC_SENSOR
		if (!frame->shot->dm.request.frameCount)
			err("request.frameCount is 0\n");
		frame->shot_ext->free_cnt = framemgr->queued_count[FS_FREE];
		frame->shot_ext->request_cnt = framemgr->queued_count[FS_REQUEST];
		frame->shot_ext->process_cnt = framemgr->queued_count[FS_PROCESS];
		frame->shot_ext->complete_cnt = framemgr->queued_count[FS_COMPLETE];
#endif
	} else {
		err("frame(%d) is not com state(%d)", index, frame->state);
		frame_manager_print_queues(framemgr);
		ret = -EINVAL;
	}

	framemgr_x_barrier_irqr(framemgr, FMGR_IDX_3, flags);

exit:
	return ret;
}

static int fimc_is_sensor_back_start(void *qdevice,
	struct fimc_is_queue *queue)
{
	int ret = 0;
	int enable;
	struct fimc_is_device_sensor *device = qdevice;
	struct v4l2_subdev *subdev_flite;
	struct fimc_is_device_flite *flite;

	BUG_ON(!device);
	BUG_ON(!device->subdev_flite);

	subdev_flite = device->subdev_flite;
	enable = FLITE_ENABLE_FLAG;

	if (test_bit(FIMC_IS_SENSOR_BACK_START, &device->state)) {
		err("already back start");
		ret = -EINVAL;
		goto p_err;
	}

	ret = fimc_is_hw_camif_cfg((void *)device);
	if (ret) {
		merr("hw_camif_cfg is error(%d)", device, ret);
		ret = -EINVAL;
		goto p_err;
	}

	flite = (struct fimc_is_device_flite *)v4l2_get_subdevdata(subdev_flite);
	if (!flite) {
		merr("flite is NULL", device);
		ret = -EINVAL;
		goto p_err;
	}

	/* if sensor is driving mode, skip finding sensor mode */
	if (test_bit(FIMC_IS_SENSOR_DRIVING, &device->state)) {
		device->cfg = NULL;
	} else {
		device->cfg = fimc_is_sensor_g_mode(device);
		if (!device->cfg) {
			merr("sensor cfg is invalid", device);
			ret = -EINVAL;
			goto p_err;
		}
		set_bit(FIMC_IS_SENSOR_S_CONFIG, &device->state);
	}

	ret = v4l2_subdev_call(subdev_flite, video, s_stream, enable);
	if (ret) {
		merr("v4l2_flite_call(s_stream) is fail(%d)", device, ret);
		goto p_err;
	}

	set_bit(FIMC_IS_SENSOR_BACK_START, &device->state);

p_err:
	minfo("[SEN:D] %s(%dx%d, %d)\n", device, __func__,
		device->image.window.width, device->image.window.height, ret);
	return ret;
}

static int fimc_is_sensor_back_stop(void *qdevice,
	struct fimc_is_queue *queue)
{
	int ret = 0;
	int enable;
	struct fimc_is_device_sensor *device = qdevice;
	struct v4l2_subdev *subdev_flite;

	BUG_ON(!device);
	BUG_ON(!device->subdev_flite);

	enable = 0;
	subdev_flite = device->subdev_flite;

	if (!test_bit(FIMC_IS_SENSOR_BACK_START, &device->state)) {
		mwarn("already back stop", device);
		goto p_err;
	}

	if (test_bit(FIMC_IS_SENSOR_BACK_NOWAIT_STOP, &device->state)) {
		mwarn("fimc_is_flite_stop, no waiting...", device);
		enable = FLITE_NOWAIT_FLAG << FLITE_NOWAIT_SHIFT;
	}

	ret = v4l2_subdev_call(subdev_flite, video, s_stream, enable);
	if (ret) {
		merr("v4l2_flite_call(s_stream) is fail(%d)", device, ret);
		goto p_err;
	}

	clear_bit(FIMC_IS_SENSOR_S_CONFIG, &device->state);
	clear_bit(FIMC_IS_SENSOR_BACK_START, &device->state);

p_err:
	minfo("[BAK:D] %s():%d\n", device, __func__, ret);
	return ret;
}

int fimc_is_sensor_front_start(struct fimc_is_device_sensor *device,
	u32 instant_cnt,
	u32 nonblock)
{
	int ret = 0;
	struct v4l2_subdev *subdev_module;
	struct v4l2_subdev *subdev_csi;
	struct fimc_is_module_enum *module;
	struct fimc_is_core *core;

	BUG_ON(!device);
	BUG_ON(!device->pdata);
	BUG_ON(!device->subdev_csi);
	BUG_ON(!device->private_data);

	if (test_bit(FIMC_IS_SENSOR_FRONT_START, &device->state)) {
		merr("already front start", device);
		ret = -EINVAL;
		goto p_err;
	}

	memset(device->timestamp, 0x0, FIMC_IS_TIMESTAMP_HASH_KEY * sizeof(u64));
	memset(device->timestampboot, 0x0, FIMC_IS_TIMESTAMP_HASH_KEY * sizeof(u64));
	device->instant_cnt = instant_cnt;
	subdev_csi = device->subdev_csi;
	subdev_module = device->subdev_module;
	if (!subdev_module) {
		merr("subdev_module is NULL", device);
		ret = -EINVAL;
		goto p_err;
	}

	module = (struct fimc_is_module_enum *)v4l2_get_subdevdata(subdev_module);
	if (!module) {
		merr("module is NULL", device);
		ret = -EINVAL;
		goto p_err;
	}

	core = device->private_data;
#ifndef ENABLE_IS_CORE
	/* Actuator Init because actuator init use cal data */
	ret = v4l2_subdev_call(device->subdev_module, core, ioctl, V4L2_CID_SENSOR_NOTIFY_ACTUATOR_INIT, 0);
	if (ret)
		warn("Actuator init fail after first init done\n");

#ifdef CONFIG_OIS_USE
	/* OIS Init because ois init use cal data */
	fimc_is_ois_init(core);
#endif
#endif

	ret = v4l2_subdev_call(subdev_csi, video, s_stream, IS_ENABLE_STREAM);
	if (ret) {
		merr("v4l2_csi_call(s_stream) is fail(%d)", device, ret);
		goto p_err;
	}

	mdbgd_sensor("%s(snesor id : %d, csi ch : %d, size : %d x %d)\n", device,
		__func__,
		module->sensor_id,
		device->pdata->csi_ch,
		device->image.window.width,
		device->image.window.height);

	if (nonblock) {
		schedule_work(&device->instant_work);
	} else {
		fimc_is_sensor_instanton(&device->instant_work);
		if (device->instant_ret) {
			merr("fimc_is_sensor_instanton is fail(%d)", device, device->instant_ret);
			ret = device->instant_ret;
			goto p_err;
		}
	}

p_err:
	return ret;
}

int fimc_is_sensor_front_stop(struct fimc_is_device_sensor *device)
{
	int ret = 0;
	struct v4l2_subdev *subdev_csi;

	BUG_ON(!device);

	if (!test_bit(FIMC_IS_SENSOR_FRONT_START, &device->state)) {
		mwarn("already front stop", device);
		goto p_err;
	}

	subdev_csi = device->subdev_csi;

	ret = fimc_is_sensor_stop(device);
	if (ret)
		merr("sensor stream off is failed(%d)\n", device, ret);

	ret = v4l2_subdev_call(subdev_csi, video, s_stream, IS_DISABLE_STREAM);
	if (ret)
		merr("v4l2_csi_call(s_stream) is fail(%d)", device, ret);

	set_bit(FIMC_IS_SENSOR_BACK_NOWAIT_STOP, &device->state);
	clear_bit(FIMC_IS_SENSOR_FRONT_START, &device->state);

#ifdef ENABLE_DTP
	if (device->dtp_check) {
		device->dtp_check = false;
		del_timer(&device->dtp_timer);
	}
#endif

p_err:
	minfo("[FRT:D] %s():%d\n", device, __func__, ret);
	return ret;
}
#ifndef ENABLE_IS_CORE
#ifdef CONFIG_OIS_USE
int fimc_is_sensor_ois_shift(struct fimc_is_device_sensor *device, u32 position)
{
	int ret = 0;
	struct fimc_is_core *core;

	BUG_ON(!device);
	BUG_ON(!device->private_data);

	core = device->private_data;
	ret = fimc_is_ois_shift_compensation(core, position);

	return ret;
}
#endif
#endif

const struct fimc_is_queue_ops fimc_is_sensor_ops = {
	.start_streaming	= fimc_is_sensor_back_start,
	.stop_streaming		= fimc_is_sensor_back_stop,
	.s_format		= fimc_is_sensor_s_format,
	.request_bufs		= fimc_is_sensor_reqbufs
};

static int fimc_is_sensor_suspend(struct device *dev)
{
	int ret = 0;

	info("%s\n", __func__);

	return ret;
}

static int fimc_is_sensor_resume(struct device *dev)
{
	int ret = 0;

	info("%s\n", __func__);

	return ret;
}

int fimc_is_sensor_runtime_suspend(struct device *dev)
{
	int ret = 0;
	struct platform_device *pdev = to_platform_device(dev);
	struct fimc_is_device_sensor *device;
	struct fimc_is_module_enum *module;
	struct v4l2_subdev *subdev_csi;

	device = NULL;
	module = NULL;

	ret = fimc_is_sensor_g_device(pdev, &device);
	if (ret) {
		err("fimc_is_sensor_g_device is fail(%d)", ret);
		return -EINVAL;
	}

	ret = fimc_is_sensor_runtime_suspend_pre(dev);
	if (ret)
		err("fimc_is_sensor_runtime_suspend_pre is fail(%d)", ret);

	CALL_MEMOP(&device->mem, suspend, device->mem.default_ctx);

	subdev_csi = device->subdev_csi;
	if (!subdev_csi)
		mwarn("subdev_csi is NULL", device);

	ret = v4l2_subdev_call(subdev_csi, core, s_power, 0);
	if (ret)
		mwarn("v4l2_csi_call(s_power) is fail(%d)", device, ret);

	ret = fimc_is_sensor_g_module(device, &module);
	if (ret) {
		merr("fimc_is_sensor_g_module is fail(%d)", device, ret);
		goto p_err;
	}

	ret = fimc_is_sensor_gpio_off(device);
	if (ret)
		mwarn("fimc_is_sensor_gpio_off is fail(%d)", device, ret);

	ret = fimc_is_sensor_iclk_off(device);
	if (ret)
		mwarn("fimc_is_sensor_iclk_off is fail(%d)", device, ret);

	ret = fimc_is_sensor_mclk_off(device, device->pdata->scenario, module->pdata->mclk_ch);
	if (ret)
		mwarn("fimc_is_sensor_mclk_off is fail(%d)", device, ret);

	v4l2_device_unregister_subdev(device->subdev_module);
	device->subdev_module = NULL;

p_err:

#if defined(CONFIG_PM_DEVFREQ)
	if (test_bit(FIMC_IS_SENSOR_DRIVING, &device->state)) {
		pm_qos_remove_request(&exynos_sensor_qos_cam);
		pm_qos_remove_request(&exynos_sensor_qos_mem);
	}
#endif

	info("[SEN:D:%d] %s():%d\n", device->instance, __func__, ret);
	return 0;
}

int fimc_is_sensor_runtime_resume(struct device *dev)
{
	int ret = 0;
	struct platform_device *pdev = to_platform_device(dev);
	struct fimc_is_device_sensor *device;
	struct v4l2_subdev *subdev_csi;

	device = NULL;

	ret = fimc_is_sensor_g_device(pdev, &device);
	if (ret) {
		err("fimc_is_sensor_g_device is fail(%d)", ret);
		return -EINVAL;
	}

	subdev_csi = device->subdev_csi;
	if (!subdev_csi) {
		merr("subdev_csi is NULL", device);
		ret = -EINVAL;
		goto p_err;
	}

	ret = fimc_is_sensor_runtime_resume_pre(dev);
	if (ret) {
		merr("fimc_is_sensor_runtime_resume_pre is fail(%d)", device, ret);
		goto p_err;
	}

	ret = v4l2_subdev_call(subdev_csi, core, s_power, 1);
	if (ret) {
		merr("v4l2_csi_call(s_power) is fail(%d)", device, ret);
		goto p_err;
	}

	/* configuration clock control */
	ret = fimc_is_sensor_iclk_on(device);
	if (ret) {
		merr("fimc_is_sensor_iclk_on is fail(%d)", device, ret);
		goto p_err;
	}

	CALL_MEMOP(&device->mem, resume, device->mem.default_ctx);

p_err:
	info("[SEN:D:%d] %s():%d\n", device->instance, __func__, ret);
	return ret;
}

static const struct dev_pm_ops fimc_is_sensor_pm_ops = {
	.suspend		= fimc_is_sensor_suspend,
	.resume			= fimc_is_sensor_resume,
	.runtime_suspend	= fimc_is_sensor_runtime_suspend,
	.runtime_resume		= fimc_is_sensor_runtime_resume,
};

#ifdef CONFIG_OF
static const struct of_device_id exynos_fimc_is_sensor_match[] = {
	{
		.compatible = "samsung,exynos5-fimc-is-sensor",
	},
	{},
};
MODULE_DEVICE_TABLE(of, exynos_fimc_is_sensor_match);

static struct platform_driver fimc_is_sensor_driver = {
	.probe		= fimc_is_sensor_probe,
	.remove 	= fimc_is_sensor_remove,
	.driver = {
		.name	= FIMC_IS_SENSOR_DEV_NAME,
		.owner	= THIS_MODULE,
		.pm	= &fimc_is_sensor_pm_ops,
		.of_match_table = exynos_fimc_is_sensor_match,
	}
};

#else
static struct platform_device_id fimc_is_sensor_driver_ids[] = {
	{
		.name		= FIMC_IS_SENSOR_DEV_NAME,
		.driver_data	= 0,
	},
	{},
};
MODULE_DEVICE_TABLE(platform, fimc_is_sensor_driver_ids);

static struct platform_driver fimc_is_sensor_driver = {
	.probe		= fimc_is_sensor_probe,
	.remove 	= __devexit_p(fimc_is_sensor_remove),
	.id_table	= fimc_is_sensor_driver_ids,
	.driver	  = {
		.name	= FIMC_IS_SENSOR_DEV_NAME,
		.owner	= THIS_MODULE,
		.pm	= &fimc_is_sensor_pm_ops,
	}
};
#endif

static int __init fimc_is_sensor_init(void)
{
	int ret = platform_driver_register(&fimc_is_sensor_driver);
	if (ret)
		err("platform_driver_register failed: %d\n", ret);

	return ret;
}
late_initcall(fimc_is_sensor_init);

static void __exit fimc_is_sensor_exit(void)
{
	platform_driver_unregister(&fimc_is_sensor_driver);
}
module_exit(fimc_is_sensor_exit);

MODULE_AUTHOR("Gilyeon lim<kilyeon.im@samsung.com>");
MODULE_DESCRIPTION("Exynos FIMC_IS_SENSOR driver");
MODULE_LICENSE("GPL");
