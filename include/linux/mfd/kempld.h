/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Kontron PLD driver definitions
 *
 * Copyright (c) 2010-2016 Kontron Europe GmbH
 * Author: Michael Brunner <michael.brunner@kontron.com>
 */

#ifndef _LINUX_MFD_KEMPLD_H_
#define _LINUX_MFD_KEMPLD_H_

/* kempld register definitions */
#define KEMPLD_IOINDEX			0xa80
#define KEMPLD_IODATA			0xa81
#define KEMPLD_REGISTER_SPACE_LEN	128
#define KEMPLD_MUTEX_KEY		0x80
#define KEMPLD_VERSION			0x00
#define KEMPLD_VERSION_LSB		0x00
#define KEMPLD_VERSION_MSB		0x01
#define KEMPLD_VERSION_GET_MINOR(x)	(x & 0x1f)
#define KEMPLD_VERSION_GET_MAJOR(x)	((x >> 5) & 0x1f)
#define KEMPLD_VERSION_GET_NUMBER(x)	((x >> 10) & 0xf)
#define KEMPLD_VERSION_GET_TYPE(x)	((x >> 14) & 0x3)
#define KEMPLD_BUILDNR			0x02
#define KEMPLD_BUILDNR_LSB		0x02
#define KEMPLD_BUILDNR_MSB		0x03
#define KEMPLD_FEATURE			0x04
#define KEMPLD_FEATURE_LSB		0x04
#define KEMPLD_FEATURE_MSB		0x05
#define KEMPLD_FEATURE_BIT_I2C		(1 << 0)
#define KEMPLD_FEATURE_BIT_WATCHDOG	(1 << 1)
#define KEMPLD_FEATURE_BIT_GPIO		(1 << 2)
#define KEMPLD_FEATURE_MASK_UART	(7 << 3)
#define KEMPLD_FEATURE_BIT_NMI		(1 << 8)
#define KEMPLD_FEATURE_BIT_SMI		(1 << 9)
#define KEMPLD_FEATURE_BIT_SCI		(1 << 10)
#define KEMPLD_SPEC			0x06
#define KEMPLD_SPEC_GET_MINOR(x)	(x & 0x0f)
#define KEMPLD_SPEC_GET_MAJOR(x)	((x >> 4) & 0x0f)
#define KEMPLD_IRQ_GPIO			0x35
#define KEMPLD_IRQ_GPIO_MASK		(0xf << 0)
#define KEMPLD_IRQ_GPIO_NMI_AVAILABLE	(1 << 5)
#define KEMPLD_IRQ_I2C			0x36
#define KEMPLD_IRQ_I2C_MASK		(0xf << 0)
#define KEMPLD_CFG			0x37
#define KEMPLD_CFG_GPIO_I2C_MUX		(1 << 0)
#define KEMPLD_CFG_BIOS_SET_PROTECT	(1 << 2)
#define KEMPLD_CFG_SAFE_BIOS_WD_EN	(1 << 3)
#define KEMPLD_CFG_ACTIVE_BIOS_CS	(1 << 4)
#define KEMPLD_CFG_SBSO			(1 << 5)
#define KEMPLD_CFG_FDSO			(1 << 6)
#define KEMPLD_CFG_EFSO			(1 << 7)
#define KEMPLD_LRC			0x38
#define KEMPLD_LRC_POR			(1 << 0)
#define KEMPLD_LRC_EXT			(1 << 1)
#define KEMPLD_LRC_WDT			(1 << 2)
#define KEMPLD_LRC_TEMP			(1 << 3)
#define KEMPLD_LRC_PWRGOOD		(1 << 4)
#define KEMPLD_LRC_SW			(1 << 5)
#define KEMPLD_LRC_OTHER		(1 << 6)
#define KEMPLD_LRC_SW_SCHEDULED		(1 << 7)
#define KEMPLD_LRC_MASK			(0x7f << 0)

#define	KEMPLD_CLK			33333333

#define	KEMPLD_TYPE_RELEASE		0x0
#define	KEMPLD_TYPE_DEBUG		0x1
#define	KEMPLD_TYPE_CUSTOM		0x2

#define KEMPLD_MUTEX_NOTIMEOUT		((unsigned int)~0)

#define KEMPLD_VERSION_LEN		10

enum kempld_eeep_type {
	KEMPLD_EEEP_NONE	= 0,
	KEMPLD_EEEP_COME	= 1, /* Board has COMe EEEP */
	KEMPLD_EEEP_COME_BKPL	= 2, /* Board has COMe EEEP & baseboard EEP */
	KEMPLD_EEEP_BKPL	= 3, /* Board has baseboard EEEP */
};

enum kempld_ddc_type {
	KEMPLD_DDC_NONE		= 0,
	KEMPLD_DDC_COME		= 1, /* Board has COMe DDC */
};

/**
 * struct kempld_info - PLD device information structure
 * @major:	PLD major revision
 * @minor:	PLD minor revision
 * @buildnr:	PLD build number
 * @number:	PLD board specific index
 * @type:	PLD type
 * @spec_major:	PLD FW specification major revision
 * @spec_minor:	PLD FW specification minor revision
 * @version:	PLD version string
 */
struct kempld_info {
	unsigned int major;
	unsigned int minor;
	unsigned int buildnr;
	unsigned int number;
	unsigned int type;
	unsigned int spec_major;
	unsigned int spec_minor;
	char version[KEMPLD_VERSION_LEN];
};

/**
 * struct kempld_device_data - Internal representation of the PLD device
 * @io_base:		Pointer to the IO memory
 * @io_index:		Pointer to the IO index register
 * @io_data:		Pointer to the IO data register
 * @pld_clock:		PLD clock frequency
 * @feature_mask:	PLD feature mask
 * @have_mutex:		Bool value that indicates if hw mutex is acquired
 * @last_index:		Last written index value
 * @dev:		Pointer to kernel device structure
 * @info:		KEMPLD info structure
 * @lock:		PLD mutex
 * @shadow:		PLD shadow registers
 * @last_reset_cause	Last reset cause
 * @eeep:		EEEP device on I2C bus
 * @ddc:		DDC device on I2C bus
 */
struct kempld_device_data {
	void __iomem		*io_base;
	void __iomem		*io_index;
	void __iomem		*io_data;
	u32			pld_clock;
	u32			feature_mask;
	int			have_mutex;
	u8			last_index;
	struct device		*dev;
	struct kempld_info	info;
	struct mutex		lock;
	u8			shadow[KEMPLD_REGISTER_SPACE_LEN];
	u8			last_reset_cause;
	enum kempld_eeep_type	eeep;
	enum kempld_ddc_type	ddc;
};

/**
 * struct kempld_platform_data - PLD hardware configuration structure
 * @pld_clock:			PLD clock frequency
 * @ioresource:			IO addresses of the PLD
 * @force_index_write:		Force writing the index register
 * @eeep:			EEEP device on I2C bus
 * @ddc:			DDC device on I2C bus
 * @gpio_names:			Platform specific GPIO naming
 * @get_mutex:			PLD specific get_mutex callback
 * @release_mutex:		PLD specific release_mutex callback
 * @get_info:			PLD specific get_info callback
 * @register_cells:		PLD specific register_cells callback
 */
struct kempld_platform_data {
	u32				pld_clock;
	struct resource			*ioresource;
	int				force_index_write;
	enum kempld_eeep_type		eeep;
	enum kempld_ddc_type		ddc;
	const char			**gpio_names;
	void (*get_hardware_mutex)	(struct kempld_device_data *);
	void (*release_hardware_mutex)	(struct kempld_device_data *);
	int (*get_info)			(struct kempld_device_data *);
	int (*register_cells)		(struct kempld_device_data *);
};

extern void kempld_get_mutex(struct kempld_device_data *pld);
extern void kempld_release_mutex(struct kempld_device_data *pld);

extern u8 kempld_read8(struct kempld_device_data *pld, u8 index);
extern u8 kempld_read8_shadow(struct kempld_device_data *pld, u8 index);
extern void kempld_write8(struct kempld_device_data *pld, u8 index, u8 data);
extern u16 kempld_read16(struct kempld_device_data *pld, u8 index);
extern u16 kempld_read16_shadow(struct kempld_device_data *pld, u8 index);
extern void kempld_write16(struct kempld_device_data *pld, u8 index, u16 data);
extern u32 kempld_read32(struct kempld_device_data *pld, u8 index);
extern u32 kempld_read32_shadow(struct kempld_device_data *pld, u8 index);
extern void kempld_write32(struct kempld_device_data *pld, u8 index, u32 data);
extern int kempld_request_irq_num(struct kempld_device_data *pld, int irq);
extern void kempld_free_irq_num(struct kempld_device_data *pld, int irq);

#endif /* _LINUX_MFD_KEMPLD_H_ */
