// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kontron PLD MFD core driver
 *
 * Copyright (c) 2010-2018 Kontron Europe GmbH
 * Author: Michael Brunner <michael.brunner@kontron.com>
 */

#include <linux/platform_device.h>
#include <linux/mfd/core.h>
#include <linux/mfd/kempld.h>
#include <linux/module.h>
#include <linux/dmi.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/acpi.h>

#define MAX_ID_LEN 4
static char force_device_id[MAX_ID_LEN + 1] = "";
module_param_string(force_device_id, force_device_id,
		    sizeof(force_device_id), 0);
MODULE_PARM_DESC(force_device_id, "Override detected product");

static int eeep = -1;
module_param(eeep, int, 0444);
MODULE_PARM_DESC(eeep,
		 "Override EEEP platform default (0=none, 1=COMe, 2=COMe+Backplane, 3=Backplane)");

static int ddc = -1;
module_param(ddc, int, 0444);
MODULE_PARM_DESC(ddc, "Override DDC platform default (none=0, COMe=1)");

static bool autoirq;
module_param(autoirq, bool, 0444);
MODULE_PARM_DESC(autoirq, "Try to assign irq automatically from BIOS irq pool");

/*
 * this option is only here for debugging and should never be needed in
 * production environments
 */
static bool force_unlock;
module_param(force_unlock, bool, 0444);
MODULE_PARM_DESC(force_unlock, "Force breaking the semaphore on driver load");

/*
 * GPIO names arrays are expected to include 16 entries
 */
static const char *kempld_gpio_names_generic[16] = {
	"GPIO0", "GPIO1", "GPIO2", "GPIO3", "GPIO4", "GPIO5", "GPIO6", "GPIO7",
	"GPIO9", "GPIO9", "GPIO10", "GPIO11", "GPIO12", "GPIO13", "GPIO14",
	"GPIO15"
};

static const char *kempld_gpio_names_sxal[16] = {
	"GPIO0_CAM0_PWR_N", "GPIO1_CAM1_PWR_N", "GPIO2_CAM0_RST_N",
	"GPIO3_CAM1_RST_N", "GPIO4_HDA_RST_N", "GPIO5_PWM_OUT", "GPIO6_TACHIN",
	"GPIO7", "GPIO8", "GPIO9", "GPIO10", "GPIO11"
};

static const char *kempld_gpio_names_sxel[16] = {
	"GPIO0", "GPIO1", "GPIO2", "GPIO3",
	"GPIO4_HDA_RST_N", "GPIO5_PWM_OUT", "GPIO6_TACHIN",
	"GPIO7", "GPIO8", "GPIO9", "GPIO10", "GPIO11", "GPIO12", "GPIO13",
	"GPIO14", "GPIO15"
};

static const char * const lrc_strings[] = {
	"power-on",
	"external",
	"watchdog",
	"over-temperature",
	"power-good-fail",
	"software",
	"other",
	NULL
};

/*
 * Get hardware mutex to block firmware from accessing the pld.
 * It is possible for the firmware may hold the mutex for an extended length of
 * time. This function will block until access has been granted.
 */
static void kempld_get_hardware_mutex(struct kempld_device_data *pld)
{
	u8 index;

	/* The mutex bit will read 1 until access has been granted */
	while ((index = ioread8(pld->io_index)) & KEMPLD_MUTEX_KEY)
		usleep_range(1000, 3000);

	pld->last_index = index;
}

static void kempld_release_hardware_mutex(struct kempld_device_data *pld)
{
	/* The harware mutex is released when 1 is written to the mutex bit. */
	iowrite8(KEMPLD_MUTEX_KEY | pld->last_index, pld->io_index);
}

static int kempld_get_info_generic(struct kempld_device_data *pld)
{
	u16 version;
	u8 spec;

	kempld_get_mutex(pld);

	version = kempld_read16(pld, KEMPLD_VERSION);
	spec = kempld_read8(pld, KEMPLD_SPEC);
	pld->info.buildnr = kempld_read16(pld, KEMPLD_BUILDNR);

	pld->info.minor = KEMPLD_VERSION_GET_MINOR(version);
	pld->info.major = KEMPLD_VERSION_GET_MAJOR(version);
	pld->info.number = KEMPLD_VERSION_GET_NUMBER(version);
	pld->info.type = KEMPLD_VERSION_GET_TYPE(version);

	if (spec == 0xff) {
		pld->info.spec_minor = 0;
		pld->info.spec_major = 1;
	} else {
		pld->info.spec_minor = KEMPLD_SPEC_GET_MINOR(spec);
		pld->info.spec_major = KEMPLD_SPEC_GET_MAJOR(spec);
	}

	if (pld->info.spec_major > 0)
		pld->feature_mask = kempld_read16(pld, KEMPLD_FEATURE);
	else
		pld->feature_mask = 0;

	kempld_release_mutex(pld);

	return 0;
}

enum kempld_cells {
	KEMPLD_I2C = 0,
	KEMPLD_WDT,
	KEMPLD_GPIO,
	KEMPLD_UART,
};

static const char *kempld_dev_names[] = {
	[KEMPLD_I2C] = "kempld-i2c",
	[KEMPLD_WDT] = "kempld-wdt",
	[KEMPLD_GPIO] = "kempld-gpio",
	[KEMPLD_UART] = "kempld-uart",
};

#define KEMPLD_MAX_DEVS	ARRAY_SIZE(kempld_dev_names)

static int kempld_register_cells_generic(struct kempld_device_data *pld)
{
	struct mfd_cell devs[KEMPLD_MAX_DEVS] = {};
	int i = 0;

	if (pld->feature_mask & KEMPLD_FEATURE_BIT_I2C)
		devs[i++].name = kempld_dev_names[KEMPLD_I2C];

	if (pld->feature_mask & KEMPLD_FEATURE_BIT_WATCHDOG)
		devs[i++].name = kempld_dev_names[KEMPLD_WDT];

	if (pld->feature_mask & KEMPLD_FEATURE_BIT_GPIO)
		devs[i++].name = kempld_dev_names[KEMPLD_GPIO];

	if (pld->feature_mask & KEMPLD_FEATURE_MASK_UART)
		devs[i++].name = kempld_dev_names[KEMPLD_UART];

	return mfd_add_devices(pld->dev, -1, devs, i, NULL, 0, NULL);
}

static struct resource kempld_ioresource = {
	.start	= KEMPLD_IOINDEX,
	.end	= KEMPLD_IODATA,
	.flags	= IORESOURCE_IO,
};

/* this is the default CPLD configuration unless something else is defined */
static const struct kempld_platform_data kempld_platform_data_generic = {
	.pld_clock		= KEMPLD_CLK,
	.ioresource		= &kempld_ioresource,
	.force_index_write	= 0,
	.eeep			= KEMPLD_EEEP_NONE,
	.ddc			= KEMPLD_DDC_NONE,
	.gpio_names		= kempld_gpio_names_generic,
	.get_hardware_mutex	= kempld_get_hardware_mutex,
	.release_hardware_mutex	= kempld_release_hardware_mutex,
	.get_info		= kempld_get_info_generic,
	.register_cells		= kempld_register_cells_generic,
};

static const struct kempld_platform_data kempld_platform_data_come = {
	.pld_clock		= KEMPLD_CLK,
	.ioresource		= &kempld_ioresource,
	.force_index_write	= 0,
	.eeep			= KEMPLD_EEEP_COME,
	.ddc			= KEMPLD_DDC_COME,
	.gpio_names		= kempld_gpio_names_generic,
	.get_hardware_mutex	= kempld_get_hardware_mutex,
	.release_hardware_mutex	= kempld_release_hardware_mutex,
	.get_info		= kempld_get_info_generic,
	.register_cells		= kempld_register_cells_generic,
};

static const struct kempld_platform_data kempld_platform_data_sxal = {
	.pld_clock		= KEMPLD_CLK,
	.ioresource		= &kempld_ioresource,
	.force_index_write	= 0,
	.eeep			= KEMPLD_EEEP_NONE,
	.ddc			= KEMPLD_DDC_NONE,
	.gpio_names		= kempld_gpio_names_sxal,
	.get_hardware_mutex	= kempld_get_hardware_mutex,
	.release_hardware_mutex	= kempld_release_hardware_mutex,
	.get_info		= kempld_get_info_generic,
	.register_cells		= kempld_register_cells_generic,
};

static const struct kempld_platform_data kempld_platform_data_sxel = {
	.pld_clock		= KEMPLD_CLK,
	.ioresource		= &kempld_ioresource,
	.force_index_write	= 0,
	.eeep			= KEMPLD_EEEP_COME,
	.ddc			= KEMPLD_DDC_COME,
	.gpio_names		= kempld_gpio_names_sxel,
	.get_hardware_mutex	= kempld_get_hardware_mutex,
	.release_hardware_mutex	= kempld_release_hardware_mutex,
	.get_info		= kempld_get_info_generic,
	.register_cells		= kempld_register_cells_generic,
};

static struct platform_device *kempld_pdev;

static int kempld_create_platform_device(const struct dmi_system_id *id)
{
	struct kempld_platform_data *pdata = id->driver_data;
	int ret;

	kempld_pdev = platform_device_alloc("kempld", -1);
	if (!kempld_pdev)
		return -ENOMEM;

	ret = platform_device_add_data(kempld_pdev, pdata, sizeof(*pdata));
	if (ret)
		goto err;

	ret = platform_device_add_resources(kempld_pdev, pdata->ioresource, 1);
	if (ret)
		goto err;

	ret = platform_device_add(kempld_pdev);
	if (ret)
		goto err;

	return 0;
err:
	platform_device_put(kempld_pdev);
	return ret;
}

/**
 * kempld_set_index -  change the current register index of the PLD
 * @pld: kempld_device_data structure describing the PLD
 * @pld:   kempld_device_data structure describing the PLD
 * @index: register index on the chip
 *
 * kempld_get_mutex must be called prior to calling this function.
 */
static void kempld_set_index(struct kempld_device_data *pld, u8 index)
{
	struct kempld_platform_data *pdata = dev_get_platdata(pld->dev);

	if (pld->last_index != index || pdata->force_index_write) {
		iowrite8(index, pld->io_index);
		pld->last_index = index;
	}
}

/**
 * kempld_request_irq_num - request free IRQ number from resource pool
 * @pld: kempld_device_data structure describing the PLD
 * @irq: requested irq number, 0 for next free one
 */
int kempld_request_irq_num(struct kempld_device_data *pld, int irq)
{
	struct platform_device *pdev = to_platform_device(pld->dev);
	struct resource *r;
	int i = 0;

	if ((irq == 0) && !autoirq)
		return  0;

	while ((r = platform_get_resource(pdev, IORESOURCE_IRQ, i++))) {
		if ((irq == 0) && !(r->flags & IORESOURCE_BUSY)) {
			r->flags |= IORESOURCE_BUSY;
			return r->start;
		} else if (r->start == irq) {
			if (r->flags & IORESOURCE_BUSY)
				return -EBUSY;
			else
				return irq;
		}
	}

	return -ENXIO;
}
EXPORT_SYMBOL_GPL(kempld_request_irq_num);

/**
 * kempld_free_irq_num - mark IRQ number from the resource pool as free
 * @pld: kempld_device_data structure describing the PLD
 * @irq: irq number
 */
void kempld_free_irq_num(struct kempld_device_data *pld, int irq)
{
	struct platform_device *pdev = to_platform_device(pld->dev);
	struct resource *r;
	int i = 0;

	while ((r = platform_get_resource(pdev, IORESOURCE_IRQ, i++))) {
		if (r->start == irq) {
			r->flags &= ~IORESOURCE_BUSY;
			return;
		}
	}
}
EXPORT_SYMBOL_GPL(kempld_free_irq_num);

/**
 * kempld_read8 - read 8 bit register
 * @pld: kempld_device_data structure describing the PLD
 * @index: register index on the chip
 *
 * kempld_get_mutex must be called prior to calling this function.
 */
u8 kempld_read8(struct kempld_device_data *pld, u8 index)
{
	kempld_set_index(pld, index);

	return ioread8(pld->io_data);
}
EXPORT_SYMBOL_GPL(kempld_read8);

u8 kempld_read8_shadow(struct kempld_device_data *pld, u8 index)
{

	return pld->shadow[index];
}
EXPORT_SYMBOL_GPL(kempld_read8_shadow);

/**
 * kempld_write8 - write 8 bit register
 * @pld: kempld_device_data structure describing the PLD
 * @index: register index on the chip
 * @data: new register value
 *
 * kempld_get_mutex must be called prior to calling this function.
 */
void kempld_write8(struct kempld_device_data *pld, u8 index, u8 data)
{
	kempld_set_index(pld, index);

	pld->shadow[index] = data;
	iowrite8(data, pld->io_data);
}
EXPORT_SYMBOL_GPL(kempld_write8);

/**
 * kempld_read16 - read 16 bit register
 * @pld: kempld_device_data structure describing the PLD
 * @index: register index on the chip
 *
 * kempld_get_mutex must be called prior to calling this function.
 */
u16 kempld_read16(struct kempld_device_data *pld, u8 index)
{
	return kempld_read8(pld, index) | kempld_read8(pld, index + 1) << 8;
}
EXPORT_SYMBOL_GPL(kempld_read16);

u16 kempld_read16_shadow(struct kempld_device_data *pld, u8 index)
{
	return kempld_read8_shadow(pld, index)
		| kempld_read8_shadow(pld, index + 1) << 8;
}
EXPORT_SYMBOL_GPL(kempld_read16_shadow);

/**
 * kempld_write16 - write 16 bit register
 * @pld: kempld_device_data structure describing the PLD
 * @index: register index on the chip
 * @data: new register value
 *
 * kempld_get_mutex must be called prior to calling this function.
 */
void kempld_write16(struct kempld_device_data *pld, u8 index, u16 data)
{
	kempld_write8(pld, index, (u8)data);
	kempld_write8(pld, index + 1, (u8)(data >> 8));
}
EXPORT_SYMBOL_GPL(kempld_write16);

/**
 * kempld_read32 - read 32 bit register
 * @pld: kempld_device_data structure describing the PLD
 * @index: register index on the chip
 *
 * kempld_get_mutex must be called prior to calling this function.
 */
u32 kempld_read32(struct kempld_device_data *pld, u8 index)
{
	return kempld_read16(pld, index) | kempld_read16(pld, index + 2) << 16;
}
EXPORT_SYMBOL_GPL(kempld_read32);

u32 kempld_read32_shadow(struct kempld_device_data *pld, u8 index)
{
	return kempld_read16_shadow(pld, index)
		| kempld_read16_shadow(pld, index + 2) << 16;
}
EXPORT_SYMBOL_GPL(kempld_read32_shadow);

/**
 * kempld_write32 - write 32 bit register
 * @pld: kempld_device_data structure describing the PLD
 * @index: register index on the chip
 * @data: new register value
 *
 * kempld_get_mutex must be called prior to calling this function.
 */
void kempld_write32(struct kempld_device_data *pld, u8 index, u32 data)
{
	kempld_write16(pld, index, (u16)data);
	kempld_write16(pld, index + 2, (u16)(data >> 16));
}
EXPORT_SYMBOL_GPL(kempld_write32);

/**
 * kempld_get_mutex - acquire PLD mutex
 * @pld: kempld_device_data structure describing the PLD
 */
void kempld_get_mutex(struct kempld_device_data *pld)
{
	struct kempld_platform_data *pdata = dev_get_platdata(pld->dev);

	mutex_lock(&pld->lock);
	pdata->get_hardware_mutex(pld);
}
EXPORT_SYMBOL_GPL(kempld_get_mutex);

/**
 * kempld_release_mutex - release PLD mutex
 * @pld: kempld_device_data structure describing the PLD
 */
void kempld_release_mutex(struct kempld_device_data *pld)
{
	struct kempld_platform_data *pdata = dev_get_platdata(pld->dev);

	pdata->release_hardware_mutex(pld);
	mutex_unlock(&pld->lock);
}
EXPORT_SYMBOL_GPL(kempld_release_mutex);

/**
 * kempld_get_info - update device specific information
 * @pld: kempld_device_data structure describing the PLD
 *
 * This function calls the configured board specific kempld_get_info_XXXX
 * function which is responsible for gathering information about the specific
 * hardware. The information is then stored within the pld structure.
 */
static int kempld_get_info(struct kempld_device_data *pld)
{
	int ret;
	struct kempld_platform_data *pdata = dev_get_platdata(pld->dev);
	char major, minor;

	ret = pdata->get_info(pld);
	if (ret)
		return ret;

	/* The Kontron PLD firmware version string has the following format:
	 * Pwxy.zzzz
	 *   P:    Fixed
	 *   w:    PLD number    - 1 hex digit
	 *   x:    Major version - 1 alphanumerical digit (0-9A-V)
	 *   y:    Minor version - 1 alphanumerical digit (0-9A-V)
	 *   zzzz: Build number  - 4 zero padded hex digits */

	if (pld->info.major < 10)
		major = pld->info.major + '0';
	else
		major = (pld->info.major - 10) + 'A';
	if (pld->info.minor < 10)
		minor = pld->info.minor + '0';
	else
		minor = (pld->info.minor - 10) + 'A';

	ret = scnprintf(pld->info.version, sizeof(pld->info.version),
			"P%X%c%c.%04X", pld->info.number, major, minor,
			pld->info.buildnr);
	if (ret < 0)
		return ret;

	return 0;
}

/**
 * kempld_get_last_reset_cause - get reason for last reset
 * @pld: kempld_device_data structure describing the PLD
 *
 * This function reads out the LRC register, if available, and stores the
 * information in the pld structure.
 */
static int kempld_get_last_reset_cause(struct kempld_device_data *pld)
{
	u8 lrc;

	if ((pld->info.spec_major < 2) ||
	    ((pld->info.spec_major == 2) && (pld->info.spec_minor < 7))) {
		pld->last_reset_cause = 0xff;
		return -ENXIO;
	}

	kempld_get_mutex(pld);
	lrc = kempld_read8(pld, KEMPLD_LRC);
	kempld_release_mutex(pld);
	if (lrc == 0xff) {
		pld->last_reset_cause = 0xff;
		return -ENXIO;
	}

	pld->last_reset_cause = lrc & KEMPLD_LRC_MASK;

	return 0;
}

/*
 * kempld_register_cells - register cell drivers
 *
 * This function registers cell drivers for the detected hardware by calling
 * the configured kempld_register_cells_XXXX function which is responsible
 * to detect and register the needed cell drivers.
 */
static int kempld_register_cells(struct kempld_device_data *pld)
{
	struct kempld_platform_data *pdata = dev_get_platdata(pld->dev);

	return pdata->register_cells(pld);
}

static const char *kempld_get_type_string(struct kempld_device_data *pld)
{
	const char *version_type;

	switch (pld->info.type) {
	case 0:
		version_type = "release";
		break;
	case 1:
		version_type = "debug";
		break;
	case 2:
		version_type = "custom";
		break;
	default:
		version_type = "unspecified";
		break;
	}

	return version_type;
}

static ssize_t kempld_build_lrc_string(struct kempld_device_data *pld,
				       char *buf, ssize_t length)
{
	ssize_t count = 0;
	int i;

	if (pld->last_reset_cause == 0)
		return scnprintf(buf, length, "0x00 (unknown)\n");
	else if (pld->last_reset_cause == 0xff)
		return scnprintf(buf, length, "0xff (not-supported)\n");

	count = scnprintf(buf, length, "0x%02x (", pld->last_reset_cause);

	i = 0;
	do {
		if (pld->last_reset_cause & (1 << i))
			count += scnprintf(&buf[count],
					   (length - count - 2),
					  "%s|", lrc_strings[i]);
	} while (lrc_strings[++i]);

	count--;
	count += scnprintf(&buf[count], (length - count - 2), ")\n");

	return count;
}

static ssize_t pld_version_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct kempld_device_data *pld = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", pld->info.version);
}

static ssize_t pld_specification_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct kempld_device_data *pld = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d.%d\n", pld->info.spec_major, pld->info.spec_minor);
}

static ssize_t pld_type_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct kempld_device_data *pld = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", kempld_get_type_string(pld));
}

static ssize_t last_reset_cause_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return kempld_build_lrc_string(dev_get_drvdata(dev), buf, PAGE_SIZE);
}

static ssize_t active_bios_cs_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct kempld_device_data *pld = dev_get_drvdata(dev);
	const char *cs_str = "normal";
	const char *sel_str;
	u8 cfg;

	if ((pld->info.spec_major > 2) ||
	    ((pld->info.spec_major == 2) && (pld->info.spec_minor >= 7))) {
		kempld_get_mutex(pld);
		cfg = kempld_read8(pld, KEMPLD_CFG);
		kempld_release_mutex(pld);
		if (cfg & KEMPLD_CFG_ACTIVE_BIOS_CS)
			cs_str = "secondary";
		if (cfg & KEMPLD_CFG_SBSO)
			sel_str = "override";
		else
			sel_str = "auto";
	} else
		sel_str = "not supported";

	return sysfs_emit(buf, "%s (%s)\n", cs_str, sel_str);
}

static int kempld_get_bios_set_protect(struct kempld_device_data *pld)
{
	u8 cfg;

	if ((pld->info.spec_major > 2) ||
	    ((pld->info.spec_major == 2) && (pld->info.spec_minor >= 9))) {
		kempld_get_mutex(pld);
		cfg = kempld_read8(pld, KEMPLD_CFG);
		kempld_release_mutex(pld);
		return !!(cfg & KEMPLD_CFG_BIOS_SET_PROTECT);
	} else
		return -ENXIO;
}

static ssize_t active_bios_cs_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct kempld_device_data *pld = dev_get_drvdata(dev);
	u8 cfg, cfg_cur;
	int ret = 0;

	if ((pld->info.spec_major < 2) ||
	    ((pld->info.spec_major == 2) && (pld->info.spec_minor < 7)))
		return -ENXIO;

	if (kempld_get_bios_set_protect(pld) > 0)
		return -EACCES;

	kempld_get_mutex(pld);
	cfg = kempld_read8(pld, KEMPLD_CFG);

	if (sysfs_streq(buf, "secondary"))
		cfg |= KEMPLD_CFG_ACTIVE_BIOS_CS | KEMPLD_CFG_SBSO;
	else if (sysfs_streq(buf, "normal")) {
		cfg &= ~KEMPLD_CFG_ACTIVE_BIOS_CS;
		cfg |= KEMPLD_CFG_SBSO;
	} else if (sysfs_streq(buf, "auto"))
		cfg &= ~KEMPLD_CFG_SBSO;
	else
		ret = -EINVAL;

	kempld_write8(pld, KEMPLD_CFG, cfg);
	cfg_cur = kempld_read8(pld, KEMPLD_CFG);
	kempld_release_mutex(pld);

	if (cfg_cur != cfg)
		ret = -ENXIO;

	return ret;
}

static ssize_t bios_set_protect_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct kempld_device_data *pld = dev_get_drvdata(dev);
	int bios_set_protect = kempld_get_bios_set_protect(pld);
	const char *str;

	if (bios_set_protect > 0)
		str = "on";
	else if (bios_set_protect == 0)
		str = "off";
	else
		str = "not supported";

	return sysfs_emit(buf, "%s\n", str);
}

static ssize_t bios_set_protect_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct kempld_device_data *pld = dev_get_drvdata(dev);
	u8 cfg, cfg_cur;
	int ret;

	ret = kempld_get_bios_set_protect(pld);
	if (ret < 0)
		return ret;

	kempld_get_mutex(pld);
	cfg = kempld_read8(pld, KEMPLD_CFG);

	if (sysfs_streq(buf, "on"))
		cfg |= KEMPLD_CFG_BIOS_SET_PROTECT;
	else if (sysfs_streq(buf, "off"))
		/*
		 * On production PLDs it should not be possible to set this
		 * bit to zero, therefore this can be used to verify if the
		 * BIOS related setting protection actually works
		 */
		cfg &= ~KEMPLD_CFG_BIOS_SET_PROTECT;
	else
		ret = -EINVAL;

	kempld_write8(pld, KEMPLD_CFG, cfg);
	cfg_cur = kempld_read8(pld, KEMPLD_CFG);
	kempld_release_mutex(pld);

	if (cfg_cur != cfg)
		ret = -EACCES;

	return ret;
}

static DEVICE_ATTR_RO(pld_version);
static DEVICE_ATTR_RO(pld_specification);
static DEVICE_ATTR_RO(pld_type);
static DEVICE_ATTR_RO(last_reset_cause);
static DEVICE_ATTR_RW(active_bios_cs);
static DEVICE_ATTR_RW(bios_set_protect);

static struct attribute *pld_attributes[] = {
	&dev_attr_pld_version.attr,
	&dev_attr_pld_specification.attr,
	&dev_attr_pld_type.attr,
	&dev_attr_last_reset_cause.attr,
	&dev_attr_active_bios_cs.attr,
	&dev_attr_bios_set_protect.attr,
	NULL
};

static const struct attribute_group pld_attr_group = {
	.attrs = pld_attributes,
};

static int kempld_detect_device(struct kempld_device_data *pld)
{
	u8 index_reg;
	int ret;
	int timeout = 1000;

	mutex_lock(&pld->lock);

	/* Check for empty IO space */
	index_reg = ioread8(pld->io_index);
	if (index_reg == 0xff && ioread8(pld->io_data) == 0xff) {
		mutex_unlock(&pld->lock);
		return -ENODEV;
	}

	/* Try to aquire mutex if not already done */
	while (index_reg & KEMPLD_MUTEX_KEY && timeout--) {
		usleep_range(1000, 3000);
		index_reg = ioread8(pld->io_index);
	}
	if (index_reg & KEMPLD_MUTEX_KEY) {
		dev_err(pld->dev, "HW mutex timed out\n");
		if (!force_unlock) {
			mutex_unlock(&pld->lock);
			return -ENODEV;
		}
		dev_warn(pld->dev, "force_unlock enabled - releasing mutex\n");
		index_reg = 0;
	}

	/* Release hardware mutex if acquired */
	if (!(index_reg & KEMPLD_MUTEX_KEY)) {
		iowrite8(KEMPLD_MUTEX_KEY, pld->io_index);
		/* PXT and COMe-cPC2 boards may require a second release */
		iowrite8(KEMPLD_MUTEX_KEY, pld->io_index);
	}

	mutex_unlock(&pld->lock);

	ret = kempld_get_info(pld);
	if (ret)
		return ret;

	dev_info(pld->dev, "Found Kontron PLD - %s (%s), spec %d.%d\n",
		 pld->info.version, kempld_get_type_string(pld),
		 pld->info.spec_major, pld->info.spec_minor);

	ret = kempld_get_last_reset_cause(pld);
	if (ret == 0) {
		char buf[128];

		kempld_build_lrc_string(pld, buf, 128);
		dev_info(pld->dev, "Last reset cause: %s", buf);
	}

	ret = sysfs_create_group(&pld->dev->kobj, &pld_attr_group);
	if (ret)
		return ret;

	ret = kempld_register_cells(pld);
	if (ret)
		sysfs_remove_group(&pld->dev->kobj, &pld_attr_group);

	return ret;
}

#ifdef CONFIG_ACPI
static int kempld_get_acpi_data(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct kempld_platform_data *pdata;
	int ret;

	pdata = acpi_device_get_match_data(dev);
	ret = platform_device_add_data(pdev, pdata,
				       sizeof(struct kempld_platform_data));
	if (ret)
		return ret;

	if (pdev->resource == NULL)
		ret = platform_device_add_resources(pdev, pdata->ioresource, 1);

	return ret;
}
#else
static int kempld_get_acpi_data(struct platform_device *pdev)
{
	return -ENODEV;
}
#endif /* CONFIG_ACPI */

static int kempld_probe(struct platform_device *pdev)
{
	const struct kempld_platform_data *pdata;
	struct device *dev = &pdev->dev;
	struct kempld_device_data *pld;
	struct resource *ioport;
	int ret;

	if (kempld_pdev == NULL) {
		/*
		 * No kempld_pdev device has been registered in kempld_init,
		 * so we seem to be probing an ACPI platform device.
		 */
		ret = kempld_get_acpi_data(pdev);
		if (ret)
			return ret;
	} else if (kempld_pdev != pdev) {
		/*
		 * The platform device we are probing is not the one we
		 * registered in kempld_init using the DMI table, so this one
		 * comes from ACPI.
		 * As we can only probe one - abort here and use the DMI
		 * based one instead.
		 */
		dev_notice(dev, "platform device exists - not using ACPI\n");
		return -ENODEV;
	}
	pdata = dev_get_platdata(dev);

	pld = devm_kzalloc(dev, sizeof(*pld), GFP_KERNEL);
	if (!pld)
		return -ENOMEM;

	ioport = platform_get_resource(pdev, IORESOURCE_IO, 0);
	if (!ioport)
		return -EINVAL;

	pld->io_base = devm_ioport_map(dev, ioport->start,
					resource_size(ioport));
	if (!pld->io_base)
		return -ENOMEM;

	pld->io_index = pld->io_base;
	pld->io_data = pld->io_base + 1;
	pld->pld_clock = pdata->pld_clock;

	if (ddc < 0)
		pld->ddc = pdata->ddc;
	else
		pld->ddc = ddc;

	if (eeep < 0)
		pld->eeep = pdata->eeep;
	else
		pld->eeep = eeep;

	pld->dev = dev;

	mutex_init(&pld->lock);
	platform_set_drvdata(pdev, pld);

	return kempld_detect_device(pld);
}

static int kempld_remove(struct platform_device *pdev)
{
	struct kempld_device_data *pld = platform_get_drvdata(pdev);
	struct kempld_platform_data *pdata = dev_get_platdata(pld->dev);

	sysfs_remove_group(&pld->dev->kobj, &pld_attr_group);

	mfd_remove_devices(&pdev->dev);
	pdata->release_hardware_mutex(pld);

	return 0;
}

#ifdef CONFIG_ACPI
static const struct acpi_device_id kempld_acpi_table[] = {
	{ "KEM0000", (kernel_ulong_t)&kempld_platform_data_generic },
	{ "KEM0001", (kernel_ulong_t)&kempld_platform_data_come },
	{}
};
MODULE_DEVICE_TABLE(acpi, kempld_acpi_table);
#endif

static struct platform_driver kempld_driver = {
	.driver		= {
		.name	= "kempld",
		.acpi_match_table = ACPI_PTR(kempld_acpi_table),
	},
	.probe		= kempld_probe,
	.remove		= kempld_remove,
};

static struct dmi_system_id kempld_dmi_table[] __initdata = {
	{
		.ident = "BBD6",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "COMe-bBD"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "BBL6",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "COMe-bBL6"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "BDV7",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "COMe-bDV7"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "BHL6",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "COMe-bHL6"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "BKL6",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "COMe-bKL6"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "BSL6",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "COMe-bSL6"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "CAL6",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "COMe-cAL"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "CBL6",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "COMe-cBL6"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "CBW6",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "COMe-cBW6"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "CCR2",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "COMe-bIP2"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "CCR6",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "COMe-bIP6"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "CDV7",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "COMe-cDV7"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "CHL6",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "COMe-cHL6"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "CHR2",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "ETXexpress-SC T2"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "CHR2",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "ETXe-SC T2"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "CHR2",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "COMe-bSC2"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "CHR6",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "ETXexpress-SC T6"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "CHR6",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "ETXe-SC T6"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "CHR6",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "COMe-bSC6"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "CKL6",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "COMe-cKL6"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "CNTG",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "ETXexpress-PC"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "CNTG",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "COMe-bPC2"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "CNTX",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "PXT"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "CSL6",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "COMe-cSL6"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "CVV6",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "COMe-cBT"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "FRI2",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BIOS_VERSION, "FRI2"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "FRI2",
		.matches = {
			DMI_MATCH(DMI_PRODUCT_NAME, "Fish River Island II"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "A203",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "KBox A-203"),
		},
		.driver_data = (void *)&kempld_platform_data_generic,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "M4A1",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "COMe-m4AL"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "MAL1",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "COMe-mAL10"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "MAPL",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "mITX-APL"),
		},
		.driver_data = (void *)&kempld_platform_data_generic,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "MBR1",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "ETX-OH"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "MVV1",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "COMe-mBT"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "NTC1",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "nanoETXexpress-TT"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "NTC1",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "nETXe-TT"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "NTC1",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "COMe-mTT"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "NUP1",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "COMe-mCT"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "PAPL",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "pITX-APL"),
		},
		.driver_data = (void *)&kempld_platform_data_generic,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "SXAL",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "SMARC-sXAL"),
		},
		.driver_data = (void *)&kempld_platform_data_sxal,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "SXAL4",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "SMARC-sXA4"),
		},
		.driver_data = (void *)&kempld_platform_data_sxal,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "SXEL",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "SMARC-SXEL"),
		},
		.driver_data = (void *)&kempld_platform_data_sxel,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "UNP1",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "microETXexpress-DC"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "UNP1",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "COMe-cDC2"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "UNTG",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "microETXexpress-PC"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "UNTG",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "COMe-cPC2"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "UUP6",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "COMe-cCT6"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "UTH6",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "COMe-cTH6"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "Q7AL",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "Qseven-Q7AL"),
		},
		.driver_data = (void *)&kempld_platform_data_generic,
		.callback = kempld_create_platform_device,
	},
	/* The following are dummy entries, not representing actual products */
	{
		.ident = "come",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "*come*"),
		},
		.driver_data = (void *)&kempld_platform_data_come,
		.callback = kempld_create_platform_device,
	}, {
		.ident = "gene",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Kontron"),
			DMI_MATCH(DMI_BOARD_NAME, "*generic*"),
		},
		.driver_data = (void *)&kempld_platform_data_generic,
		.callback = kempld_create_platform_device,
	},
	{}
};
MODULE_DEVICE_TABLE(dmi, kempld_dmi_table);

static int __init kempld_init(void)
{
	const struct dmi_system_id *id;

	if (force_device_id[0]) {
		for (id = kempld_dmi_table;
		     id->matches[0].slot != DMI_NONE; id++)
			if (strstr(id->ident, force_device_id))
				if (id->callback && !id->callback(id))
					break;
		if (id->matches[0].slot == DMI_NONE)
			return -ENODEV;
	} else {
		dmi_check_system(kempld_dmi_table);
	}

	return platform_driver_register(&kempld_driver);
}

static void __exit kempld_exit(void)
{
	if (kempld_pdev)
		platform_device_unregister(kempld_pdev);

	platform_driver_unregister(&kempld_driver);
}

module_init(kempld_init);
module_exit(kempld_exit);

MODULE_DESCRIPTION("KEM PLD Core Driver");
MODULE_AUTHOR("Michael Brunner <michael.brunner@kontron.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:kempld-core");
MODULE_VERSION("36.0");
