/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  Kontron PLD I2C driver definitions
 *
 *  Copyright (c) 2010-2014 Kontron Europe GmbH
 *  Author: Michael Brunner <michael.brunner@kontron.com>
 */

#ifndef _KEMPLD_I2C_H_
#define _KEMPLD_I2C_H_

#define KEMPLD_I2C_PRELOW	0x0b
#define KEMPLD_I2C_PREHIGH	0x0c
#define KEMPLD_I2C_DATA		0x0e

#define KEMPLD_I2C_CTRL		0x0d
#define I2C_CTRL_BCEN		0x20
#define I2C_CTRL_IEN		0x40
#define I2C_CTRL_EN		0x80

#define KEMPLD_I2C_STAT		0x0f
#define I2C_STAT_IF		0x01
#define I2C_STAT_TIP		0x02
#define I2C_STAT_ARBLOST	0x20
#define I2C_STAT_BUSY		0x40
#define I2C_STAT_NACK		0x80

#define KEMPLD_I2C_CMD		0x0f
#define I2C_CMD_START		0x91
#define I2C_CMD_STOP		0x41
#define I2C_CMD_READ		0x21
#define I2C_CMD_WRITE		0x11
#define I2C_CMD_READ_ACK	0x21
#define I2C_CMD_READ_NACK	0x29
#define I2C_CMD_IACK		0x01

#define KEMPLD_I2C_MX			0x15
#define	KEMPLD_I2C_MX_GET_MAX(x)	((x & 0xf0)>>4)
#define	KEMPLD_I2C_MX_MASK		0x0f

enum {
	STATE_DONE = 0,
	STATE_INIT,
	STATE_ADDR,
	STATE_ADDR_CONT,
	STATE_ADDR10,
	STATE_START,
	STATE_WRITE,
	STATE_READ,
	STATE_ERROR,
};

struct kempld_i2c_data {
	struct i2c_adapter		adap;
	struct platform_device          *mux_pdev;
	struct i2c_msg			*msg;
	int				base_nr; /* -1 for dynamic */
	int				pos;
	int				nmsgs;
	int				state;
	int				was_active;
	int				busclear;
	int				mx;
	int				mx_max;
	int				gpio_mux;
	wait_queue_head_t		wait;
	int				irq_wake;
	unsigned long			timeout;
	int				ret;
	int				irq;
	struct kempld_device_data	*pld;
};

#endif /* _KEMPLD_I2C_H_ */
