// SPDX-License-Identifier: GPL-2.0
/*
 *  I2C bus multiplexer driver for Kontron COM modules
 *
 *  Copyright (c) 2010-2024 Kontron Europe GmbH
 *  Copyright (c) 2025 JUMPtec GmbH
 *  Author: Michael Brunner <michael.brunner@jumptec.com>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/dmi.h>
#include <linux/i2c.h>
#include <linux/i2c-mux.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/mfd/kempld.h>
#include "../busses/i2c-kempld.h"

static bool noeeep;
module_param(noeeep, bool, 0444);
MODULE_PARM_DESC(noeeep, "Skip registering the Kontron EEEP device");

static bool noddcbl;
module_param(noddcbl, bool, 0444);
MODULE_PARM_DESC(noddcbl, "Skip registering the Kontron DDC backlight device");

static const struct i2c_board_info kontron_eeep_info_come = {
	I2C_BOARD_INFO("kontron_eeep", 0x50),
};

static const struct i2c_board_info kontron_eeep_info_bkpl = {
	I2C_BOARD_INFO("kontron_eeep", 0x57),
};

static const struct i2c_board_info kontron_ddcbl_info_come = {
	I2C_BOARD_INFO("kontron_i2c_bklt", 0x31),
};

static void i2c_mux_kempld_register_eeep(struct kempld_device_data *pld,
					 struct i2c_mux_core *muxc)
{
	if (pld->eeep == KEMPLD_EEEP_NONE)
		return;

	if (pld->eeep == KEMPLD_EEEP_COME_BKPL
	    || pld->eeep == KEMPLD_EEEP_COME) {
		if (i2c_new_client_device(muxc->adapter[0],
					  &kontron_eeep_info_come))
			dev_info(pld->dev, "Registered COMe EEEP\n");
		if (pld->eeep == KEMPLD_EEEP_COME)
			return;
	}

	if (pld->eeep == KEMPLD_EEEP_COME_BKPL
	    || pld->eeep == KEMPLD_EEEP_BKPL) {
		if (i2c_new_client_device(muxc->adapter[0],
					  &kontron_eeep_info_bkpl))
			dev_info(pld->dev, "Registered COMe backplane EEEP\n");
		return;
	}

	dev_warn(pld->dev, "Unknown EEEP configuration specified (%d)\n",
		 pld->eeep);
}

static void i2c_mux_kempld_register_ddcbl(struct kempld_device_data *pld,
					  struct i2c_mux_core *muxc)
{
	switch (pld->ddc) {
	case KEMPLD_DDC_NONE:
		break;
	case KEMPLD_DDC_COME:
		if (i2c_new_client_device(muxc->adapter[1],
					  &kontron_ddcbl_info_come))
			dev_info(pld->dev, "Registered COMe DDC backlight\n");
		break;
	default:
		dev_warn(pld->dev,
			 "Unknown DDC backlight configuration specified (%d)\n",
			 pld->ddc);
		break;
	}
}

static int i2c_mux_kempld_select(struct i2c_mux_core *muxc, u32 chan)
{
	struct kempld_i2c_data *i2c = i2c_mux_priv(muxc);
	struct kempld_device_data *pld = i2c->pld;
	int ret = 0;

	if ((i2c->state == STATE_DONE) || (i2c->state == STATE_ERROR)) {
		if (i2c->mx != chan) {
			kempld_get_mutex(pld);
			i2c->mx = chan & 0x0f;
			kempld_write8(pld, KEMPLD_I2C_MX, i2c->mx);
			kempld_release_mutex(pld);
		}
	} else
		ret = -EBUSY;

	return ret;
}

static int i2c_mux_kempld_probe(struct platform_device *pdev)
{
	struct i2c_mux_core *muxc;
	struct kempld_i2c_data *i2c = dev_get_platdata(&pdev->dev);
	struct kempld_device_data *pld = i2c->pld;
	struct i2c_adapter *parent;
	int i;
	int ret;

	parent = i2c_get_adapter(i2c->adap.nr);
	if (!parent)
		return -EPROBE_DEFER;

	muxc = i2c_mux_alloc(parent, &pdev->dev, i2c->mx_max + 1, 0, 0,
			     i2c_mux_kempld_select, NULL);
	if (!muxc)
		return -ENOMEM;

	muxc->priv = i2c;

	platform_set_drvdata(pdev, muxc);

	for (i = 0; i <= i2c->mx_max; i++) {
		u32 nr = 0;

		if (i2c->base_nr >= 0)
			nr = i2c->base_nr + 1 + i;

		ret = i2c_mux_add_adapter(muxc, nr, i);
		if (ret) {
			dev_err(&pdev->dev, "Failed to add adapter %d\n", i);
			goto add_mux_adapter_failed;
		}
	}

	/*
	 * Check for Kontron EEEP devices that might be attached to this
	 * controller
	 */
	if (!noeeep)
		i2c_mux_kempld_register_eeep(pld, muxc);

	if (!noddcbl)
		i2c_mux_kempld_register_ddcbl(pld, muxc);

	return 0;

add_mux_adapter_failed:
	i2c_mux_del_adapters(muxc);
	i2c_put_adapter(parent);

	return ret;
}

static void i2c_mux_kempld_remove(struct platform_device *pdev)
{
	struct i2c_mux_core *muxc = platform_get_drvdata(pdev);

	i2c_mux_del_adapters(muxc);

	if (muxc->parent)
		i2c_put_adapter(muxc->parent);
}

static struct platform_driver i2c_mux_kempld_driver = {
	.driver = {
		.name = "i2c-mux-kempld",
	},
	.probe		= i2c_mux_kempld_probe,
	.remove		= i2c_mux_kempld_remove,
};

static int __init i2c_mux_kempld_init(void)
{
	return platform_driver_register(&i2c_mux_kempld_driver);
}

static void __exit i2c_mux_kempld_exit(void)
{
	platform_driver_unregister(&i2c_mux_kempld_driver);
}

module_init(i2c_mux_kempld_init);
module_exit(i2c_mux_kempld_exit);

MODULE_DESCRIPTION("KEM PLD I2C multiplexer driver");
MODULE_AUTHOR("Michael Brunner <michael.brunner@jumptec.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:i2c-mux-kempld");
MODULE_VERSION("38.0");
