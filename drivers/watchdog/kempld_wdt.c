// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kontron PLD watchdog driver
 *
 * Copyright (c) 2010-2013 Kontron Europe GmbH
 * Author: Michael Brunner <michael.brunner@kontron.com>
 *
 * Note: From the PLD watchdog point of view timeout and pretimeout are
 *       defined differently than in the kernel.
 *       First the pretimeout stage runs out before the timeout stage gets
 *       active.
 *
 * Kernel/API:                     P-----| pretimeout
 *               |-----------------------T timeout
 * Watchdog:     |-----------------P       pretimeout_stage
 *                                 |-----T timeout_stage
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/uaccess.h>
#include <linux/watchdog.h>
#include <linux/platform_device.h>
#include <linux/mfd/kempld.h>
#include <linux/nmi.h>

#define KEMPLD_WDT_STAGE_TIMEOUT(x)	(0x1b + (x) * 4)
#define KEMPLD_WDT_STAGE_CFG(x)		(0x18 + (x))
#define STAGE_CFG_GET_PRESCALER(x)	(((x) & 0x30) >> 4)
#define STAGE_CFG_SET_PRESCALER(x)	(((x) & 0x3) << 4)
#define STAGE_CFG_PRESCALER_MASK	0x30
#define STAGE_CFG_ACTION_MASK		0xf

#define KEMPLD_WDT_MAX_STAGES		2
#define KEMPLD_WDT_KICK			0x16
#define KEMPLD_WDT_CFG			0x17
#define KEMPLD_WDT_CFG_GLOBAL_LOCK	(1 << 7)
#define KEMPLD_WDT_CFG_AUTO_RELOAD_EN	(1 << 6)
#define KEMPLD_WDT_CFG_ENABLE		(1 << 4)
#define KEMPLD_WDT_CFG_ENABLE_LOCK	(1 << 3)
#define KEMPLD_WDT_CFG_STAGE_TOUT(x)	(1 << x)

enum {
	ACTION_NONE = 0,
	ACTION_RESET,
	ACTION_NMI,
	ACTION_SMI,
	ACTION_SCI,
	ACTION_DELAY,
	ACTION_WDT,
	ACTION_RESERVED1,
	ACTION_RESERVED2,
	ACTION_RESET_WDT,
	ACTION_NMI_WDT,
	ACTION_SMI_WDT,
	ACTION_SCI_WDT,
	ACTION_DELAY_WDT,
	ACTION_RESERVED3,
	ACTION_RESERVED4,
};

enum {
	STAGE_TIMEOUT = 0,
	STAGE_PRETIMEOUT,
};

enum {
	PRESCALER_21 = 0,
	PRESCALER_17,
	PRESCALER_12,
};

static const u32 kempld_prescaler[] = {
	[PRESCALER_21] = (1 << 21) - 1,
	[PRESCALER_17] = (1 << 17) - 1,
	[PRESCALER_12] = (1 << 12) - 1,
	0,
};

struct kempld_wdt_stage {
	unsigned int	id;
	u32		mask;
};

struct kempld_wdt_data {
	struct kempld_device_data	*pld;
	struct watchdog_device		wdd;
	struct kempld_wdt_stage		stage[KEMPLD_WDT_MAX_STAGES];
	bool				locked;
#ifdef CONFIG_PM
	u8				pm_status_store;
#endif
};

#define DEFAULT_TIMEOUT			30 /* seconds */
#define DEFAULT_PRETIMEOUT		0
#define DEFAULT_PRETIMEOUT_ACTION	ACTION_NMI
#define DEFAULT_TIMEOUT_ACTION		ACTION_RESET_WDT

static unsigned int timeout = DEFAULT_TIMEOUT;
module_param(timeout, uint, 0444);
MODULE_PARM_DESC(timeout,
	"Watchdog timeout in seconds. (>=0, default="
	__MODULE_STRING(DEFAULT_TIMEOUT) ")");

static unsigned int timeout_action = DEFAULT_TIMEOUT_ACTION;
module_param(timeout_action, uint, 0664);
MODULE_PARM_DESC(timeout_action,
	"Watchdog timeout action. (0-10, default="
	__MODULE_STRING(DEFAULT_TIMEOUT_ACTION)
	", none=0, reset=1, NMI=2, SMI=3, SCI=4, delay=5, WDT=8, reset+WDT=9, NMI+WDT=10, SMI+WDT=11, SCI+WDT=12, delay+WDT=13)"
	);

static unsigned int pretimeout = DEFAULT_PRETIMEOUT;
module_param(pretimeout, uint, 0444);
MODULE_PARM_DESC(pretimeout,
	"Watchdog pretimeout in seconds. (>=0, default="
	__MODULE_STRING(DEFAULT_PRETIMEOUT) ")");

static unsigned int pretimeout_action = DEFAULT_PRETIMEOUT_ACTION;
module_param(pretimeout_action, uint, 0664);
MODULE_PARM_DESC(pretimeout_action,
	"Watchdog pretimeout action. (0-10, default="
	__MODULE_STRING(DEFAULT_PRETIMEOUT_ACTION)
	", none=0, reset=1, NMI=2, SMI=3, SCI=4, delay=5, WDT=8, reset+WDT=9, NMI+WDT=10, SMI+WDT=11, SCI+WDT=12, delay+WDT=13)"
	);

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0444);
MODULE_PARM_DESC(nowayout,
	"Watchdog cannot be stopped once started (default="
	__MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

static bool updateonrestart = true;
module_param(updateonrestart, bool, 0444);
MODULE_PARM_DESC(updateonrestart,
	"Update timeouts of watchdog active on boottime on first start (default="
	__MODULE_STRING(updateonrestart) ")");

static int kempld_wdt_get_bootstatus(struct watchdog_device *wdd)
{
	struct kempld_wdt_data *wdt_data = watchdog_get_drvdata(wdd);
	struct kempld_device_data *pld = wdt_data->pld;

	if (!pld->last_reset_cause)
		return -ENXIO;

	if (pld->last_reset_cause & KEMPLD_LRC_TEMP)
		wdd->bootstatus |= WDIOF_OVERHEAT;

	if (pld->last_reset_cause & KEMPLD_LRC_EXT)
		wdd->bootstatus |= WDIOF_EXTERN1;

	if (pld->last_reset_cause & KEMPLD_LRC_PWRGOOD)
		wdd->bootstatus |= WDIOF_POWERUNDER;

	if (pld->last_reset_cause & KEMPLD_LRC_WDT)
		wdd->bootstatus |= WDIOF_CARDRESET;

	return 0;
}

static int kempld_wdt_check_stage_action(struct kempld_wdt_data *wdt_data,
					u8 action)
{
	struct kempld_device_data *pld = wdt_data->pld;
	int ret = 0;

	switch (action) {
	case ACTION_NMI:
	case ACTION_NMI_WDT:
		if (!(pld->feature_mask & KEMPLD_FEATURE_BIT_NMI))
			ret = -EINVAL;
		break;
	case ACTION_SMI:
	case ACTION_SMI_WDT:
		if (!(pld->feature_mask & KEMPLD_FEATURE_BIT_SMI))
			ret = -EINVAL;
		break;
	case ACTION_SCI:
	case ACTION_SCI_WDT:
		if (!(pld->feature_mask & KEMPLD_FEATURE_BIT_SMI))
			ret = -EINVAL;
		break;
	case ACTION_RESERVED1:
	case ACTION_RESERVED2:
	case ACTION_RESERVED3:
	case ACTION_RESERVED4:
		ret = -EINVAL;
		break;
	default:
		break;
	}

	return ret;
}

static int kempld_wdt_set_stage_action(struct kempld_wdt_data *wdt_data,
					struct kempld_wdt_stage *stage,
					u8 action)
{
	struct kempld_device_data *pld = wdt_data->pld;
	u8 stage_cfg;

	if (!stage || !stage->mask || action > ACTION_RESERVED4)
		return -EINVAL;

	kempld_get_mutex(pld);
	stage_cfg = kempld_read8(pld, KEMPLD_WDT_STAGE_CFG(stage->id));
	stage_cfg &= ~STAGE_CFG_ACTION_MASK;
	stage_cfg |= action;

	kempld_write8(pld, KEMPLD_WDT_STAGE_CFG(stage->id), stage_cfg);
	kempld_release_mutex(pld);

	return 0;
}

static int kempld_wdt_get_stage_action(struct kempld_wdt_data *wdt_data,
					struct kempld_wdt_stage *stage)
{
	struct kempld_device_data *pld = wdt_data->pld;
	u8 stage_cfg;

	if (!stage || !stage->mask)
		return -EINVAL;

	kempld_get_mutex(pld);
	stage_cfg = kempld_read8(pld, KEMPLD_WDT_STAGE_CFG(stage->id));
	kempld_release_mutex(pld);

	return stage_cfg & STAGE_CFG_ACTION_MASK;
}

static int kempld_wdt_set_stage_timeout(struct kempld_wdt_data *wdt_data,
					struct kempld_wdt_stage *stage,
					unsigned int timeout)
{
	struct kempld_device_data *pld = wdt_data->pld;
	u32 prescaler;
	u64 stage_timeout64;
	u32 stage_timeout;
	u32 remainder;
	u8 stage_cfg;

	prescaler = kempld_prescaler[PRESCALER_21];

	if (!stage)
		return -EINVAL;

	stage_timeout64 = (u64)timeout * pld->pld_clock;
	remainder = do_div(stage_timeout64, prescaler);
	if (remainder)
		stage_timeout64++;

	if (stage_timeout64 > stage->mask)
		return -EINVAL;

	stage_timeout = stage_timeout64 & stage->mask;

	kempld_get_mutex(pld);
	stage_cfg = kempld_read8(pld, KEMPLD_WDT_STAGE_CFG(stage->id));
	stage_cfg &= ~STAGE_CFG_PRESCALER_MASK;
	stage_cfg |= STAGE_CFG_SET_PRESCALER(PRESCALER_21);
	kempld_write8(pld, KEMPLD_WDT_STAGE_CFG(stage->id), stage_cfg);
	kempld_write32(pld, KEMPLD_WDT_STAGE_TIMEOUT(stage->id),
			stage_timeout);
	kempld_release_mutex(pld);

	return 0;
}

/*
 * kempld_get_mutex must be called prior to calling this function.
 */
static unsigned int kempld_wdt_get_timeout(struct kempld_wdt_data *wdt_data,
						struct kempld_wdt_stage *stage)
{
	struct kempld_device_data *pld = wdt_data->pld;
	unsigned int timeout;
	u64 stage_timeout;
	u32 prescaler;
	u32 remainder;
	u8 stage_cfg;

	if (!stage->mask)
		return 0;

	stage_cfg = kempld_read8(pld, KEMPLD_WDT_STAGE_CFG(stage->id));
	stage_timeout = kempld_read32(pld, KEMPLD_WDT_STAGE_TIMEOUT(stage->id));
	prescaler = kempld_prescaler[STAGE_CFG_GET_PRESCALER(stage_cfg)];

	stage_timeout = (stage_timeout & stage->mask) * prescaler;
	remainder = do_div(stage_timeout, pld->pld_clock);

	timeout = stage_timeout;
	WARN_ON_ONCE(timeout != stage_timeout);

	return timeout;
}

static int kempld_wdt_set_timeout(struct watchdog_device *wdd,
					unsigned int timeout)
{
	struct kempld_wdt_data *wdt_data = watchdog_get_drvdata(wdd);
	struct kempld_wdt_stage *pretimeout_stage;
	struct kempld_wdt_stage *timeout_stage;
	unsigned int pretimeout = 0;
	int ret;

	if (wdt_data->locked)
		return -EPERM;

	timeout_stage = &wdt_data->stage[STAGE_TIMEOUT];
	pretimeout_stage = &wdt_data->stage[STAGE_PRETIMEOUT];

	if (pretimeout_stage->mask && wdd->pretimeout > 0) {
		if (wdd->pretimeout > timeout)
			return -EINVAL;

		pretimeout = timeout - wdd->pretimeout;
		timeout = wdd->pretimeout;

		ret = kempld_wdt_set_stage_timeout(wdt_data, pretimeout_stage,
						   pretimeout);
		if (ret)
			return ret;
	}

	if (kempld_wdt_check_stage_action(wdt_data, timeout_action))
		dev_warn(wdd->parent,
			 "Requested timeout action (%d) not supported",
			 timeout_action);

	ret = kempld_wdt_set_stage_action(wdt_data, timeout_stage,
						timeout_action);
	if (ret)
		return ret;
	ret = kempld_wdt_set_stage_timeout(wdt_data, timeout_stage, timeout);
	if (ret)
		return ret;

	wdd->timeout = timeout + pretimeout;

	return 0;
}

static int kempld_wdt_set_pretimeout(struct watchdog_device *wdd,
					unsigned int pretimeout)
{
	struct kempld_wdt_data *wdt_data = watchdog_get_drvdata(wdd);
	struct kempld_wdt_stage *pretimeout_stage;
	struct kempld_wdt_stage *timeout_stage;
	u8 action = ACTION_NONE;
	int ret;

	if (wdt_data->locked)
		return -EPERM;

	timeout_stage = &wdt_data->stage[STAGE_TIMEOUT];
	pretimeout_stage = &wdt_data->stage[STAGE_PRETIMEOUT];

	if (!pretimeout_stage->mask)
		return -ENXIO;

	if (pretimeout > wdd->timeout)
		return -EINVAL;

	if (pretimeout > 0)
		action = pretimeout_action;

	if (kempld_wdt_check_stage_action(wdt_data, action))
		dev_warn(wdd->parent,
			 "Requested pretimeout action (%d) not supported",
			 action);
	/*
	 * If action is configured to delay, stage 0 disables itself after
	 * timeout occures - this is usually not what we want.
	 */
	if (((action == ACTION_DELAY) || (action == ACTION_DELAY_WDT))
	    && (pretimeout_stage->id == 0)) {
		dev_warn(wdd->parent,
			 "Pretimeout action delay might not work as expected!");
	}

	ret = kempld_wdt_set_stage_action(wdt_data, pretimeout_stage,
						action);
	if (ret)
		return ret;
	ret = kempld_wdt_set_stage_timeout(wdt_data, pretimeout_stage,
						wdd->timeout - pretimeout);
	if (ret)
		return ret;
	ret = kempld_wdt_set_stage_timeout(wdt_data, timeout_stage,
					   pretimeout);
	if (ret)
		return ret;

	wdd->pretimeout = pretimeout;
	return 0;
}

static void kempld_wdt_update_timeouts(struct kempld_wdt_data *wdt_data)
{
	struct watchdog_device *wdd = &wdt_data->wdd;
	struct kempld_device_data *pld = wdt_data->pld;
	struct kempld_wdt_stage *pretimeout_stage;
	struct kempld_wdt_stage *timeout_stage;
	unsigned int pretimeout, timeout, action;

	pretimeout_stage = &wdt_data->stage[STAGE_PRETIMEOUT];
	timeout_stage = &wdt_data->stage[STAGE_TIMEOUT];

	kempld_get_mutex(pld);
	pretimeout = kempld_wdt_get_timeout(wdt_data, pretimeout_stage);
	timeout = kempld_wdt_get_timeout(wdt_data, timeout_stage);
	kempld_release_mutex(pld);

	action = kempld_wdt_get_stage_action(wdt_data, pretimeout_stage);
	if ((action == ACTION_RESET) || (action == ACTION_RESET_WDT))
		timeout = 0;

	if (pretimeout)
		wdd->pretimeout = timeout;
	else
		wdd->pretimeout = 0;

	wdt_data->wdd.timeout = pretimeout + timeout;
}

static int kempld_wdt_start(struct watchdog_device *wdd)
{
	struct kempld_wdt_data *wdt_data = watchdog_get_drvdata(wdd);
	struct kempld_device_data *pld = wdt_data->pld;
	int pretimeout_stage_id, timeout_stage_id;
	u8 status;
	int ret;

	if (!wdt_data->locked) {
		if (updateonrestart) {
			wdd->pretimeout = pretimeout;
			wdd->timeout = timeout;
			updateonrestart = 0;
		}

		if (wdd->info->options & WDIOF_PRETIMEOUT) {
			ret = kempld_wdt_set_pretimeout(wdd, wdd->pretimeout);
			if (ret)
				return ret;
		}

		ret = kempld_wdt_set_timeout(wdd, wdd->timeout);
		if (ret)
			return ret;
	}

	pretimeout_stage_id = wdt_data->stage[STAGE_PRETIMEOUT].id;
	timeout_stage_id = wdt_data->stage[STAGE_TIMEOUT].id;

	kempld_get_mutex(pld);
	status = kempld_read8(pld, KEMPLD_WDT_CFG);
	status |= KEMPLD_WDT_CFG_ENABLE;
	status |= KEMPLD_WDT_CFG_STAGE_TOUT(pretimeout_stage_id);
	status |= KEMPLD_WDT_CFG_STAGE_TOUT(timeout_stage_id);
	kempld_write8(pld, KEMPLD_WDT_CFG, status);
	status = kempld_read8(pld, KEMPLD_WDT_CFG);
	kempld_release_mutex(pld);

	/* Check if the watchdog was enabled */
	if (!(status & KEMPLD_WDT_CFG_ENABLE))
		return -EACCES;

	return 0;
}

static int kempld_wdt_stop(struct watchdog_device *wdd)
{
	struct kempld_wdt_data *wdt_data = watchdog_get_drvdata(wdd);
	struct kempld_device_data *pld = wdt_data->pld;
	u8 status;

	kempld_get_mutex(pld);
	status = kempld_read8(pld, KEMPLD_WDT_CFG);
	status &= ~KEMPLD_WDT_CFG_ENABLE;
	kempld_write8(pld, KEMPLD_WDT_CFG, status);
	status = kempld_read8(pld, KEMPLD_WDT_CFG);
	kempld_release_mutex(pld);

	/* Check if the watchdog was disabled */
	if (status & KEMPLD_WDT_CFG_ENABLE)
		return -EACCES;

	return 0;
}

static int kempld_wdt_keepalive(struct watchdog_device *wdd)
{
	struct kempld_wdt_data *wdt_data = watchdog_get_drvdata(wdd);
	struct kempld_device_data *pld = wdt_data->pld;

	kempld_get_mutex(pld);
	kempld_write8(pld, KEMPLD_WDT_KICK, 'K');
	kempld_release_mutex(pld);

	return 0;
}

static long kempld_wdt_ioctl(struct watchdog_device *wdd, unsigned int cmd,
				unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	int ret = -ENOIOCTLCMD;
	int __user *p = argp;
	int new_value;

	switch (cmd) {
	case WDIOC_SETPRETIMEOUT:
		if (get_user(new_value, p))
			return -EFAULT;
		ret = kempld_wdt_set_pretimeout(wdd, new_value);
		if (ret)
			return ret;
		ret = kempld_wdt_keepalive(wdd);
		break;
	}

	return ret;
}

static int kempld_wdt_probe_stages(struct kempld_wdt_data *wdt_data)
{
	struct kempld_device_data *pld = wdt_data->pld;
	struct kempld_wdt_stage *pretimeout_stage;
	struct kempld_wdt_stage *timeout_stage;
	u8 index, data, data_orig;
	u32 mask;
	int i, j;

	pretimeout_stage = &wdt_data->stage[STAGE_PRETIMEOUT];
	timeout_stage = &wdt_data->stage[STAGE_TIMEOUT];

	pretimeout_stage->mask = 0;
	timeout_stage->mask = 0;

	for (i = 0; i < 3; i++) {
		index = KEMPLD_WDT_STAGE_TIMEOUT(i);
		mask = 0;

		kempld_get_mutex(pld);
		/* Probe each byte individually. */
		for (j = 0; j < 4; j++) {
			data_orig = kempld_read8(pld, index + j);
			if (!wdt_data->locked) {
				kempld_write8(pld, index + j, 0x00);
				data = kempld_read8(pld, index + j);
				/* A failed write means this byte is reserved */
				if (data != 0x00)
					break;
				kempld_write8(pld, index + j, data_orig);
			} else
				if (data_orig == 0xff)
					break;

			mask |= 0xff << (j * 8);
		}
		kempld_release_mutex(pld);

		/* Assign available stages to timeout and pretimeout */
		if (!timeout_stage->mask) {
			timeout_stage->mask = mask;
			timeout_stage->id = i;
		} else {
			pretimeout_stage->mask = timeout_stage->mask;
			timeout_stage->mask = mask;
			pretimeout_stage->id = timeout_stage->id;
			timeout_stage->id = i;
			break;
		}
	}

	if ((!timeout_stage->mask) && (!wdt_data->locked))
		return -ENODEV;

	return 0;
}

#ifdef CONFIG_WATCHDOG_PRETIMEOUT_GOV
static struct watchdog_device *nmi_watchdog_device;

static int kempld_wdt_pretimeout_nmi(unsigned int reason,
				     struct pt_regs *regs)
{
	watchdog_notify_pretimeout(nmi_watchdog_device);

	return NMI_HANDLED;
}

static void kempld_wdt_register_nmi(struct watchdog_device *wdd)
{
	int ret;

	/* Try to register an NMI handler */
	nmi_watchdog_device = wdd;
	ret = register_nmi_handler(NMI_UNKNOWN,
				   kempld_wdt_pretimeout_nmi, 0,
				   "kempld-wdt");
	if (ret) {
		nmi_watchdog_device = NULL;
		dev_warn(wdd->parent,
			 "Unable to register a pretimeout NMI handler.\n");
	}
}

static void kempld_wdt_unregister_nmi(void)
{
	if (nmi_watchdog_device)
		unregister_nmi_handler(NMI_UNKNOWN, "kempld-wdt");
}
#else
static void kempld_wdt_register_nmi(struct watchdog_device *wdd) { }
static void kempld_wdt_unregister_nmi(void) { }
#endif

static struct watchdog_info kempld_wdt_info = {
	.identity	= "KEMPLD Watchdog",
	.options	= WDIOF_KEEPALIVEPING
};

static const struct watchdog_ops kempld_wdt_ops = {
	.owner		= THIS_MODULE,
	.start		= kempld_wdt_start,
	.stop		= kempld_wdt_stop,
	.ping		= kempld_wdt_keepalive,
	.set_timeout	= kempld_wdt_set_timeout,
	.ioctl		= kempld_wdt_ioctl,
};

static int kempld_wdt_probe(struct platform_device *pdev)
{
	struct kempld_device_data *pld = dev_get_drvdata(pdev->dev.parent);
	struct kempld_wdt_data *wdt_data;
	struct device *dev = &pdev->dev;
	struct watchdog_device *wdd;
	u8 status;
	int ret = 0;

	wdt_data = devm_kzalloc(dev, sizeof(*wdt_data), GFP_KERNEL);
	if (!wdt_data)
		return -ENOMEM;

	wdt_data->pld = pld;
	wdd = &wdt_data->wdd;
	wdd->parent = dev;

	kempld_get_mutex(pld);
	status = kempld_read8(pld, KEMPLD_WDT_CFG);
	kempld_release_mutex(pld);

	/* Enable nowayout if watchdog is already locked */

	if (status & KEMPLD_WDT_CFG_GLOBAL_LOCK) {
		wdt_data->locked = true;
		dev_notice(dev, "Watchdog registers are globally locked - no access!\n");
		dev_notice(dev, "Probing stages and configured timeouts may be inaccurate!\n");
	} else
		kempld_wdt_info.options |= WDIOF_SETTIMEOUT;

	if (status & KEMPLD_WDT_CFG_ENABLE_LOCK) {
		if (!nowayout)
			dev_warn(dev,
				 "Forcing nowayout - watchdog lock enabled!\n");
		nowayout = true;
	} else
		kempld_wdt_info.options |= WDIOF_MAGICCLOSE;

	/* watchdog firmware version is identical to the CPLD version */
	kempld_wdt_info.firmware_version = (pld->info.major<<24)
		| (pld->info.minor<<16) | pld->info.buildnr;

	ret = kempld_wdt_probe_stages(wdt_data);
	if (ret)
		return ret;


	if (wdt_data->stage[STAGE_PRETIMEOUT].mask)
		kempld_wdt_info.options |= WDIOF_PRETIMEOUT;

	wdd->info = &kempld_wdt_info;
	wdd->ops = &kempld_wdt_ops;

	watchdog_set_drvdata(wdd, wdt_data);
	watchdog_set_nowayout(wdd, nowayout);

	if (timeout_action & ~STAGE_CFG_ACTION_MASK)
		return -EINVAL;

	if (pretimeout_action & ~STAGE_CFG_ACTION_MASK)
		return -EINVAL;

	/* Get current watchdog settings */
	kempld_wdt_update_timeouts(wdt_data);

	/* Check if watchdog is already enabled */
	if (status & KEMPLD_WDT_CFG_ENABLE) {
		set_bit(WDOG_HW_RUNNING, &wdd->status);
		dev_info(dev, "Watchdog was already enabled\n");
		if (updateonrestart && !wdt_data->locked)
			dev_info(dev,
				 "New watchdog timings will be set on (re)start\n");
	} else if (!wdt_data->locked) {
		kempld_wdt_set_pretimeout(wdd, pretimeout);
		kempld_wdt_set_timeout(wdd, timeout);
	}

	/* Get bootstatus, if the device supports it */
	if (kempld_wdt_get_bootstatus(wdd) == 0)
		kempld_wdt_info.options |= WDIOF_OVERHEAT | WDIOF_EXTERN1
			| WDIOF_POWERUNDER | WDIOF_CARDRESET;

	kempld_wdt_register_nmi(wdd);

	platform_set_drvdata(pdev, wdt_data);
	watchdog_stop_on_reboot(wdd);
	watchdog_stop_on_unregister(wdd);
	ret = devm_watchdog_register_device(dev, wdd);
	if (ret)
		return ret;

	dev_info(dev, "Watchdog registered with %ds timeout\n", wdd->timeout);

	return 0;
}

static int kempld_wdt_remove(struct platform_device *pdev)
{
	kempld_wdt_unregister_nmi();

	return 0;
}

#ifdef CONFIG_PM
/* Disable watchdog if it is active during suspend */
static int kempld_wdt_suspend(struct platform_device *pdev,
				pm_message_t message)
{
	struct kempld_wdt_data *wdt_data = platform_get_drvdata(pdev);
	struct kempld_device_data *pld = wdt_data->pld;
	struct watchdog_device *wdd = &wdt_data->wdd;

	kempld_get_mutex(pld);
	wdt_data->pm_status_store = kempld_read8(pld, KEMPLD_WDT_CFG);
	kempld_release_mutex(pld);

	kempld_wdt_update_timeouts(wdt_data);

	if (wdt_data->pm_status_store & KEMPLD_WDT_CFG_ENABLE)
		return kempld_wdt_stop(wdd);

	return 0;
}

/* Enable watchdog and configure it if necessary */
static int kempld_wdt_resume(struct platform_device *pdev)
{
	struct kempld_wdt_data *wdt_data = platform_get_drvdata(pdev);
	struct kempld_device_data *pld = wdt_data->pld;
	struct watchdog_device *wdd = &wdt_data->wdd;
	u8 status;

	if (wdt_data->locked) {
		/* Check again if the global lock is still active */
		kempld_get_mutex(pld);
		status = kempld_read8(pld, KEMPLD_WDT_CFG);
		kempld_release_mutex(pld);
		if (!(status & KEMPLD_WDT_CFG_GLOBAL_LOCK)) {
			wdt_data->locked = false;
			kempld_wdt_update_timeouts(wdt_data);
		}
	}

	/*
	 * If watchdog was stopped before suspend be sure it gets disabled
	 * again, for the case BIOS has enabled it during resume
	 */
	if (wdt_data->pm_status_store & KEMPLD_WDT_CFG_ENABLE)
		return kempld_wdt_start(wdd);
	else
		return kempld_wdt_stop(wdd);
}
#else
#define kempld_wdt_suspend	NULL
#define kempld_wdt_resume	NULL
#endif

static struct platform_driver kempld_wdt_driver = {
	.driver		= {
		.name	= "kempld-wdt",
	},
	.probe		= kempld_wdt_probe,
	.remove		= kempld_wdt_remove,
	.suspend	= kempld_wdt_suspend,
	.resume		= kempld_wdt_resume,
};

module_platform_driver(kempld_wdt_driver);

MODULE_DESCRIPTION("KEM PLD Watchdog Driver");
MODULE_AUTHOR("Michael Brunner <michael.brunner@kontron.com>");
MODULE_LICENSE("GPL");
