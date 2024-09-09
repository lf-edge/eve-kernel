// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kontron PLD GPIO driver
 *
 * Copyright (c) 2010-2018 Kontron Europe GmbH
 * Author: Michael Brunner <michael.brunner@kontron.com>
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/bitops.h>
#include <linux/seq_file.h>
#include <linux/errno.h>
#include <linux/platform_device.h>
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/mfd/kempld.h>

#define CUSTOM_DBG_SHOW 1

#define KEMPLD_GPIO_MAX_NUM		16
#define KEMPLD_GPIO_MASK(x)		(BIT((x) % 8))
#define KEMPLD_GPIO_DIR_NUM(x)		(0x40 + (x) / 8)
#define KEMPLD_GPIO_LVL_NUM(x)		(0x42 + (x) / 8)
#define KEMPLD_GPIO_STS_NUM(x)		(0x44 + (x) / 8)
#define KEMPLD_GPIO_EVT_LVL_EDGE_NUM(x)	(0x46 + (x) / 8)
#define KEMPLD_GPIO_EVT_LOW_HIGH_NUM(x)	(0x48 + (x) / 8)
#define KEMPLD_GPIO_IEN_NUM(x)		(0x4A + (x) / 8)
#define KEMPLD_GPIO_NMIEN_NUM(x)	(0x4C + (x) / 8)
#define KEMPLD_GPIO_OUT_LVL_NUM(x)	(0x4E + (x) / 8)
#define KEMPLD_GPIO_DIR			0x40
#define KEMPLD_GPIO_LVL			0x42
#define KEMPLD_GPIO_STS			0x44
#define KEMPLD_GPIO_EVT_LVL_EDGE	0x46
#define KEMPLD_GPIO_EVT_LOW_HIGH	0x48
#define KEMPLD_GPIO_IEN			0x4A
#define KEMPLD_GPIO_NMIEN		0x4C
#define KEMPLD_GPIO_OUT_LVL		0x4E

struct kempld_gpio_data {
	struct gpio_chip		chip;
	int				irq;
	struct kempld_device_data	*pld;
	uint16_t			nmi_mask;
	uint16_t			nmien;
	uint16_t			ien;
	uint16_t			evt_lvl_edge;
	uint16_t			evt_low_high;
	uint16_t			dir_save;
	uint16_t			lvl_save;
	int				has_out_lvl_reg;
};

static int modparam_gpiobase = -1; /* dynamic */
module_param_named(gpiobase, modparam_gpiobase, int, 0444);
MODULE_PARM_DESC(gpiobase, "The GPIO number base. -1 means dynamic, which is the default.");

static int gpionmien; /* = 0x0000; */
module_param(gpionmien, int, 0444);
MODULE_PARM_DESC(gpionmien, "Set GPIO NMIEN register (default 0x00)");

static int gpio_irq = -1;
module_param(gpio_irq, int, 0444);
MODULE_PARM_DESC(gpio_irq, "Set legacy GPIO IRQ (1-15)");

static bool restore_state;
module_param(restore_state, bool, 0444);
MODULE_PARM_DESC(restore_state,
		 "Restore GPIO state after resume [default 0=no]");

static bool use_shadow_registers = true;
module_param(use_shadow_registers, bool, 0664);
MODULE_PARM_DESC(use_shadow_registers,
		 "Use shadow registers [default 1=yes]");

static bool assign_names = true;
module_param(assign_names, bool, 0664);
MODULE_PARM_DESC(assign_names, "Assign pin names [default 1=yes]");

/*
 * Decide if to use the out_lvl or the lvl register based on the
 * availability of the register
 */
static u8 kempld_get_out_lvl_reg(struct kempld_gpio_data *gpio, int io)
{
	if (gpio->has_out_lvl_reg)
		return KEMPLD_GPIO_OUT_LVL_NUM(io);
	else
		return KEMPLD_GPIO_LVL_NUM(io);
}

/*
 * Set or clear GPIO bit
 * kempld_get_mutex must be called prior to calling this function.
 */
static void kempld_gpio_bitop(struct kempld_device_data *pld,
			      u8 reg, u8 bit, u8 val)
{
	u8 status;

	if (use_shadow_registers)
		status = kempld_read8_shadow(pld, reg);
	else
		status = kempld_read8(pld, reg);
	if (val)
		status |= KEMPLD_GPIO_MASK(bit);
	else
		status &= ~KEMPLD_GPIO_MASK(bit);
	kempld_write8(pld, reg, status);
}

static int kempld_gpio_get_bit(struct kempld_device_data *pld, u8 reg, u8 bit)
{
	u8 status;

	kempld_get_mutex(pld);
	status = kempld_read8(pld, reg);
	kempld_release_mutex(pld);

	return !!(status & KEMPLD_GPIO_MASK(bit));
}

static int kempld_gpio_get(struct gpio_chip *chip, unsigned offset)
{
	struct kempld_gpio_data *gpio = gpiochip_get_data(chip);
	struct kempld_device_data *pld = gpio->pld;

	return !!kempld_gpio_get_bit(pld, KEMPLD_GPIO_LVL_NUM(offset), offset);
}

static int kempld_gpio_get_multiple(struct gpio_chip *chip, unsigned long *mask,
				    unsigned long *bits)
{
	struct kempld_gpio_data *gpio = gpiochip_get_data(chip);
	struct kempld_device_data *pld = gpio->pld;

	kempld_get_mutex(pld);

	/* 8 bit access is faster here, so try to save some time */
	if (*mask & ~0xff)
		*bits = kempld_read16(pld, KEMPLD_GPIO_LVL) & *mask;
	else
		*bits = kempld_read8(pld, KEMPLD_GPIO_LVL) & *mask;

	kempld_release_mutex(pld);

	return 0;
}

static void kempld_gpio_set(struct gpio_chip *chip, unsigned offset, int value)
{
	struct kempld_gpio_data *gpio = gpiochip_get_data(chip);
	struct kempld_device_data *pld = gpio->pld;
	u8 reg;

	reg = kempld_get_out_lvl_reg(gpio, offset);
	kempld_get_mutex(pld);
	kempld_gpio_bitop(pld, reg, offset, value);
	kempld_release_mutex(pld);
}

static void kempld_gpio_set_multiple(struct gpio_chip *chip,
				     unsigned long *mask, unsigned long *bits)
{
	struct kempld_gpio_data *gpio = gpiochip_get_data(chip);
	struct kempld_device_data *pld = gpio->pld;
	u16 reg;

	kempld_get_mutex(pld);

	/* 8 bit access is faster here, so try to save some time */
	if (*mask & ~0xff) {
		if (use_shadow_registers)
			reg = kempld_read16_shadow(pld, KEMPLD_GPIO_LVL);
		else
			reg = kempld_read16(pld, KEMPLD_GPIO_LVL);
		reg &= ~*mask;
		reg |= *bits;
		kempld_write16(pld, KEMPLD_GPIO_LVL, reg);
	} else {
		if (use_shadow_registers)
			reg = kempld_read8_shadow(pld, KEMPLD_GPIO_LVL);
		else
			reg = kempld_read8(pld, KEMPLD_GPIO_LVL);
		reg &= ~*mask;
		reg |= *bits;
		kempld_write8(pld, KEMPLD_GPIO_LVL, reg);
	}

	kempld_release_mutex(pld);
}

static int kempld_gpio_direction_input(struct gpio_chip *chip, unsigned offset)
{
	struct kempld_gpio_data *gpio = gpiochip_get_data(chip);
	struct kempld_device_data *pld = gpio->pld;

	kempld_get_mutex(pld);
	kempld_gpio_bitop(pld, KEMPLD_GPIO_DIR_NUM(offset), offset, 0);
	kempld_release_mutex(pld);

	return 0;
}

static int kempld_gpio_direction_output(struct gpio_chip *chip, unsigned offset,
					int value)
{
	struct kempld_gpio_data *gpio = gpiochip_get_data(chip);
	struct kempld_device_data *pld = gpio->pld;
	u8 reg;

	reg = kempld_get_out_lvl_reg(gpio, offset);
	kempld_get_mutex(pld);
	kempld_gpio_bitop(pld, reg, offset, value);
	kempld_gpio_bitop(pld, KEMPLD_GPIO_DIR_NUM(offset), offset, 1);
	kempld_release_mutex(pld);

	return 0;
}

static int kempld_gpio_get_direction(struct gpio_chip *chip, unsigned offset)
{
	struct kempld_gpio_data *gpio = gpiochip_get_data(chip);
	struct kempld_device_data *pld = gpio->pld;

	if (kempld_gpio_get_bit(pld, KEMPLD_GPIO_DIR_NUM(offset), offset))
		return GPIO_LINE_DIRECTION_OUT;

	return GPIO_LINE_DIRECTION_IN;
}

static int kempld_gpio_pincount(struct kempld_device_data *pld)
{
	u16 evt, evt_back;

	kempld_get_mutex(pld);

	/* Backup event register as it might be already initialized */
	evt_back = kempld_read16(pld, KEMPLD_GPIO_EVT_LVL_EDGE);
	/* Clear event register */
	kempld_write16(pld, KEMPLD_GPIO_EVT_LVL_EDGE, 0x0000);
	/* Read back event register */
	evt = kempld_read16(pld, KEMPLD_GPIO_EVT_LVL_EDGE);
	/* Restore event register */
	kempld_write16(pld, KEMPLD_GPIO_EVT_LVL_EDGE, evt_back);

	kempld_release_mutex(pld);

	return evt ? __ffs(evt) : 16;
}

static int kempld_gpio_to_irq(struct gpio_chip *chip, unsigned int offset)
{
	struct kempld_gpio_data *gpio = gpiochip_get_data(chip);

	return gpio->irq;
}

#ifdef CONFIG_DEBUG_FS
static int kempld_gpio_get_ien(struct gpio_chip *chip, unsigned int offset)
{
	struct kempld_gpio_data *gpio = gpiochip_get_data(chip);
	struct kempld_device_data *pld = gpio->pld;
	int status;

	kempld_get_mutex(pld);

	status = kempld_read8(pld, KEMPLD_GPIO_IEN_NUM(offset));
	status &= KEMPLD_GPIO_MASK(offset);

	kempld_release_mutex(pld);

	return status ? 1 : 0;
}

static int kempld_gpio_get_evt_lvl_edge(struct gpio_chip *chip,
					unsigned int offset)
{
	struct kempld_gpio_data *gpio = gpiochip_get_data(chip);
	struct kempld_device_data *pld = gpio->pld;
	int status;

	kempld_get_mutex(pld);

	status = kempld_read8(pld, KEMPLD_GPIO_EVT_LVL_EDGE_NUM(offset));
	status &= KEMPLD_GPIO_MASK(offset);

	kempld_release_mutex(pld);

	return status ? 1 : 0;
}

static int kempld_gpio_get_evt_high_low(struct gpio_chip *chip,
					unsigned int offset)
{
	struct kempld_gpio_data *gpio = gpiochip_get_data(chip);
	struct kempld_device_data *pld = gpio->pld;
	int status;

	gpio = dev_get_drvdata(chip->parent);
	pld = gpio->pld;

	kempld_get_mutex(pld);

	status = kempld_read8(pld, KEMPLD_GPIO_EVT_LOW_HIGH_NUM(offset));
	status &= KEMPLD_GPIO_MASK(offset);

	kempld_release_mutex(pld);

	return status ? 1 : 0;
}

static int kempld_gpio_get_nmien(struct gpio_chip *chip, unsigned int offset)
{
	struct kempld_gpio_data *gpio = gpiochip_get_data(chip);
	struct kempld_device_data *pld = gpio->pld;
	int status;

	gpio = dev_get_drvdata(chip->parent);
	pld = gpio->pld;

	kempld_get_mutex(pld);

	status = kempld_read8(pld, KEMPLD_GPIO_NMIEN_NUM(offset));
	status &= KEMPLD_GPIO_MASK(offset);

	kempld_release_mutex(pld);

	return status ? 1 : 0;
}

static void kempld_gpio_dbg_show(struct seq_file *s, struct gpio_chip *chip)
{
	int i;

	for (i = 0; i < chip->ngpio; i++) {
		int gpio = i + chip->base;

		seq_printf(s, " gpio-%-3d %s %s", gpio,
			   kempld_gpio_get_direction(chip, i) ? "in" : "out",
			   kempld_gpio_get(chip, i) ? "hi" : "lo");
		seq_printf(s, ", event on %s (irq %s, nmi %s)",
			   (kempld_gpio_get_evt_lvl_edge(chip, i)
			    ? (kempld_gpio_get_evt_high_low(chip, i)
			       ? "rising edge" : "falling edge") :
			    (kempld_gpio_get_evt_high_low(chip, i)
			     ? "high level" : "low level")),
			   kempld_gpio_get_ien(chip, i)
			   ? "enabled" : "disabled",
			   kempld_gpio_get_nmien(chip, i)
			   ? "enabled" : "disabled");
		if (chip->names)
			seq_printf(s, " - %s\n", chip->names[i]);
		else
			seq_puts(s, "\n");
	}
}
#else
#define kempld_gpio_dbg_show NULL
#endif

static void kempld_gpio_setup_event(struct kempld_gpio_data *gpio)
{
	struct kempld_device_data *pld = gpio->pld;
	struct gpio_chip *chip = &gpio->chip;
	struct device *dev = chip->parent;
	u8 irq;
	int irq_tmp;

	if (gpio->irq < 0)
		return;

	irq_tmp = gpio->irq;

	kempld_get_mutex(pld);

	irq = kempld_read8(pld, KEMPLD_IRQ_GPIO);

	if (irq == 0xff) {
		kempld_release_mutex(pld);
		dev_info(pld->dev, "GPIO controller has no IRQ support\n");
		gpio->irq = -1;
		return;
	}

	/* This only has to be done once */
	if (gpio->irq == 0) {
		irq_tmp = kempld_request_irq_num(pld,
						   irq & KEMPLD_IRQ_GPIO_MASK);
		if (irq_tmp < 0) {
			dev_notice(dev, "Automatic IRQ configuration failed");
			irq_tmp = 0;
		}
	}

	irq &= ~KEMPLD_IRQ_GPIO_MASK;

	if ((gpio_irq > 0) && (gpio_irq <= 15))
		irq |= gpio_irq;
	else {
		if (gpio_irq != -1) {
			dev_warn(dev, "gpio_irq option out of range - ignored\n");
			gpio_irq = -1;
		}
		irq |= irq_tmp;
	}

	kempld_write8(pld, KEMPLD_IRQ_GPIO, irq);

	kempld_write16(pld, KEMPLD_GPIO_STS, 0xffff);
	kempld_write16(pld, KEMPLD_GPIO_NMIEN, gpio->nmien);
	kempld_write16(pld, KEMPLD_GPIO_IEN, gpio->ien);
	kempld_write16(pld, KEMPLD_GPIO_EVT_LVL_EDGE, gpio->evt_lvl_edge);
	kempld_write16(pld, KEMPLD_GPIO_EVT_LOW_HIGH, gpio->evt_low_high);

	irq = kempld_read8(pld, KEMPLD_IRQ_GPIO);

	kempld_release_mutex(pld);

	if (irq & KEMPLD_IRQ_GPIO_NMI_AVAILABLE) {
		gpio->nmi_mask = gpionmien & (BIT(chip->ngpio)-1);
		if (gpio->nmi_mask)
			dev_info(pld->dev, "GPIO NMI source mask: 0x%04X",
				 gpio->nmi_mask);
	}

	irq = irq & KEMPLD_IRQ_GPIO_MASK;

	if (irq == 0)
		gpio->irq = -1;
	else
		gpio->irq = irq;
}

#ifdef CONFIG_GPIOLIB_IRQCHIP
static void kempld_irq_mask(struct irq_data *data)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	struct kempld_gpio_data *gpio = gpiochip_get_data(chip);
	struct kempld_device_data *pld = gpio->pld;

	gpio->ien &= ~BIT(data->hwirq);
	gpio->nmien &= ~BIT(data->hwirq);

	kempld_write16(pld, KEMPLD_GPIO_NMIEN, gpio->nmien);
	kempld_write16(pld, KEMPLD_GPIO_IEN, gpio->ien);
}

static void kempld_irq_unmask(struct irq_data *data)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	struct kempld_gpio_data *gpio = gpiochip_get_data(chip);
	struct kempld_device_data *pld = gpio->pld;

	/* If GPIO is set to NMI, IRQ is masked and only NMI is activated */
	gpio->ien |= ~gpio->nmi_mask & BIT(data->hwirq);
	gpio->nmien |= gpio->nmi_mask & BIT(data->hwirq);

	kempld_write16(pld, KEMPLD_GPIO_STS, BIT(data->hwirq));
	kempld_write16(pld, KEMPLD_GPIO_NMIEN, gpio->nmien);
	kempld_write16(pld, KEMPLD_GPIO_IEN, gpio->ien);
}

static int kempld_irq_set_type(struct irq_data *data, unsigned int type)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	struct kempld_gpio_data *gpio = gpiochip_get_data(chip);
	struct kempld_device_data *pld = gpio->pld;

	switch (type) {
	case IRQ_TYPE_EDGE_RISING:
		gpio->evt_low_high |= BIT(data->hwirq);
		gpio->evt_lvl_edge |= BIT(data->hwirq);
		break;
	case IRQ_TYPE_EDGE_FALLING:
		gpio->evt_low_high &= ~BIT(data->hwirq);
		gpio->evt_lvl_edge |= BIT(data->hwirq);
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		gpio->evt_low_high |= BIT(data->hwirq);
		gpio->evt_lvl_edge &= ~BIT(data->hwirq);
		break;
	case IRQ_TYPE_LEVEL_LOW:
		gpio->evt_low_high &= ~BIT(data->hwirq);
		gpio->evt_lvl_edge &= ~BIT(data->hwirq);
		break;
	default:
		return -EINVAL;
	}

	kempld_write16(pld, KEMPLD_GPIO_EVT_LVL_EDGE, gpio->evt_lvl_edge);
	kempld_write16(pld, KEMPLD_GPIO_EVT_LOW_HIGH, gpio->evt_low_high);

	return 0;
}

static void kempld_bus_lock(struct irq_data *data)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	struct kempld_gpio_data *gpio = gpiochip_get_data(chip);
	struct kempld_device_data *pld = gpio->pld;

	kempld_get_mutex(pld);
}

static void kempld_bus_sync_unlock(struct irq_data *data)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	struct kempld_gpio_data *gpio = gpiochip_get_data(chip);
	struct kempld_device_data *pld = gpio->pld;

	kempld_release_mutex(pld);
}


static struct irq_chip kempld_irqchip = {
	.name			= "kempld-gpio",
	.irq_mask		= kempld_irq_mask,
	.irq_unmask		= kempld_irq_unmask,
	.irq_set_type		= kempld_irq_set_type,
	.irq_bus_lock		= kempld_bus_lock,
	.irq_bus_sync_unlock	= kempld_bus_sync_unlock,
};

/* NOTE: The IRQ handler is threaded */
static irqreturn_t kempld_gpio_irq_handler(int irq, void *dev_id)
{
	struct gpio_chip *chip = dev_id;
	struct kempld_gpio_data *gpio = gpiochip_get_data(chip);
	struct kempld_device_data *pld = gpio->pld;
	u16 status;
	int handled = 0;
	unsigned int pin, virq;

	/*
	 * Loop until all incoming interrupts are handled, otherwise it might
	 * happen that something gets lost with a lot events and the IRQ
	 * handling stops.
	 */
	while (1) {
		kempld_get_mutex(pld);
		if (gpio->ien & ~0xff)
			status = kempld_read16(pld, KEMPLD_GPIO_STS);
		else
			status = kempld_read8(pld, KEMPLD_GPIO_STS);

		if (!(status & gpio->ien)) {
			kempld_release_mutex(pld);
			break;
		}

		if (gpio->ien & ~0xff)
			kempld_write16(pld, KEMPLD_GPIO_STS, status);
		else
			kempld_write8(pld, KEMPLD_GPIO_STS, status);
		kempld_release_mutex(pld);

		status &= gpio->ien;

		for_each_set_bit(pin, (const unsigned long *)&status,
				 chip->ngpio) {
			virq = irq_find_mapping(chip->irq.domain, pin);
			handle_nested_irq(virq);
		}
		handled = 1;
	}

	return handled ? IRQ_HANDLED : IRQ_NONE;
}

static int kempld_gpio_irq_init(struct device *dev,
				struct kempld_gpio_data *gpio,
				struct gpio_chip *chip)
{
	struct gpio_irq_chip *girq;
	int ret;

	ret = devm_request_threaded_irq(dev, gpio->irq, NULL,
					kempld_gpio_irq_handler, IRQF_ONESHOT,
					chip->label, chip);
	if (ret) {
		dev_err(dev, "failed to request irq %d\n", gpio->irq);
		return ret;
	}

	girq = &chip->irq;
	girq->chip = &kempld_irqchip;
	girq->parent_handler = NULL;
	girq->num_parents = 0;
	girq->parents = NULL;
	girq->default_type = IRQ_TYPE_NONE;
	girq->handler = handle_simple_irq;
	girq->threaded = true;

	dev_info(dev, "Enabled IRQ functionality with IRQ %u as base\n",
		 gpio->irq);

	return 0;
}
#else
static int kempld_gpio_irq_init(struct device *dev,
				struct kempld_gpio_data *gpio,
				struct gpio_chip *chip)
{
	dev_warn(dev,
		 "GPIOLIB_IRQCHIP not enabled - GPIO IRQs not supported\n");

	return 0;
}
#endif

static int kempld_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct kempld_device_data *pld = dev_get_drvdata(dev->parent);
	struct kempld_platform_data *pdata = dev_get_platdata(pld->dev);
	struct kempld_gpio_data *gpio;
	struct gpio_chip *chip;
	int ret;

	if (pld->info.spec_major < 2) {
		dev_err(dev,
			"Driver only supports GPIO devices compatible to PLD spec. rev. 2.0 or higher\n");
		return -ENODEV;
	}

	gpio = devm_kzalloc(dev, sizeof(*gpio), GFP_KERNEL);
	if (!gpio)
		return -ENOMEM;

	if ((pld->info.spec_major > 2) || (pld->info.spec_minor >= 8))
		gpio->has_out_lvl_reg = 1;

	gpio->pld = pld;

	platform_set_drvdata(pdev, gpio);

	chip = &gpio->chip;
	chip->label = "gpio-kempld";
	chip->owner = THIS_MODULE;
	chip->parent = dev;
	chip->can_sleep = true;
	chip->base = modparam_gpiobase;
	chip->direction_input = kempld_gpio_direction_input;
	chip->direction_output = kempld_gpio_direction_output;
	chip->get_direction = kempld_gpio_get_direction;
	chip->get = kempld_gpio_get;
	chip->set = kempld_gpio_set;
	chip->get_multiple = kempld_gpio_get_multiple;
	chip->set_multiple = kempld_gpio_set_multiple;
	chip->ngpio = kempld_gpio_pincount(pld);
	if (chip->ngpio == 0) {
		dev_err(dev, "No GPIO pins detected\n");
		return -ENODEV;
	}

	if (assign_names)
		chip->names = pdata->gpio_names;

	if (CUSTOM_DBG_SHOW)
		chip->dbg_show = kempld_gpio_dbg_show;

	if (gpio->irq)
		chip->to_irq = kempld_gpio_to_irq;

	gpio->nmien = 0x0000;
	gpio->ien = 0x0000;
	gpio->evt_lvl_edge = 0xffff;
	gpio->evt_low_high = 0xffff;

	/* Make sure the lvl and dir shadow registers are initialized */
	kempld_get_mutex(pld);
	gpio->lvl_save = kempld_read16(pld, KEMPLD_GPIO_LVL);
	kempld_write16(pld, KEMPLD_GPIO_LVL, gpio->lvl_save);
	gpio->dir_save = kempld_read16(pld, KEMPLD_GPIO_DIR);
	kempld_write16(pld, KEMPLD_GPIO_DIR, gpio->dir_save);
	kempld_release_mutex(pld);

	kempld_gpio_setup_event(gpio);
	if (gpio->irq > 0) {
		ret = kempld_gpio_irq_init(dev, gpio, chip);
		if (ret) {
			dev_err(dev, "GPIO IRQ initialization failed\n");
			gpio->irq = -1;
		}
	}

	ret = devm_gpiochip_add_data(dev, chip, gpio);
	if (ret) {
		dev_err(dev, "Could not register GPIO chip\n");
		return ret;
	}

	dev_info(dev, "GPIO functionality initialized with %d pins\n",
		 chip->ngpio);

	return 0;
}

#ifdef CONFIG_PM
static int kempld_gpio_suspend(struct device *dev)
{
	struct kempld_gpio_data *gpio = dev_get_drvdata(dev);
	struct kempld_device_data *pld = dev_get_drvdata(dev->parent);

	dev_dbg(pld->dev, "Suspending KEMPLD GPIO driver\n");

	kempld_get_mutex(pld);
	if (use_shadow_registers) {
		gpio->lvl_save = kempld_read16_shadow(pld, KEMPLD_GPIO_LVL);
		gpio->dir_save = kempld_read16_shadow(pld, KEMPLD_GPIO_DIR);
	} else {
		gpio->lvl_save = kempld_read16(pld, KEMPLD_GPIO_LVL);
		gpio->dir_save = kempld_read16(pld, KEMPLD_GPIO_DIR);
	}
	kempld_release_mutex(pld);

	return 0;
}

static int kempld_gpio_resume(struct device *dev)
{
	struct kempld_gpio_data *gpio = dev_get_drvdata(dev);
	struct kempld_device_data *pld = dev_get_drvdata(dev->parent);

	dev_dbg(pld->dev, "Resuming KEMPLD GPIO driver\n");

	if (!restore_state)
		return 0;

	dev_dbg(pld->dev, "Restoring KEMPLD GPIO configuration\n");

	kempld_get_mutex(pld);

	kempld_write16(pld, KEMPLD_GPIO_LVL, gpio->lvl_save);
	kempld_write16(pld, KEMPLD_GPIO_DIR, gpio->dir_save);

	kempld_release_mutex(pld);

	kempld_gpio_setup_event(gpio);

	return 0;
}
#endif

static int kempld_gpio_remove(struct platform_device *pdev)
{
	struct kempld_gpio_data *gpio = platform_get_drvdata(pdev);
	struct kempld_device_data *pld = gpio->pld;

	kempld_free_irq_num(pld, gpio->irq);

	return 0;
}

static const struct dev_pm_ops kempld_gpio_pm_ops = {
#ifdef CONFIG_PM_SLEEP
	.suspend = kempld_gpio_suspend,
	.resume = kempld_gpio_resume,
	.poweroff =  kempld_gpio_suspend,
	.restore = kempld_gpio_resume,
#endif
};

static struct platform_driver kempld_gpio_driver = {
	.driver = {
		.name = "kempld-gpio",
		.owner = THIS_MODULE,
		.pm = &kempld_gpio_pm_ops,
	},
	.probe		= kempld_gpio_probe,
	.remove		= kempld_gpio_remove,
};

static int __init kempld_gpio_init(void)
{
	return platform_driver_register(&kempld_gpio_driver);
}

static void __exit kempld_gpio_exit(void)
{
	platform_driver_unregister(&kempld_gpio_driver);
}

module_init(kempld_gpio_init);
module_exit(kempld_gpio_exit);

MODULE_DESCRIPTION("KEM PLD GPIO Driver");
MODULE_AUTHOR("Michael Brunner <michael.brunner@kontron.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:kempld-gpio");
MODULE_VERSION("36.0");
