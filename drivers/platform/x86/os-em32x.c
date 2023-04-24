// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Operational Services EM32x platform driver
 * Copyright (c) 2022, Zededa, Inc
 *
 * Author: Mikhail Malyshev <mikem@zededa.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/dmi.h>
#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <linux/platform_data/pca953x.h>

#include <linux/leds.h>

#define OS_DRIVER_VERSION "0.1"
#define EM320_PCA4554_I2C_ADDR 0x22
static struct pca953x_platform_data em320_led_gpio_data = {
	.gpio_base = 0,
	.invert = 0,
	.irq_base = -1,
};

static struct i2c_board_info em320_board_info[] = {
	{
		I2C_BOARD_INFO("pca9554", EM320_PCA4554_I2C_ADDR),
		.platform_data = &em320_led_gpio_data,
	}
};

static struct gpio_led geos_leds[] = {
	{
		.name = "red:disk-0",
		.gpio = 0,
		.default_trigger = "default-off",
		.active_low = 1,
	},
	{
		.name = "red:disk-1",
		.gpio = 1,
		.default_trigger = "default-off",
		.active_low = 1,
	},
	{
		.name = "red:disk-2",
		.gpio = 2,
		.default_trigger = "default-off",
		.active_low = 1,
	},
	{
		.name = "red:disk-3",
		.gpio = 3,
		.default_trigger = "default-off",
		.active_low = 1,
	},
	{
		.name = "blue:status-0",
		.gpio = 7,
		.default_trigger = "default-off",
		.active_low = 1,
	},
	{
		.name = "blue:status-1",
		.gpio = 6,
		.default_trigger = "default-off",
		.active_low = 1,
	},
	{
		.name = "blue:status-2",
		.gpio = 5,
		.default_trigger = "default-off",
		.active_low = 1,
	},
	{
		.name = "blue:status-3",
		.gpio = 4,
		.default_trigger = "default-off",
		.active_low = 1,
	},
};

static struct gpio_led_platform_data em320_leds_data = {
	.num_leds = ARRAY_SIZE(geos_leds),
	.leds = geos_leds,
};

static struct platform_device em320_leds_dev = {
	.name = "leds-gpio",
	.id = -1,
	.dev.platform_data = &em320_leds_data,
};

static struct platform_device *em320_devs[] = {
	&em320_leds_dev,
};

struct os_dev_config {
	int i2c_bus;
	struct i2c_board_info *i2c_devices;
	int num_i2c_devices;
	struct platform_device **plat_devs;
	int num_plat_devs;
};

static struct os_dev_config em320_config = {
	.i2c_bus = 0,
	.i2c_devices = em320_board_info,
	.num_i2c_devices = ARRAY_SIZE(em320_board_info),
	.plat_devs = em320_devs,
	.num_plat_devs = ARRAY_SIZE(em320_devs)
};

static struct os_dev_config *dev_config;

static int dmi_check_cb(const struct dmi_system_id *dmi)
{
	pr_info("Found Operational Services device '%s'\n", dmi->ident);
	dev_config = dmi->driver_data;
	return 1;
}

static const struct dmi_system_id os_dmi_table[] __initconst = {
	{
		.ident = "EM320",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Axiomtek Co., Ltd"),
			DMI_MATCH(DMI_PRODUCT_NAME, "EM320"),
		},
		.driver_data = &em320_config,
		.callback = dmi_check_cb
	},
	{
		.ident = "EM321",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Axiomtek Co., Ltd"),
			DMI_MATCH(DMI_PRODUCT_NAME, "EM321"),
		},
		.driver_data = &em320_config,
		.callback = dmi_check_cb
	},
};

static struct i2c_client *client;

static int __init os_init(void)
{
	struct i2c_adapter *adaptor;
	int ret;

	pr_info("Checking for OS devices...\n");

	if (!dmi_check_system(os_dmi_table))
		return -ENODEV;

	adaptor = i2c_get_adapter(dev_config->i2c_bus);

	if (!adaptor) {
		pr_err("Cannot get i2c adapter for bus %d\n",
		       dev_config->i2c_bus);
		return -ENODEV;
	}

	client = i2c_new_client_device(adaptor, dev_config->i2c_devices);
	i2c_put_adapter(adaptor);

	if (IS_ERR(client)) {
		pr_err("Cannot create i2c device\n");
		return PTR_ERR(client);
	}

	ret = platform_add_devices(dev_config->plat_devs,
				   dev_config->num_plat_devs);

	if (ret < 0) {
		pr_err("Cannot register platform devices\n");
		i2c_unregister_device(client);
	}

	return ret;
}

static void __exit os_exit(void)
{
	pr_info("exiting...\n");

	if (client) {
		i2c_unregister_device(client);
		platform_device_unregister(em320_devs[0]);
	}
}

module_init(os_init);
module_exit(os_exit);

MODULE_AUTHOR("Mikhail Malyshev");
MODULE_DESCRIPTION("Operational Services EM32x Support");
MODULE_VERSION(OS_DRIVER_VERSION);
MODULE_LICENSE("GPL");
