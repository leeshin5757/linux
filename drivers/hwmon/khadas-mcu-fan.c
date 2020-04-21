// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Khadas MCU Controlled FAN driver
 *
 * Copyright (C) 2020 BayLibre SAS
 * Author(s): Neil Armstrong <narmstrong@baylibre.com>
 */

#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/mfd/khadas-mcu.h>
#include <linux/regmap.h>
#include <linux/sysfs.h>
#include <linux/thermal.h>

#define MAX_LEVEL 3

struct khadas_mcu_fan_ctx {
	struct khadas_mcu *mcu;
	unsigned int level;
	struct thermal_cooling_device *cdev;
};

static int khadas_mcu_fan_set_level(struct khadas_mcu_fan_ctx *ctx,
				    unsigned int level)
{
	int ret;

	ret = regmap_write(ctx->mcu->map, KHADAS_MCU_CMD_FAN_STATUS_CTRL_REG,
			   level);
	if (ret)
		return ret;

	ctx->level = level;

	return 0;
}

static ssize_t level_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct khadas_mcu_fan_ctx *ctx = dev_get_drvdata(dev);
	unsigned long level;
	int ret;

	if (kstrtoul(buf, 10, &level) || level > MAX_LEVEL)
		return -EINVAL;

	ret = khadas_mcu_fan_set_level(ctx, level);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t level_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct khadas_mcu_fan_ctx *ctx = dev_get_drvdata(dev);

	return sprintf(buf, "%u\n", ctx->level);
}

static SENSOR_DEVICE_ATTR_RW(level1, level, 0);

static struct attribute *khadas_mcu_fan_attrs[] = {
	&sensor_dev_attr_level1.dev_attr.attr,
	NULL,
};

static const struct attribute_group khadas_mcu_fan_group = {
	.attrs = khadas_mcu_fan_attrs,
};

static const struct attribute_group *khadas_mcu_fan_groups[] = {
	&khadas_mcu_fan_group,
	NULL,
};

/* thermal cooling device callbacks */
static int khadas_mcu_fan_get_max_state(struct thermal_cooling_device *cdev,
					unsigned long *state)
{
	struct khadas_mcu_fan_ctx *ctx = cdev->devdata;

	if (!ctx)
		return -EINVAL;

	*state = MAX_LEVEL;

	return 0;
}

static int khadas_mcu_fan_get_cur_state(struct thermal_cooling_device *cdev,
					unsigned long *state)
{
	struct khadas_mcu_fan_ctx *ctx = cdev->devdata;

	if (!ctx)
		return -EINVAL;

	*state = ctx->level;

	return 0;
}

static int
khadas_mcu_fan_set_cur_state(struct thermal_cooling_device *cdev,
			     unsigned long state)
{
	struct khadas_mcu_fan_ctx *ctx = cdev->devdata;

	if (!ctx || (state > MAX_LEVEL))
		return -EINVAL;

	if (state == ctx->level)
		return 0;

	return khadas_mcu_fan_set_level(ctx, state);
}

static const struct thermal_cooling_device_ops khadas_mcu_fan_cooling_ops = {
	.get_max_state = khadas_mcu_fan_get_max_state,
	.get_cur_state = khadas_mcu_fan_get_cur_state,
	.set_cur_state = khadas_mcu_fan_set_cur_state,
};

static int khadas_mcu_fan_probe(struct platform_device *pdev)
{
	struct khadas_mcu *mcu = dev_get_drvdata(pdev->dev.parent);
	struct thermal_cooling_device *cdev;
	struct device *dev = &pdev->dev;
	struct khadas_mcu_fan_ctx *ctx;
	struct device *hwmon;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->mcu = mcu;
	platform_set_drvdata(pdev, ctx);

	hwmon = devm_hwmon_device_register_with_groups(dev, "khadas-mcu-fan",
						       ctx,
						       khadas_mcu_fan_groups);
	if (IS_ERR(hwmon)) {
		dev_err(dev, "Failed to register hwmon device\n");
		return PTR_ERR(hwmon);
	}

	if (IS_ENABLED(CONFIG_THERMAL)) {
		cdev = devm_thermal_of_cooling_device_register(dev->parent,
			dev->parent->of_node, "khadas-mcu-fan", ctx,
			&khadas_mcu_fan_cooling_ops);
		if (IS_ERR(cdev)) {
			ret = PTR_ERR(cdev);
			dev_err(dev,
				"Failed to register khadas-mcu-fan as cooling device: %d\n",
				ret);
			return ret;
		}
		ctx->cdev = cdev;
		thermal_cdev_update(cdev);
	}

	return 0;
}

static int khadas_mcu_fan_disable(struct device *dev)
{
	struct khadas_mcu_fan_ctx *ctx = dev_get_drvdata(dev);
	unsigned int level_save = ctx->level;
	int ret;

	ret = khadas_mcu_fan_set_level(ctx, 0);
	if (ret)
		return ret;

	ctx->level = level_save;

	return 0;
}

static void khadas_mcu_fan_shutdown(struct platform_device *pdev)
{
	khadas_mcu_fan_disable(&pdev->dev);
}

#ifdef CONFIG_PM_SLEEP
static int khadas_mcu_fan_suspend(struct device *dev)
{
	return khadas_mcu_fan_disable(dev);
}

static int khadas_mcu_fan_resume(struct device *dev)
{
	struct khadas_mcu_fan_ctx *ctx = dev_get_drvdata(dev);

	return khadas_mcu_fan_set_level(ctx, ctx->level);
}
#endif

static SIMPLE_DEV_PM_OPS(khadas_mcu_fan_pm, khadas_mcu_fan_suspend,
			 khadas_mcu_fan_resume);

static const struct platform_device_id khadas_mcu_fan_id_table[] = {
	{ .name = "khadas-mcu-fan-ctrl", },
	{},
};
MODULE_DEVICE_TABLE(platform, khadas_mcu_fan_id_table);

static struct platform_driver khadas_mcu_fan_driver = {
	.probe		= khadas_mcu_fan_probe,
	.shutdown	= khadas_mcu_fan_shutdown,
	.driver	= {
		.name		= "khadas-mcu-fan-ctrl",
		.pm		= &khadas_mcu_fan_pm,
	},
	.id_table	= khadas_mcu_fan_id_table,
};

module_platform_driver(khadas_mcu_fan_driver);

MODULE_AUTHOR("Neil Armstrong <narmstrong@baylibre.com>");
MODULE_DESCRIPTION("Khadas MCU FAN driver");
MODULE_LICENSE("GPL");
