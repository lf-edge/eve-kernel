// SPDX-License-Identifier: GPL-2.0-only
/*
 * I2C bus driver for Kontron COM modules
 *
 * Copyright (c) 2010-2016 Kontron Europe GmbH
 * Author: Michael Brunner <michael.brunner@kontron.com>
 *
 * The driver is based on the i2c-ocores driver by Peter Korsgaard.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/mfd/kempld.h>
#include <linux/interrupt.h>
#include "i2c-kempld.h"

static int scl_frequency; /* 0 = don't change */
module_param(scl_frequency, int, 0444);
MODULE_PARM_DESC(scl_frequency,
		 "Set I2C SCL frequency (0=don't change, otherwise kHz, default=0)");
static int i2c_mx_bus = -1;
module_param(i2c_mx_bus, int, 0444);
MODULE_PARM_DESC(i2c_mx_bus, "Set I2C MX bus (0-15, default=-1 (FW default))");
static bool force_polling;
module_param(force_polling, bool, 0444);
MODULE_PARM_DESC(force_polling, "Force polling mode");
static int i2c_gpio_mux = -1;
module_param(i2c_gpio_mux, int, 0444);
MODULE_PARM_DESC(i2c_gpio_mux, "Enable I2C port on GPIO out");
static int i2c_irq = -1;
module_param(i2c_irq, int, 0444);
MODULE_PARM_DESC(i2c_irq, "Set legacy I2C IRQ (1-15)");
static int i2c_bus = -1;
module_param(i2c_bus, int, 0444);
MODULE_PARM_DESC(i2c_bus,
		 "Set I2C bus number (default=-1 for dynamic assignment)");
static int enbusclear = -1;
module_param(enbusclear, int, 0444);
MODULE_PARM_DESC(enbusclear,
		 "Enable bus-clear functionality (0/1 (default=-1 for BIOS default))");
static int timeout = 1000;
module_param(timeout, int, 0664);
MODULE_PARM_DESC(timeout, "Transaction timeout in ms (default=1000)");

static void kempld_controller_reset(struct kempld_i2c_data *i2c)
{
	struct kempld_device_data *pld = i2c->pld;
	u8 ctrl, stat;

	kempld_get_mutex(pld);
	ctrl = kempld_read8(pld, KEMPLD_I2C_CTRL);
	ctrl &= ~I2C_CTRL_EN;
	kempld_write8(pld, KEMPLD_I2C_CTRL, ctrl);
	ctrl |= I2C_CTRL_EN;
	kempld_write8(pld, KEMPLD_I2C_CTRL, ctrl);
	stat = kempld_read8(pld, KEMPLD_I2C_STAT);
	if (stat & I2C_STAT_BUSY)
		kempld_write8(pld, KEMPLD_I2C_CMD, I2C_CMD_STOP);
	kempld_release_mutex(pld);
}

static int kempld_i2c_process(struct kempld_i2c_data *i2c)
{
	struct kempld_device_data *pld = i2c->pld;
	u8 stat = kempld_read8(pld, KEMPLD_I2C_STAT);
	struct i2c_msg *msg = i2c->msg;
	u8 addr;

	i2c->irq_wake = 0;

	/* Ready? */
	if (stat & I2C_STAT_TIP)
		return -EBUSY;

	if (i2c->state == STATE_DONE || i2c->state == STATE_ERROR) {
		i2c->irq_wake = 1;
		/* Stop has been sent */
		if (i2c->state == STATE_ERROR)
			return -EIO;
		return 0;
	}

	if (i2c->state == STATE_INIT) {
		if (stat & I2C_STAT_BUSY)
			return -EBUSY;

		i2c->state = STATE_ADDR;
	} else if (stat & I2C_STAT_ARBLOST) {
		i2c->state = STATE_ERROR;
		i2c->irq_wake = 1;
		kempld_write8(pld, KEMPLD_I2C_CMD, I2C_CMD_STOP);
		return -EAGAIN;
	}

	if (i2c->state == STATE_ADDR || i2c->state == STATE_ADDR_CONT) {
		/* 10 bit address? */
		if (i2c->msg->flags & I2C_M_TEN) {
			addr = 0xf0 | ((i2c->msg->addr >> 7) & 0x6);
			/* Set read bit if necessary */
			addr |= (i2c->msg->flags & I2C_M_RD) ? 1 : 0;
			i2c->state = STATE_ADDR10;
		} else {
			addr = i2c_8bit_addr_from_msg(i2c->msg);
			i2c->state = STATE_START;
		}

		kempld_write8(pld, KEMPLD_I2C_DATA, addr);
		kempld_write8(pld, KEMPLD_I2C_CMD, I2C_CMD_START);

		return 0;
	}

	/* Second part of 10 bit addressing */
	if (i2c->state == STATE_ADDR10) {
		kempld_write8(pld, KEMPLD_I2C_DATA, i2c->msg->addr & 0xff);
		kempld_write8(pld, KEMPLD_I2C_CMD, I2C_CMD_WRITE);

		i2c->state = STATE_START;
		return 0;
	}

	if (i2c->state == STATE_START || i2c->state == STATE_WRITE) {
		i2c->state = (msg->flags & I2C_M_RD) ? STATE_READ : STATE_WRITE;

		if (stat & I2C_STAT_NACK) {
			i2c->state = STATE_ERROR;
			i2c->irq_wake = 1;
			kempld_write8(pld, KEMPLD_I2C_CMD, I2C_CMD_STOP);
			return -ENXIO;
		}
	} else {
		msg->buf[i2c->pos++] = kempld_read8(pld, KEMPLD_I2C_DATA);
	}

	if (i2c->pos >= msg->len) {
		i2c->nmsgs--;
		i2c->msg++;
		i2c->pos = 0;
		msg = i2c->msg;

		if (i2c->nmsgs) {
			if (!(msg->flags & I2C_M_NOSTART)) {
				/*
				 * This is a virtual state, directly restart
				 * the handler.
				 */
				i2c->state = STATE_ADDR_CONT;
				i2c->irq_wake = 1;
				return 0;
			}
				i2c->state = (msg->flags & I2C_M_RD)
					? STATE_READ : STATE_WRITE;
		} else {
			i2c->state = STATE_DONE;
			i2c->irq_wake = 1;
			kempld_write8(pld, KEMPLD_I2C_CMD, I2C_CMD_STOP);
			return 0;
		}
	}

	if (i2c->state == STATE_READ) {
		kempld_write8(pld, KEMPLD_I2C_CMD, i2c->pos == (msg->len - 1) ?
			      I2C_CMD_READ_NACK : I2C_CMD_READ_ACK);
	} else {
		kempld_write8(pld, KEMPLD_I2C_DATA, msg->buf[i2c->pos++]);
		kempld_write8(pld, KEMPLD_I2C_CMD, I2C_CMD_WRITE);
	}

	return 0;
}

static irqreturn_t kempld_i2c_isr(int irq, void *dev_id)
{
	struct kempld_i2c_data *i2c = dev_id;
	struct kempld_device_data *pld = i2c->pld;

	kempld_get_mutex(pld);
	i2c->ret = kempld_i2c_process(i2c);
	kempld_release_mutex(pld);
	wake_up(&i2c->wait);

	return IRQ_HANDLED;
}

static int kempld_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
				int num)
{
	struct kempld_i2c_data *i2c = i2c_get_adapdata(adap);
	struct kempld_device_data *pld = i2c->pld;
	unsigned long time_limit;

	i2c->timeout = msecs_to_jiffies(timeout);
	i2c->msg = msgs;
	i2c->pos = 0;
	i2c->nmsgs = num;
	i2c->state = STATE_INIT;
	i2c->ret = -ETIMEDOUT;

	/* Handle the transfer */
	time_limit = i2c->timeout + jiffies;
	do {
		if ((i2c->irq < 1) || (i2c->state == STATE_INIT)
		    || (i2c->state == STATE_ADDR_CONT)) {
		kempld_get_mutex(pld);
			i2c->ret = kempld_i2c_process(i2c);
		kempld_release_mutex(pld);
		}

		if (i2c->state == STATE_DONE || i2c->state == STATE_ERROR)
			return (i2c->state == STATE_DONE) ? num : i2c->ret;


		if ((i2c->irq > 0) && (i2c->state > STATE_INIT)) {
			i2c->ret = -ETIMEDOUT;
			wait_event_timeout(i2c->wait, i2c->irq_wake,
					   i2c->timeout);
		} else if (i2c->state != STATE_ADDR_CONT)
		usleep_range(5, 15);

		if (i2c->ret == 0)
			time_limit = jiffies + i2c->timeout;
	} while (time_before(jiffies, time_limit));

	dev_warn(&adap->dev, "Bus seems to be blocked - trying reset!\n");

	i2c->state = STATE_ERROR;
	kempld_controller_reset(i2c);

	return -ETIMEDOUT;
}

static void kempld_i2c_device_init(struct kempld_i2c_data *i2c)
{
	struct kempld_device_data *pld = i2c->pld;
	struct device *dev = &i2c->adap.dev;
	long prescale;
	u16 prescale_corr;
	u8 cfg;
	u8 ctrl;
	u8 stat;
	u8 mx;

	kempld_get_mutex(pld);
	/* Make sure the device is disabled */
	ctrl = kempld_read8(pld, KEMPLD_I2C_CTRL);
	if (ctrl & I2C_CTRL_EN)
		i2c->was_active = 1;

	/* make sure the device is disabled */
	ctrl &= ~(I2C_CTRL_EN | I2C_CTRL_IEN);
	kempld_write8(pld, KEMPLD_I2C_CTRL, ctrl);


	if (scl_frequency > 0) {
		/*
		 * The clock frequency calculation has been changed a bit
		 * between the spec. revisions
		 */
	if (pld->info.spec_major == 1)
			prescale = (pld->pld_clock / (scl_frequency*5)) - 1000;
	else
			prescale = (pld->pld_clock / (scl_frequency*4)) - 3000;


	if (prescale < 0)
		prescale = 0;

	/* Round to the best matching value */
	prescale_corr = prescale / 1000;
		if ((prescale % 1000) >= 500)
		prescale_corr++;

	kempld_write8(pld, KEMPLD_I2C_PRELOW, prescale_corr & 0xff);
	kempld_write8(pld, KEMPLD_I2C_PREHIGH, prescale_corr >> 8);
	}

	if (enbusclear > -1) {
		if (enbusclear)
			ctrl |= I2C_CTRL_BCEN;
		else
			ctrl &= ~I2C_CTRL_BCEN;
	}
	/* Activate I2C bus output on GPIO pins */
	/* Check how much multiplexed I2C busses we have */
	mx = kempld_read8(pld, KEMPLD_I2C_MX);
	if (pld->info.spec_major > 1) {
		i2c->mx_max = KEMPLD_I2C_MX_GET_MAX(mx);
		if (i2c->mx_max == 0xf) /* No multiplexer available */
			i2c->mx_max = 0;
	} else {
		if ((mx & KEMPLD_I2C_MX_MASK) == 0xf)
			i2c->mx_max = 0; /* No multiplexer available */
		else
			i2c->mx_max = 1; /*
					  * 2 busses should be enough for all
					  * boards using spec revision 1
					  */
	}

	/* Activate I2C bus output on GPIO pins */
	if ((i2c->mx_max > 0) && (pld->info.spec_major > 1)) {
		if (i2c_gpio_mux > -1) {
	cfg = kempld_read8(pld, KEMPLD_CFG);
	if (i2c_gpio_mux)
		cfg |= KEMPLD_CFG_GPIO_I2C_MUX;
	else
		cfg &= ~KEMPLD_CFG_GPIO_I2C_MUX;
	kempld_write8(pld, KEMPLD_CFG, cfg);
		}
		cfg = kempld_read8(pld, KEMPLD_CFG);
		if (cfg & KEMPLD_CFG_GPIO_I2C_MUX)
			i2c->gpio_mux = 1;
		else
			i2c->gpio_mux = 0;
		if (((i2c_gpio_mux > 0) && (!i2c->gpio_mux))
		    || ((i2c_gpio_mux == 0) && (i2c->gpio_mux)))
			dev_warn(dev,
				 "Unable to change GPIO I2C MUX setting\n");
	}

	/* Enable the device */
	if ((i2c_mx_bus == -1) || (i2c_mx_bus > i2c->mx_max)) {
		if (i2c_mx_bus > i2c->mx_max) {
			dev_err(dev,
				"bus selected with i2c_mx_bus not available - leaving MX setting unchanged\n");
		}
		if (i2c->mx_max > 0)
			i2c->mx = mx & KEMPLD_I2C_MX_MASK;
		else
			i2c->mx = 0;
	} else
		i2c->mx = i2c_mx_bus;

	/* Connect the controller to the chosen bus output */
	if (i2c->mx_max > 0)
		kempld_write8(pld, KEMPLD_I2C_MX, i2c->mx);

	/* enable the device */
	ctrl |= I2C_CTRL_EN;
	kempld_write8(pld, KEMPLD_I2C_CTRL, ctrl);

	/*
	 * If bus is busy send a STOP signal to be sure the controller is
	 * not hanging...
	 */
	stat = kempld_read8(pld, KEMPLD_I2C_STAT);
	if (stat & I2C_STAT_BUSY) {
		dev_warn(dev, "I2C bus is busy - generating stop signal\n");
		kempld_write8(pld, KEMPLD_I2C_CMD, I2C_CMD_STOP);
}

	/* Read again to check if busclear is supported */
	ctrl = kempld_read8(pld, KEMPLD_I2C_CTRL);

	kempld_release_mutex(pld);

	i2c->busclear = (ctrl & I2C_CTRL_BCEN) ? 1 : 0;
}

static void kempld_i2c_irq_enable(struct kempld_i2c_data *i2c)
{
	struct kempld_device_data *pld = i2c->pld;
	struct device *dev = &i2c->adap.dev;
	u8 irq, ctrl;
	int irq_tmp, ret;

	if (i2c->irq < 0)
		return;

	irq_tmp = i2c->irq;

	kempld_get_mutex(pld);

	irq = kempld_read8(pld, KEMPLD_IRQ_I2C);

	if (irq == 0xff) {
		kempld_release_mutex(pld);
		dev_info(dev, "I2C controller has no IRQ support\n");
		i2c->irq = -1;
		return;
	}

	if (i2c->irq == 0) {
		irq_tmp = kempld_request_irq_num(pld,
						  irq & KEMPLD_IRQ_I2C_MASK);
		if (irq_tmp < 0) {
			dev_notice(dev,
				   "Automatic IRQ configuration failed");
			irq_tmp = 0;
		}
	}

	irq &= ~KEMPLD_IRQ_I2C_MASK;

	if ((i2c_irq > 0) && (i2c_irq <= 15)) {
		irq |= i2c_irq;
	} else {
		if (i2c_irq != -1) {
			dev_warn(dev,
				 "i2c_irq option out of range - ignored\n");
			i2c_irq = -1;
		}
		irq |= irq_tmp;
	}

	kempld_write8(pld, KEMPLD_IRQ_I2C, irq);
	irq = kempld_read8(pld, KEMPLD_IRQ_I2C) & KEMPLD_IRQ_I2C_MASK;
	kempld_release_mutex(pld);

	/* IRQ support disabled */
	if (irq == 0) {
		i2c->irq = -1;
		return;
	}

	/* Initialize interrupt handlers if not already done */
	if (i2c->irq == 0) {
		ret = devm_request_threaded_irq(dev, irq, NULL, kempld_i2c_isr,
						IRQF_ONESHOT, i2c->adap.name,
						i2c);
		if (ret) {
			dev_err(dev,
				"Unable to claim IRQ - using polling mode\n");
			i2c->irq = -1;
			return;
		}
	}

	i2c->irq = irq;

	/* Now enable interrupts in the controller */
	kempld_get_mutex(pld);
	ctrl = kempld_read8(pld, KEMPLD_I2C_CTRL);
	ctrl |= I2C_CTRL_IEN;
	kempld_write8(pld, KEMPLD_I2C_CTRL, ctrl);
	kempld_release_mutex(pld);
}

static void kempld_i2c_irq_disable(struct kempld_i2c_data *i2c)
{
	struct kempld_device_data *pld = i2c->pld;
	u8 ctrl;

	if (i2c->irq <= 0)
		return;

	kempld_get_mutex(pld);
	ctrl = kempld_read8(pld, KEMPLD_I2C_CTRL);
	ctrl &= ~I2C_CTRL_IEN;
	kempld_write8(pld, KEMPLD_I2C_CTRL, ctrl);
	kempld_release_mutex(pld);
}

static u32 kempld_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_10BIT_ADDR
		| I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm kempld_i2c_algorithm = {
	.master_xfer	= kempld_i2c_xfer,
	.functionality	= kempld_i2c_func,
};

static const struct i2c_adapter kempld_i2c_adapter = {
	.owner		= THIS_MODULE,
	.name		= "i2c-kempld",
	.class		= 0,			/*
						 * No autodetection as we don't
						 * know the mux setting of the
						 * adapter - use i2c-mux-kempld
						 * driver if autodetection is
						 * desired
						 */
	.algo		= &kempld_i2c_algorithm,
};

static int kempld_i2c_get_scl_frequency(struct kempld_i2c_data *i2c)
{
	struct kempld_device_data *pld = i2c->pld;
	int frequency;
	u16 prescale;

	kempld_get_mutex(pld);

	prescale = kempld_read8(pld, KEMPLD_I2C_PRELOW)
		| kempld_read8(pld, KEMPLD_I2C_PREHIGH)<<8;

	kempld_release_mutex(pld);

	/*
	 * The clock frequency calculation has been changed a bit
	 * between the spec. revisions
	 */
	if (pld->info.spec_major == 1)
		frequency = (pld->pld_clock / (prescale + 1)) / 5000;
	else
		frequency = (pld->pld_clock / (prescale + 3)) / 4000;

	return frequency;
}

static int kempld_i2c_probe(struct platform_device *pdev)
{
	struct kempld_i2c_data *i2c;
	struct kempld_device_data *pld;
	int ret;

	pld = dev_get_drvdata(pdev->dev.parent);

	i2c = devm_kzalloc(&pdev->dev, sizeof(*i2c), GFP_KERNEL);
	if (!i2c)
		return -ENOMEM;

	i2c->pld = pld;

	kempld_i2c_device_init(i2c);


	platform_set_drvdata(pdev, i2c);
	i2c->adap = kempld_i2c_adapter;
	i2c_set_adapdata(&i2c->adap, i2c);
	i2c->adap.dev.parent = &pdev->dev;

	/* Add I2C adapter to I2C tree */
	if (i2c_bus >= -1) {
		i2c->adap.nr = i2c_bus;
		i2c->base_nr = i2c_bus;
	}
	ret = i2c_add_numbered_adapter(&i2c->adap);
	if (ret) {
		dev_err(&pdev->dev, "Failed to add adapter\n");
		goto add_adapter_failed;
	}

	/* Set up IRQ support */
	if (force_polling)
		i2c->irq = -1;

	init_waitqueue_head(&i2c->wait);
	kempld_i2c_irq_enable(i2c);

	/* Register i2c-mux device */
	i2c->mux_pdev = platform_device_register_data(&i2c->adap.dev,
					       "i2c-mux-kempld",
					       PLATFORM_DEVID_AUTO,
					       i2c,
					       sizeof(struct kempld_i2c_data));
	if (IS_ERR(i2c->mux_pdev)) {
		dev_err(&pdev->dev, "Failed to register mux platform device\n");
		goto register_mux_failed;
	}

	dev_info(&pdev->dev, "I2C bus initialized with %d kHz SCL frequency\n",
		 kempld_i2c_get_scl_frequency(i2c));
	dev_info(&pdev->dev, "I2C MUX connected to bus %d (available: %s%d)\n",
		 i2c->mx, i2c->mx_max ? "0-" : "", i2c->mx_max);
	dev_info(&pdev->dev, "I2C IRQs %s\n", (i2c->irq > 0) ? "enabled"
		 : "disabled");
	dev_info(&pdev->dev, "I2C Bus Clear %s\n", i2c->busclear ? "enabled"
		 : "disabled");
	if (i2c->gpio_mux)
		dev_info(&pdev->dev, "GPIO I2C MUX pins enabled\n");

	return 0;

register_mux_failed:
	i2c_del_adapter(&i2c->adap);
add_adapter_failed:

	return ret;
}

static int kempld_i2c_remove(struct platform_device *pdev)
{
	struct kempld_i2c_data *i2c = platform_get_drvdata(pdev);
	struct kempld_device_data *pld = i2c->pld;
	u8 ctrl;

	platform_device_unregister(i2c->mux_pdev);

	kempld_i2c_irq_disable(i2c);
	kempld_free_irq_num(pld, i2c->irq);

	if (!i2c->was_active) {
	/*
		 * disable I2C logic if it was not activated before the
	 * driver loaded
	 */
		kempld_get_mutex(pld);
		ctrl = kempld_read8(pld, KEMPLD_I2C_CTRL);
		ctrl &= ~I2C_CTRL_EN;
		kempld_write8(pld, KEMPLD_I2C_CTRL, ctrl);
		kempld_release_mutex(pld);
	}


	i2c_del_adapter(&i2c->adap);

	platform_set_drvdata(pdev, NULL);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int kempld_i2c_suspend(struct device *dev)
{
	struct kempld_i2c_data *i2c = dev_get_drvdata(dev);
	struct kempld_device_data *pld = i2c->pld;
	u8 ctrl;

	kempld_i2c_irq_disable(i2c);

	if (!i2c->was_active) {
		/* make sure the device is disabled */
	kempld_get_mutex(pld);
	ctrl = kempld_read8(pld, KEMPLD_I2C_CTRL);
	ctrl &= ~I2C_CTRL_EN;
	kempld_write8(pld, KEMPLD_I2C_CTRL, ctrl);
	kempld_release_mutex(pld);
	}

	return 0;
}

static int kempld_i2c_resume(struct device *dev)
{
	struct kempld_i2c_data *i2c = dev_get_drvdata(dev);

	kempld_i2c_device_init(i2c);
	kempld_i2c_irq_enable(i2c);

	return 0;
}
#endif

static const struct dev_pm_ops kempld_i2c_pm_ops = {
#ifdef CONFIG_PM_SLEEP
	.suspend = kempld_i2c_suspend,
	.resume = kempld_i2c_resume,
	.poweroff =  kempld_i2c_suspend,
	.restore = kempld_i2c_resume,
#endif
};

static struct platform_driver kempld_i2c_driver = {
	.driver = {
		.name = "kempld-i2c",
		.pm = &kempld_i2c_pm_ops,
	},
	.probe		= kempld_i2c_probe,
	.remove		= kempld_i2c_remove,
};

static int __init kempld_i2c_init(void)
{
	/* Check if a valid value for the i2c_mx_bus parameter is provided */
	if ((i2c_mx_bus != -1) && (i2c_mx_bus & ~KEMPLD_I2C_MX_MASK))
		return -EINVAL;

	return platform_driver_register(&kempld_i2c_driver);
}

static void __exit kempld_i2c_exit(void)
{
	platform_driver_unregister(&kempld_i2c_driver);
}

module_init(kempld_i2c_init);
module_exit(kempld_i2c_exit);

MODULE_DESCRIPTION("KEM PLD I2C Driver");
MODULE_AUTHOR("Michael Brunner <michael.brunner@kontron.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:kempld_i2c");
MODULE_VERSION("33.0");
