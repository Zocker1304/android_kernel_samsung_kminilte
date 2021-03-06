/*
 * S5P/EXYNOS4 SoC series camera host interface media device driver
 *
 * Copyright (C) 2011 Samsung Electronics Co., Ltd.
 * Contact: Sylwester Nawrocki, <s.nawrocki@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation, either version 2 of the License,
 * or (at your option) any later version.
 */

#include <linux/bug.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <media/v4l2-ctrls.h>
#include <media/media-device.h>
#include <media/s5p_fimc.h>
#include <mach/videonode-exynos4.h>

#include "fimc-core.h"
#include "fimc-is.h"
#include "fimc-isp.h"
#include "fimc-lite.h"
#include "fimc-mdevice.h"
#include "mipi-csis.h"

static int __fimc_md_set_camclk(struct fimc_md *fmd,
				struct fimc_sensor_info *s_info,
				bool on);
/**
 * fimc_pipeline_prepare - update pipeline information with subdevice pointers
 * @fimc: fimc device terminating the pipeline
 *
 * Caller holds the graph mutex.
 */
static void fimc_pipeline_prepare(struct fimc_pipeline *p,
				  struct media_entity *me)
{
	struct media_pad *pad = &me->pads[0];
	struct v4l2_subdev *sd;
	int i;

	for (i = 0; i < IDX_MAX; i++)
		p->subdevs[i] = NULL;

	while (1) {
		if (!(pad->flags & MEDIA_PAD_FL_SINK))
			break;

		/* source pad */
		pad = media_entity_remote_source(pad);
		if (pad == NULL ||
		    media_entity_type(pad->entity) != MEDIA_ENT_T_V4L2_SUBDEV)
			break;

		sd = media_entity_to_v4l2_subdev(pad->entity);

		switch (sd->grp_id) {
		case FIMC_IS_SENSOR_GROUP_ID:
		case SENSOR_GROUP_ID:
			p->subdevs[IDX_SENSOR] = sd;
			break;
		case CSIS_GROUP_ID:
			p->subdevs[IDX_CSIS] = sd;
			break;
		case FLITE_GROUP_ID:
			p->subdevs[IDX_FLITE] = sd;
			break;
		case FIMC_GROUP_ID:
			/* No need to control FIMC subdev through subdev ops */
			break;
		case FIMC_IS_GROUP_ID:
			p->subdevs[IDX_IS_ISP] = sd;
			break;
		default:
			pr_warn("%s: Unknown subdev grp_id: %#x\n",
				__func__, sd->grp_id);
		}
		/* sink pad */
		pad = &sd->entity.pads[0];
	}
}

/**
 * __subdev_set_power - change power state of a single subdev
 * @sd: subdevice to change power state for
 * @on: 1 to enable power or 0 to disable
 *
 * Return result of s_power subdev operation or -ENXIO if sd argument
 * is NULL. Return 0 if the subdevice does not implement s_power.
 */
static int __subdev_set_power(struct v4l2_subdev *sd, int on)
{
	int *use_count;
	int ret;

	if (sd == NULL)
		return -ENXIO;

	use_count = &sd->entity.use_count;
	if (on && (*use_count)++ > 0)
		return 0;
	else if (!on && (*use_count == 0 || --(*use_count) > 0))
		return 0;
	ret = v4l2_subdev_call(sd, core, s_power, on);

	return ret != -ENOIOCTLCMD ? ret : 0;
}

/**
 * fimc_pipeline_s_power - change power state of all pipeline subdevs
 * @fimc: fimc device terminating the pipeline
 * @state: true to power on, false to power off
 *
 * Needs to be called with the graph mutex held.
 */
static int fimc_pipeline_s_power(struct fimc_pipeline *p, bool state)
{
	unsigned int i;
	int ret;

	if (p->subdevs[IDX_SENSOR] == NULL)
		return -ENXIO;

	for (i = 0; i < IDX_MAX; i++) {
		unsigned int idx = state ? (IDX_MAX - 1) - i : i;

		ret = __subdev_set_power(p->subdevs[idx], state);
		if (ret < 0 && ret != -ENXIO)
			return ret;
	}

	return 0;
}

/**
 * __fimc_pipeline_open - update the pipeline information, enable power
 *                        of all pipeline subdevs and the sensor clock
 * @me: media entity to start graph walk with
 * @prep: true to acquire sensor (and csis) subdevs
 *
 * This function must be called with the graph mutex held.
 */
static int __fimc_pipeline_open(struct fimc_pipeline *p,
				struct media_entity *me, bool prep)
{
	int ret;

	if (prep)
		fimc_pipeline_prepare(p, me);

	if (p->subdevs[IDX_SENSOR] == NULL)
		return -EINVAL;

	ret = fimc_md_set_camclk(p->subdevs[IDX_SENSOR], true);
	if (ret)
		return ret;

	return fimc_pipeline_s_power(p, 1);
}

static int fimc_pipeline_open(struct fimc_pipeline *p,
			      struct media_entity *me, bool prep)
{
	int ret;

	mutex_lock(&me->parent->graph_mutex);
	ret =  __fimc_pipeline_open(p, me, prep);
	mutex_unlock(&me->parent->graph_mutex);

	return ret;
}

/**
 * __fimc_pipeline_close - disable the sensor clock and pipeline power
 * @fimc: fimc device terminating the pipeline
 *
 * Disable power of all subdevs in the pipeline and turn off the external
 * sensor clock.
 * Called with the graph mutex held.
 */
static int __fimc_pipeline_close(struct fimc_pipeline *p)
{
	int ret = 0;

	if (p->subdevs[IDX_SENSOR]) {
		ret = fimc_pipeline_s_power(p, 0);
		fimc_md_set_camclk(p->subdevs[IDX_SENSOR], false);
	}
	return ret == -ENXIO ? 0 : ret;
}

static int fimc_pipeline_close(struct fimc_pipeline *p)
{
	struct media_entity *me;
	int ret;

	if (!p || !p->subdevs[IDX_SENSOR])
		return -EINVAL;

	me = &p->subdevs[IDX_SENSOR]->entity;
	mutex_lock(&me->parent->graph_mutex);
	ret = __fimc_pipeline_close(p);
	mutex_unlock(&me->parent->graph_mutex);

	return ret;
}

/**
 * fimc_pipeline_s_stream - invoke s_stream on pipeline subdevs
 * @pipeline: video pipeline structure
 * @on: passed as the s_stream call argument
 */
int fimc_pipeline_s_stream(struct fimc_pipeline *p, bool on)
{
	int ret = 0;

	if (p->subdevs[IDX_SENSOR] == NULL)
		return -ENODEV;
	if (on) {
		/* 1. MIPI SUBDEV */
		if (p->subdevs[IDX_CSIS])
			ret = v4l2_subdev_call(p->subdevs[IDX_CSIS],
							video, s_stream, on);
		if (ret < 0 && ret != -ENOIOCTLCMD && ret != -ENODEV)
			return ret;
		/* 2. FIMC-LITE SUBDEV */
		if (p->subdevs[IDX_FLITE])
			ret = v4l2_subdev_call(p->subdevs[IDX_FLITE],
							video, s_stream, on);
		if (ret < 0 && ret != -ENOIOCTLCMD && ret != -ENODEV)
			return ret;
		/* 3. FIMC SUBDEV */
		if (p->subdevs[IDX_FIMC])
			ret = v4l2_subdev_call(p->subdevs[IDX_FIMC],
							video, s_stream, on);
		if (ret < 0 && ret != -ENOIOCTLCMD && ret != -ENODEV)
			return ret;
		/* 4. SENSOR SUBDEV */
		if (p->subdevs[IDX_SENSOR])
			ret = v4l2_subdev_call(p->subdevs[IDX_SENSOR],
							video, s_stream, on);
		if (ret < 0 && ret != -ENOIOCTLCMD && ret != -ENODEV)
			return ret;
		/* 5. IS-ISP SUBDEV */;
		if (p->subdevs[IDX_IS_ISP])
			ret = v4l2_subdev_call(p->subdevs[IDX_IS_ISP],
							video, s_stream, on);
		if (ret < 0 && ret != -ENOIOCTLCMD && ret != -ENODEV)
			return ret;
	} else {
		/* 1. FIMC SUBDEV */
		if (p->subdevs[IDX_FIMC])
			ret = v4l2_subdev_call(p->subdevs[IDX_FIMC],
							video, s_stream, on);
		if (ret < 0 && ret != -ENOIOCTLCMD && ret != -ENODEV)
			return ret;
		/* 2. SENSOR SUBDEV */
		if (p->subdevs[IDX_SENSOR])
			ret = v4l2_subdev_call(p->subdevs[IDX_SENSOR],
							video, s_stream, on);
		if (ret < 0 && ret != -ENOIOCTLCMD && ret != -ENODEV)
			return ret;
		/* 3. IS-ISP SUBDEV */;
		if (p->subdevs[IDX_IS_ISP])
			ret = v4l2_subdev_call(p->subdevs[IDX_IS_ISP],
							video, s_stream, on);
		if (ret < 0 && ret != -ENOIOCTLCMD && ret != -ENODEV)
			return ret;
		/* 4. MIPI SUBDEV */
		if (p->subdevs[IDX_CSIS])
			ret = v4l2_subdev_call(p->subdevs[IDX_CSIS],
							video, s_stream, on);
		if (ret < 0 && ret != -ENOIOCTLCMD && ret != -ENODEV)
			return ret;
		/* 5. FIMC-LITE SUBDEV */
		if (p->subdevs[IDX_FLITE])
			ret = v4l2_subdev_call(p->subdevs[IDX_FLITE],
							video, s_stream, on);
		if (ret < 0 && ret != -ENOIOCTLCMD && ret != -ENODEV)
			return ret;
	}

	return 0;
}

/* Media pipeline operations for the FIMC/FIMC-LITE video device driver */
static const struct fimc_pipeline_ops fimc_pipeline_ops = {
	.open		= fimc_pipeline_open,
	.close		= fimc_pipeline_close,
	.set_stream	= fimc_pipeline_s_stream,
};

/*
 * Sensor subdevice helper functions
 */
static struct v4l2_subdev *fimc_md_register_is_sensor(struct fimc_md *fmd,
				struct fimc_sensor_info *s_info, int index)
{
	struct v4l2_subdev *sd = NULL;
	const char *sd_name;

	if (fmd->fimc_is == NULL)
		return NULL;

	sd = &fmd->fimc_is->sensor[index].subdev;
	/* Override FIMC-IS subdev name */
	sd_name = fimc_is_get_sensor_name(s_info->pdata.is_sensor_info);
	pr_info("fimc_md_register_is_sensor : sd_name (%s)\n", sd_name);
	if (sd_name == NULL) {
		v4l2_err(sd, "Null FIMC-IS sensor name\n");
		return NULL;
	}
	strlcpy(sd->name, sd_name, sizeof(sd->name));
	v4l2_set_subdev_hostdata(sd, s_info);
	sd->grp_id = FIMC_IS_SENSOR_GROUP_ID;

	v4l2_info(&fmd->v4l2_dev, "Registered logical sensor subdevice %s\n",
			sd->name);
	return sd;
}

static struct v4l2_subdev *fimc_md_register_sensor(struct fimc_md *fmd,
				   struct fimc_sensor_info *s_info)
{
	struct i2c_adapter *adapter;
	struct v4l2_subdev *sd = NULL;

	if (!s_info || !fmd)
		return NULL;

	adapter = i2c_get_adapter(s_info->pdata.i2c_bus_num);
	if (!adapter) {
		v4l2_warn(&fmd->v4l2_dev,
			  "Failed to get I2C adapter %d, deferring probe\n",
			  s_info->pdata.i2c_bus_num);
		return ERR_PTR(-EPROBE_DEFER);
	}
	sd = v4l2_i2c_new_subdev_board(&fmd->v4l2_dev, adapter,
				       s_info->pdata.board_info, NULL);
	if (IS_ERR_OR_NULL(sd)) {
		i2c_put_adapter(adapter);
		v4l2_warn(&fmd->v4l2_dev,
			  "Failed to acquire subdev %s, deferring probe\n",
			  s_info->pdata.board_info->type);
		return ERR_PTR(-EPROBE_DEFER);
	}
	v4l2_set_subdev_hostdata(sd, s_info);
	sd->grp_id = SENSOR_GROUP_ID;

	v4l2_info(&fmd->v4l2_dev, "Registered sensor subdevice %s\n",
		  s_info->pdata.board_info->type);
	return sd;
}

static void fimc_md_unregister_sensor(struct v4l2_subdev *sd)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct i2c_adapter *adapter;

	if (!client)
		return;
	v4l2_device_unregister_subdev(sd);
	adapter = client->adapter;
	i2c_unregister_device(client);
	if (adapter)
		i2c_put_adapter(adapter);
}

static int fimc_md_register_sensor_entities(struct fimc_md *fmd)
{
	struct s5p_platform_fimc *pdata = fmd->pdev->dev.platform_data;
	struct fimc_dev *fd = NULL;
	int num_clients, ret, i, is_sensor_index;

	/*
	 * Runtime resume one of the FIMC entities to make sure
	 * the sclk_cam clocks are not globally disabled.
	 */
	for (i = 0; !fd && i < ARRAY_SIZE(fmd->fimc); i++)
		if (fmd->fimc[i])
			fd = fmd->fimc[i];
	if (!fd)
		return -ENXIO;
	ret = pm_runtime_get_sync(&fd->pdev->dev);
	if (ret < 0)
		return ret;

	WARN_ON(pdata->num_clients > ARRAY_SIZE(fmd->sensor));
	num_clients = min_t(u32, pdata->num_clients, ARRAY_SIZE(fmd->sensor));

	fmd->num_sensors = num_clients;
	is_sensor_index = 0;
	for (i = 0; i < num_clients; i++) {
		struct v4l2_subdev *sd;

		fmd->sensor[i].pdata = pdata->isp_info[i];
		ret = __fimc_md_set_camclk(fmd, &fmd->sensor[i], true);
		if (ret)
			break;
		if (!pdata->isp_info[i].use_isp) {
			sd = fimc_md_register_sensor(fmd, &fmd->sensor[i]);
		} else {
			sd = fimc_md_register_is_sensor(fmd, &fmd->sensor[i],
							is_sensor_index);
			is_sensor_index++;
		}
		ret = __fimc_md_set_camclk(fmd, &fmd->sensor[i], false);

		if (!IS_ERR(sd)) {
			fmd->sensor[i].subdev = sd;
		} else {
			fmd->sensor[i].subdev = NULL;
			ret = PTR_ERR(sd);
			break;
		}
		if (ret)
			break;
	}
	pm_runtime_put(&fd->pdev->dev);
	return ret;
}

/*
 * MIPI CSIS and FIMC platform devices registration.
 */
static int fimc_register_callback(struct device *dev, void *p)
{
	struct fimc_dev *fimc = dev_get_drvdata(dev);
	struct v4l2_subdev *sd = &fimc->vid_cap.subdev;
	struct fimc_md *fmd = p;
	int ret = 0;

	if (!fimc || !fimc->pdev)
		return 0;

	if (fimc->pdev->id < 0 || fimc->pdev->id >= FIMC_MAX_DEVS)
		return 0;

	fimc->pipeline_ops = &fimc_pipeline_ops;
	fmd->fimc[fimc->pdev->id] = fimc;
	sd->grp_id = FIMC_GROUP_ID;

	ret = v4l2_device_register_subdev(&fmd->v4l2_dev, sd);
	if (ret) {
		v4l2_err(&fmd->v4l2_dev, "Failed to register FIMC.%d (%d)\n",
			 fimc->id, ret);
	}

	return ret;
}

static int fimc_lite_register_callback(struct device *dev, void *p)
{
	struct fimc_lite *fimc = dev_get_drvdata(dev);
	struct v4l2_subdev *sd = &fimc->subdev;
	struct fimc_md *fmd = p;
	int ret;

	if (fimc == NULL)
		return 0;

	if (fimc->index >= FIMC_LITE_MAX_DEVS)
		return 0;

	fimc->pipeline_ops = &fimc_pipeline_ops;
	fmd->fimc_lite[fimc->index] = fimc;
	sd->grp_id = FLITE_GROUP_ID;

	ret = v4l2_device_register_subdev(&fmd->v4l2_dev, sd);
	if (ret) {
		v4l2_err(&fmd->v4l2_dev,
			 "Failed to register FIMC-LITE.%d (%d)\n",
			 fimc->index, ret);
	}
	return ret;
}

static int csis_register_callback(struct device *dev, void *p)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct platform_device *pdev;
	struct fimc_md *fmd = p;
	int id, ret;

	if (!sd)
		return 0;
	pdev = v4l2_get_subdevdata(sd);
	if (!pdev || pdev->id < 0 || pdev->id >= CSIS_MAX_ENTITIES)
		return 0;
	v4l2_info(sd, "csis%d sd: %s\n", pdev->id, sd->name);

	id = pdev->id < 0 ? 0 : pdev->id;
	fmd->csis[id].sd = sd;
	sd->grp_id = CSIS_GROUP_ID;
	ret = v4l2_device_register_subdev(&fmd->v4l2_dev, sd);
	if (ret)
		v4l2_err(&fmd->v4l2_dev,
			 "Failed to register CSIS subdevice: %d\n", ret);
	return ret;
}

#if defined(CONFIG_VIDEO_EXYNOS4_FIMC_IS)
static int fimc_is_register_callback(struct device *dev, void *p)
{
	struct fimc_is *is = dev_get_drvdata(dev);
	struct v4l2_subdev *sd;
	struct fimc_md *fmd = p;
	int i, ret;

	if (is == NULL)
		return -ENXIO;

	for (i = 0; i < is->pdata->num_sensors; i++) {
		sd = &is->sensor[i].subdev;
		ret = v4l2_device_register_subdev(&fmd->v4l2_dev, sd);
		if (ret) {
			v4l2_err(&fmd->v4l2_dev,
				 "Failed to register FIMC-IS-SENSOR (%d)\n",
				 ret);
			return ret;
		}
	}

	sd = &is->isp.subdev;
	ret = v4l2_device_register_subdev(&fmd->v4l2_dev, sd);
	if (ret) {
		v4l2_err(&fmd->v4l2_dev,
			 "Failed to register FIMC-ISP (%d)\n", ret);
		return ret;
	}

	pr_info("v4l2_device_register_subdev : FIMC-IS (%d)\n", is->pdev->id);
	fmd->fimc_is = is;
	return 0;
}
#else
static int fimc_is_register_callback(struct device *dev, void *p)
{
	return -ENXIO;
}
#endif

/**
 * fimc_md_register_platform_entities - register FIMC and CSIS media entities
 */
static int fimc_md_register_platform_entities(struct fimc_md *fmd)
{
	struct s5p_platform_fimc *pdata = fmd->pdev->dev.platform_data;
	struct device_driver *driver;
	int ret, i, tmp;

	driver = driver_find(FIMC_MODULE_NAME, &platform_bus_type);
	if (!driver) {
		v4l2_warn(&fmd->v4l2_dev,
			 "%s driver not found, deffering probe\n",
			 FIMC_MODULE_NAME);
		return -EPROBE_DEFER;
	}

	ret = driver_for_each_device(driver, NULL, fmd,
				     fimc_register_callback);
	if (ret)
		return ret;

	driver = driver_find(FIMC_LITE_DRV_NAME, &platform_bus_type);
	if (driver && try_module_get(driver->owner)) {
		ret = driver_for_each_device(driver, NULL, fmd,
					     fimc_lite_register_callback);
		if (ret)
			return ret;
		module_put(driver->owner);
	}

	tmp = 0;
	if (pdata == NULL)
		return 0;
	for (i = 0; i < pdata->num_clients; i++) {
		if (pdata->isp_info[i].use_isp) {
			tmp++;
			break;
		}
	}

	if (tmp > 0) {
		driver = driver_find(FIMC_IS_DRV_NAME, &platform_bus_type);
		if (!driver)
			return -EPROBE_DEFER;

		ret = driver_for_each_device(driver, NULL, fmd,
					     fimc_is_register_callback);
		if (ret)
			return -EPROBE_DEFER;
	}
	/*
	 * Check if there is any sensor on the MIPI-CSI2 bus and
	 * if not skip the s5p-csis module loading.
	 */
	for (i = 0; i < pdata->num_clients; i++) {
		if ((pdata->isp_info[i].bus_type == FIMC_MIPI_CSI2) ||
			(pdata->isp_info[i].bus_type == FIMC_IS_WB)) {
			ret = 1;
			break;
		}
	}
	if (!ret)
		return 0;

	driver = driver_find(CSIS_DRIVER_NAME, &platform_bus_type);
	if (!driver || !try_module_get(driver->owner)) {
		v4l2_warn(&fmd->v4l2_dev,
			 "%s driver not found, deffering probe\n",
			 CSIS_DRIVER_NAME);
		return -EPROBE_DEFER;
	}

	return driver_for_each_device(driver, NULL, fmd,
				      csis_register_callback);
}

static void fimc_md_unregister_entities(struct fimc_md *fmd)
{
	int i;

	for (i = 0; i < FIMC_MAX_DEVS; i++) {
		if (fmd->fimc[i] == NULL)
			continue;
		v4l2_device_unregister_subdev(&fmd->fimc[i]->vid_cap.subdev);
		fmd->fimc[i]->pipeline_ops = NULL;
		fmd->fimc[i] = NULL;
	}
	for (i = 0; i < FIMC_LITE_MAX_DEVS; i++) {
		if (fmd->fimc_lite[i] == NULL)
			continue;
		v4l2_device_unregister_subdev(&fmd->fimc_lite[i]->subdev);
		fmd->fimc[i]->pipeline_ops = NULL;
		fmd->fimc_lite[i] = NULL;
	}
	for (i = 0; i < CSIS_MAX_ENTITIES; i++) {
		if (fmd->csis[i].sd == NULL)
			continue;
		v4l2_device_unregister_subdev(fmd->csis[i].sd);
		module_put(fmd->csis[i].sd->owner);
		fmd->csis[i].sd = NULL;
	}
	for (i = 0; i < fmd->num_sensors; i++) {
		if (fmd->sensor[i].subdev == NULL)
			continue;
		fimc_md_unregister_sensor(fmd->sensor[i].subdev);
		fmd->sensor[i].subdev = NULL;
	}
}

/**
 * fimc_md_create_links - create default links between registered entities
 *
 * Parallel interface sensor entities are connected directly to FIMC capture
 * entities. The sensors using MIPI CSIS bus are connected through immutable
 * link with CSI receiver entity specified by mux_id. Any registered CSIS
 * entity has a link to each registered FIMC capture entity. Enabled links
 * are created by default between each subsequent registered sensor and
 * subsequent FIMC capture entity. The number of default active links is
 * determined by the number of available sensors or FIMC entities,
 * whichever is less.
 */
static int fimc_md_create_links(struct fimc_md *fmd)
{
	struct v4l2_subdev *sensor, *csis;
	struct fimc_sensor_info *s_info;
	struct s5p_fimc_isp_info *pdata;
	struct media_entity *source, *sink;
	int i, j, ret = 0;
	u32 flags = 0;

	for (j = 0; j < fmd->num_sensors; j++) {
		if (fmd->sensor[j].subdev == NULL) {
			v4l2_warn(&fmd->v4l2_dev,
				"fmd->sensor[%d].subdev = NULL!!\n", j);
			continue;
		}

		sensor = fmd->sensor[j].subdev;
		s_info = v4l2_get_subdev_hostdata(sensor);
		if (!s_info) {
			v4l2_warn(&fmd->v4l2_dev,
				"(%d) fimc_sensor_info = NULL!!\n", j);
			continue;
		}
		pdata = &s_info->pdata;
		switch (pdata->bus_type) {
		case FIMC_ITU_601...FIMC_ITU_656:
			/* TODO : add ITU configuration */
			break;
		case FIMC_MIPI_CSI2:
			/* 1. create link beween sensor and mipi-csi */
			if (WARN(pdata->mux_id >= CSIS_MAX_ENTITIES,
				"Wrong CSI channel id: %d\n", pdata->mux_id))
				return -EINVAL;
			csis = fmd->csis[pdata->mux_id].sd;
			if (WARN(csis == NULL,
				 "MIPI-CSI interface specified "
				 "but s5p-csis module is not loaded!\n"))
				return -EINVAL;

			ret = media_entity_create_link(&sensor->entity, 0,
					      &csis->entity, CSIS_PAD_SINK,
					      flags);
			if (ret)
				return ret;

			v4l2_info(&fmd->v4l2_dev, "created link [%s] -> [%s]",
				  sensor->entity.name, csis->entity.name);

			/* 2. create link beween mipi-csi and fimc */
			source = &csis->entity;
			for (i = 0; i < FIMC_MAX_DEVS; i++) {
				if (!fmd->fimc[i])
					continue;
				/*
				 * Some FIMC variants are not fitted with camera capture
				 * interface. Skip creating a link from sensor for those.
				 */
				if (!fmd->fimc[i]->variant->has_cam_if)
					continue;
				sink = &fmd->fimc[i]->vid_cap.subdev.entity;

				ret = media_entity_create_link(source,
					CSIS_PAD_SOURCE,
					sink,
					FIMC_SD_PAD_SINK, flags);
				if (ret)
					return ret;

				/* Notify FIMC capture subdev entity */
				ret = media_entity_call(sink, link_setup,
					&sink->pads[FIMC_SD_PAD_SINK],
					&source->pads[CSIS_PAD_SOURCE], flags);

				if (ret)
					break;
				v4l2_info(&fmd->v4l2_dev,
					"created link [%s] %c> [%s]",
					source->name, flags ? '=' : '-',
					sink->name);
			}
			break;
		case FIMC_IS_WB:
			/* 1. create link beween sensor and mipi-csi */
			if (WARN(pdata->mux_id >= CSIS_MAX_ENTITIES,
				"Wrong CSI channel id: %d\n", pdata->mux_id))
				return -EINVAL;
			csis = fmd->csis[pdata->mux_id].sd;
			if (WARN(csis == NULL,
				 "MIPI-CSI interface specified "
				 "but s5p-csis module is not loaded!\n"))
				return -EINVAL;

			ret = media_entity_create_link(&sensor->entity, 0,
					      &csis->entity, CSIS_PAD_SINK,
					      flags);
			if (ret)
				return ret;

			v4l2_info(&fmd->v4l2_dev, "created link [%s] -> [%s]",
				  sensor->entity.name, csis->entity.name);
			/* 2. create link beween mipi-csi and fimc-lite */
			source = &csis->entity;
			sink = &fmd->fimc_lite[pdata->mux_id]->subdev.entity;
			ret = media_entity_create_link(source, CSIS_PAD_SOURCE,
					sink, FLITE_SD_PAD_SINK,
					flags);
			if (ret)
				return ret;
			/* Notify FIMC-LITE subdev entity */
			ret = media_entity_call(sink, link_setup,
					&source->pads[CSIS_PAD_SOURCE],
					&sink->pads[FLITE_SD_PAD_SINK],
					flags);
			if (ret)
				break;

			v4l2_info(&fmd->v4l2_dev, "created link [%s] -> [%s]",
				source->name, sink->name);
			/* 3. create link beween fimc-lite and fimc-is */
			source = &fmd->fimc_lite[pdata->mux_id]->subdev.entity;
			sink = &fmd->fimc_is->isp.subdev.entity;
			ret = media_entity_create_link(source,
					FLITE_SD_PAD_SOURCE,
					sink, FIMC_IS_SD_PAD_SINK,
					flags);
			if (ret)
				return ret;

			/* Notify FIMC-IS subdev entity */
			ret = media_entity_call(sink, link_setup,
					&source->pads[FLITE_SD_PAD_SOURCE],
					&sink->pads[FIMC_IS_SD_PAD_SINK],
					flags);

			v4l2_info(&fmd->v4l2_dev, "created link [%s] -> [%s]",
				source->name, sink->name);

			/* 4. create link beween fimc-is and fimc */
			source = &fmd->fimc_is->isp.subdev.entity;
			for (i = 0; i < (FIMC_MAX_DEVS - 1); i++) {
				if (!fmd->fimc[i])
					continue;
				/*
				 * Some FIMC variants are not fitted with camera capture
				 * interface. Skip creating a link from sensor for those.
				 */
				if (!fmd->fimc[i]->variant->has_cam_if)
					continue;
				sink = &fmd->fimc[i]->vid_cap.subdev.entity;

				ret = media_entity_create_link(source,
					CSIS_PAD_SOURCE,
					sink,
					FIMC_SD_PAD_SINK, flags);
				if (ret)
					return ret;

				/* Notify FIMC capture subdev entity */
				ret = media_entity_call(sink, link_setup,
					&source->pads[CSIS_PAD_SOURCE],
					&sink->pads[FIMC_SD_PAD_SINK],
					flags);

				if (ret)
					break;
				v4l2_info(&fmd->v4l2_dev,
					"created link [%s] %c> [%s]",
					source->name, flags ? '=' : '-',
					sink->name);
			}
			break;
		default:
			v4l2_err(&fmd->v4l2_dev, "Wrong bus_type: %x\n",
				 pdata->bus_type);
			return -EINVAL;
		}
	}

	/* Create immutable link between each FIMC's subdev and video node */
	flags = MEDIA_LNK_FL_IMMUTABLE | MEDIA_LNK_FL_ENABLED;
	for (i = 0; i < FIMC_MAX_DEVS; i++) {
		if (!fmd->fimc[i])
			continue;
		source = &fmd->fimc[i]->vid_cap.subdev.entity;
		sink = &fmd->fimc[i]->vid_cap.vfd.entity;
		ret = media_entity_create_link(source, FIMC_SD_PAD_SOURCE,
					      sink, 0, flags);
		if (ret)
			break;
		v4l2_info(&fmd->v4l2_dev, "created link [%s] %c> [%s]",
				source->name, flags ? '=' : '-', sink->name);
	}
	return ret;
}

/*
 * The peripheral sensor clock management.
 */
static int fimc_md_get_clocks(struct fimc_md *fmd,
				struct s5p_platform_fimc *pdata)
{
	char clk_name[32];
	struct clk *clock;
	int i, tmp;

	for (i = 0; i < FIMC_MAX_CAMCLKS; i++) {
		snprintf(clk_name, sizeof(clk_name), "sclk_cam%u", i);
		clock = clk_get(NULL, clk_name);
		if (IS_ERR_OR_NULL(clock)) {
			v4l2_err(&fmd->v4l2_dev, "Failed to get clock: %s",
				  clk_name);
			return -ENXIO;
		}
		fmd->camclk[i].clock = clock;
	}

	tmp = 0;
	for (i = 0; i < pdata->num_clients; i++) {
		if (pdata->isp_info[i].use_isp) {
			tmp++;
			break;
		}
	}

	if (tmp == 0)
		return 0;
	/*
	 * For now get only PIXELASYNCM1 clock (Writeback B/ISP),
	 * leave PIXELASYNCM0 out for the display driver.
	 */
	for (i = CLK_IDX_WB_B ; i < FIMC_MAX_WBCLKS; i++) {
		snprintf(clk_name, sizeof(clk_name), "pxl_async%u", i);
		clock = clk_get(NULL, clk_name);
		if (IS_ERR_OR_NULL(clock)) {
			v4l2_err(&fmd->v4l2_dev, "Failed to get clock: %s",
				  clk_name);
			return -ENXIO;
		}
		fmd->wbclk[i] = clock;

		pr_info("fimc_md_get_clocks : clk_get : pxl_async%u\n", i);
	}
	return 0;
}

static void fimc_md_put_clocks(struct fimc_md *fmd)
{
	int i;

	/* External sensor master clocks (SCLK_CAM) */
	for (i = 0; i < FIMC_MAX_CAMCLKS; i++) {
		if (IS_ERR_OR_NULL(fmd->camclk[i].clock))
			continue;
		clk_put(fmd->camclk[i].clock);
		fmd->camclk[i].clock = NULL;
	}
	/* Writeback (PIXELASYNCMx) clocks */
	for (i = 0; i < FIMC_MAX_WBCLKS; i++) {
		if (IS_ERR_OR_NULL(fmd->wbclk[i]))
			continue;
		clk_put(fmd->wbclk[i]);
		fmd->wbclk[i] = NULL;
	}
}

static int __fimc_md_set_camclk(struct fimc_md *fmd,
				struct fimc_sensor_info *s_info,
				bool on)
{
	struct s5p_fimc_isp_info *pdata = &s_info->pdata;
	struct fimc_camclk_info *camclk;
	int ret = 0;

	if (WARN_ON(pdata->clk_id >= FIMC_MAX_CAMCLKS) || fmd == NULL)
		return -EINVAL;

	camclk = &fmd->camclk[pdata->clk_id];

	dbg("camclk %d, f: %lu, use_count: %d, on: %d",
	    pdata->clk_id, pdata->clk_frequency, camclk->use_count, on);

	if (on) {
		if (camclk->use_count > 0 &&
		    camclk->frequency != pdata->clk_frequency)
			return -EINVAL;

		if (camclk->use_count++ == 0) {
			clk_set_rate(camclk->clock, pdata->clk_frequency);
			camclk->frequency = pdata->clk_frequency;
			ret = clk_enable(camclk->clock);
			dbg("Enabled camclk %d: f: %lu", pdata->clk_id,
			    clk_get_rate(camclk->clock));

			if (pdata->is_sensor_info)
				ret = clk_enable(fmd->wbclk[CLK_IDX_WB_B]);
		}
		return ret;
	}

	if (WARN_ON(camclk->use_count == 0))
		return 0;

	if (--camclk->use_count == 0) {
		clk_disable(camclk->clock);
		dbg("Disabled camclk %d", pdata->clk_id);

		if (pdata->is_sensor_info)
			clk_disable(fmd->wbclk[CLK_IDX_WB_B]);
	}
	return ret;
}

/**
 * fimc_md_set_camclk - peripheral sensor clock setup
 * @sd: sensor subdev to configure sclk_cam clock for
 * @on: 1 to enable or 0 to disable the clock
 *
 * There are 2 separate clock outputs available in the SoC for external
 * image processors. These clocks are shared between all registered FIMC
 * devices to which sensors can be attached, either directly or through
 * the MIPI CSI receiver. The clock is allowed here to be used by
 * multiple sensors concurrently if they use same frequency.
 * This function should only be called when the graph mutex is held.
 */
int fimc_md_set_camclk(struct v4l2_subdev *sd, bool on)
{
	struct fimc_sensor_info *s_info = v4l2_get_subdev_hostdata(sd);
	struct fimc_md *fmd = entity_to_fimc_mdev(&sd->entity);

	return __fimc_md_set_camclk(fmd, s_info, on);
}

static int fimc_md_link_notify(struct media_pad *source,
			       struct media_pad *sink, u32 flags)
{
	struct fimc_lite *fimc_lite = NULL;
	struct fimc_dev *fimc = NULL;
	struct fimc_pipeline *pipeline;
	struct v4l2_subdev *sd;
	int ret = 0;

	if (media_entity_type(sink->entity) != MEDIA_ENT_T_V4L2_SUBDEV)
		return 0;

	sd = media_entity_to_v4l2_subdev(sink->entity);

	switch (sd->grp_id) {
	case FLITE_GROUP_ID:
		fimc_lite = v4l2_get_subdevdata(sd);
		pipeline = &fimc_lite->pipeline;
		break;
	case FIMC_GROUP_ID:
		fimc = v4l2_get_subdevdata(sd);
		pipeline = &fimc->pipeline;
		break;
	default:
		return 0;
	}

	if (!(flags & MEDIA_LNK_FL_ENABLED)) {
		ret = __fimc_pipeline_close(pipeline);
		pipeline->subdevs[IDX_SENSOR] = NULL;
		pipeline->subdevs[IDX_CSIS] = NULL;

		if (fimc) {
			mutex_lock(&fimc->lock);
			fimc_ctrls_delete(fimc->vid_cap.ctx);
			mutex_unlock(&fimc->lock);
		}
		return ret;
	}
	/*
	 * Link activation. Enable power of pipeline elements only if the
	 * pipeline is already in use, i.e. its video node is opened.
	 * Recreate the controls destroyed during the link deactivation.
	 */
	if (fimc) {
		mutex_lock(&fimc->lock);
		if (fimc->vid_cap.refcnt > 0) {
			ret = __fimc_pipeline_open(pipeline,
						   source->entity, true);
		if (!ret)
			ret = fimc_capture_ctrls_create(fimc);
		}
		mutex_unlock(&fimc->lock);
	} else {
		mutex_lock(&fimc_lite->lock);
		ret = __fimc_pipeline_open(pipeline, source->entity, true);
		mutex_unlock(&fimc_lite->lock);
	}
	return ret ? -EPIPE : ret;
}

static ssize_t fimc_md_sysfs_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct fimc_md *fmd = platform_get_drvdata(pdev);

	if (fmd->user_subdev_api)
		return strlcpy(buf, "Sub-device API (sub-dev)\n", PAGE_SIZE);

	return strlcpy(buf, "V4L2 video node only API (vid-dev)\n", PAGE_SIZE);
}

static ssize_t fimc_md_sysfs_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct fimc_md *fmd = platform_get_drvdata(pdev);
	bool subdev_api;
	int i;

	if (!strcmp(buf, "vid-dev\n"))
		subdev_api = false;
	else if (!strcmp(buf, "sub-dev\n"))
		subdev_api = true;
	else
		return count;

	fmd->user_subdev_api = subdev_api;
	for (i = 0; i < FIMC_MAX_DEVS; i++)
		if (fmd->fimc[i])
			fmd->fimc[i]->vid_cap.user_subdev_api = subdev_api;
	return count;
}
/*
 * This device attribute is to select video pipeline configuration method.
 * There are following valid values:
 *  vid-dev - for V4L2 video node API only, subdevice will be configured
 *  by the host driver.
 *  sub-dev - for media controller API, subdevs must be configured in user
 *  space before starting streaming.
 */
static DEVICE_ATTR(subdev_conf_mode, S_IWUSR | S_IRUGO,
		   fimc_md_sysfs_show, fimc_md_sysfs_store);

static int fimc_md_probe(struct platform_device *pdev)
{
	struct s5p_platform_fimc *pdata = pdev->dev.platform_data;
	struct v4l2_device *v4l2_dev;
	struct fimc_md *fmd;
	int ret;

	fmd = devm_kzalloc(&pdev->dev, sizeof(*fmd), GFP_KERNEL);
	if (!fmd)
		return -ENOMEM;

	spin_lock_init(&fmd->slock);
	fmd->pdev = pdev;

	strlcpy(fmd->media_dev.model, "SAMSUNG S5P FIMC",
		sizeof(fmd->media_dev.model));
	fmd->media_dev.link_notify = fimc_md_link_notify;
	fmd->media_dev.dev = &pdev->dev;

	v4l2_dev = &fmd->v4l2_dev;
	v4l2_dev->mdev = &fmd->media_dev;
	v4l2_dev->notify = fimc_sensor_notify;
	snprintf(v4l2_dev->name, sizeof(v4l2_dev->name), "%s",
		 dev_name(&pdev->dev));

	ret = v4l2_device_register(&pdev->dev, &fmd->v4l2_dev);
	if (ret < 0) {
		v4l2_err(v4l2_dev, "Failed to register v4l2_device: %d\n", ret);
		return ret;
	}
	ret = media_device_register(&fmd->media_dev);
	if (ret < 0) {
		v4l2_err(v4l2_dev, "Failed to register media device: %d\n", ret);
		goto err_md;
	}
	ret = fimc_md_get_clocks(fmd, pdata);
	if (ret)
		goto err_clk;

	fmd->user_subdev_api = true;

	/* Protect the media graph while we're registering entities */
	mutex_lock(&fmd->media_dev.graph_mutex);

	ret = fimc_md_register_platform_entities(fmd);
	if (ret)
		goto err_unlock;

	if (pdata->isp_info) {
		ret = fimc_md_register_sensor_entities(fmd);
		if (ret)
			goto err_unlock;
	}
	ret = fimc_md_create_links(fmd);
	if (ret)
		goto err_unlock;
	ret = v4l2_device_register_subdev_nodes(&fmd->v4l2_dev);
	if (ret)
		goto err_unlock;

	ret = device_create_file(&pdev->dev, &dev_attr_subdev_conf_mode);
	if (ret)
		goto err_unlock;

	platform_set_drvdata(pdev, fmd);
	mutex_unlock(&fmd->media_dev.graph_mutex);
	return 0;

err_unlock:
	mutex_unlock(&fmd->media_dev.graph_mutex);
err_clk:
	media_device_unregister(&fmd->media_dev);
	fimc_md_put_clocks(fmd);
	fimc_md_unregister_entities(fmd);
err_md:
	v4l2_device_unregister(&fmd->v4l2_dev);
	return ret;
}

static int __devexit fimc_md_remove(struct platform_device *pdev)
{
	struct fimc_md *fmd = platform_get_drvdata(pdev);

	if (!fmd)
		return 0;
	device_remove_file(&pdev->dev, &dev_attr_subdev_conf_mode);
	fimc_md_unregister_entities(fmd);
	media_device_unregister(&fmd->media_dev);
	fimc_md_put_clocks(fmd);
	return 0;
}

static struct platform_driver fimc_md_driver = {
	.probe		= fimc_md_probe,
	.remove		= __devexit_p(fimc_md_remove),
	.driver = {
		.name	= "s5p-fimc-md",
		.owner	= THIS_MODULE,
	}
};

static int __init fimc_md_init(void)
{
	int ret;

	request_module("s5p-csis");
	ret = fimc_register_driver();
	if (ret)
		return ret;

	return platform_driver_register(&fimc_md_driver);
}

static void __exit fimc_md_exit(void)
{
	platform_driver_unregister(&fimc_md_driver);
	fimc_unregister_driver();
}

module_init(fimc_md_init);
module_exit(fimc_md_exit);

MODULE_AUTHOR("Sylwester Nawrocki <s.nawrocki@samsung.com>");
MODULE_DESCRIPTION("S5P FIMC camera host interface/video postprocessor driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("2.0.1");
