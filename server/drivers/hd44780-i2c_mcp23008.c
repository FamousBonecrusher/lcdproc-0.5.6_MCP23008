/** \file server/drivers/hd44780-i2c.c
 * \c i2c connection type of \c hd44780 driver for Hitachi HD44780 based LCD displays.
 *
 * The LCD is operated in its 4 bit-mode to be connected to the 8 bit-port
 * of a single PCF8574(A) or PCA9554(A) that is accessed by the server via the i2c bus.
 */

/* Copyright (c) 2005 Matthias Goebl <matthias.goebl@goebl.net>
 *		  2000, 1999, 1995 Benjamin Tse <blt@Comports.com>
 *		  2001 Joris Robijn <joris@robijn.net>
 *		  1999 Andrew McMeikan <andrewm@engineer.com>
 *		  1998 Richard Rognlie <rrognlie@gamerz.net>
 *		  1997 Matthias Prinke <m.prinke@trashcan.mcnet.de>
 *
 * The connections are:
 * PCF8574AP	  LCD
 * P0 (4)	  D4 (11)
 * P1 (5)	  D5 (12)
 * P2 (6)	  D6 (13)
 * P3 (7)	  D7 (14)
 * P4 (9)	  RS (4)
 * P5 (10)	  RW (5)
 * P6 (11)	  EN (6)
 *
 * Backlight
 * P7 (12)	  /backlight (optional, active-low)
 *
 * Configuration:
 * device=/dev/i2c-0   # the device file of the i2c bus
 * port=0x20   # the i2c address of the i2c port expander
 *
 *  Attention: Bit 8 of the address given in port is special:
 *  It tells the driver to treat the device as PCA9554 or similar,
 *  a device that needs a 2-byte command, and it will be stripped
 *  off the address.
 *  So we have:
 *  port=0x20..0x27   PCF8574  with A[012]=0..7
 *  port=0x38..0x3f   PCF8574A with A[012]=0..7
 *  port=0xa0..0xa7   PCA9554  with A[012]=0..7
 *  port=0xa0..0xa7   PCA9554A with A[012]=0..7
 *
 * When using this driver, DON'T load the i2c chip module (e.g. pcf8574),
 * you only need the i2c bus driver module!
 *
 *
 * Based mostly on the hd44780-4bit module, see there for a complete history.
 * Suggestions for PCA9554 support from Tonu Samuel <tonu@jes.ee>.
 *
 * This file is released under the GNU General Public License. Refer to the
 * COPYING file distributed with this package.
 */

#include "hd44780-i2c_mcp23008.h"
#include "hd44780-low.h"

#include "report.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
/* I2C_SLAVE is missing in linux/i2c-dev.h from kernel headers of 2.4.x kernels */
#ifndef I2C_SLAVE
#define I2C_SLAVE 0x0703  /* Change slave address */
#endif

// Generally, any function that accesses the LCD control lines needs to be
// implemented separately for each HW design. This is typically (but not
// restricted to):
// HD44780_senddata
// HD44780_readkeypad

void i2c_mcp23008_HD44780_senddata(PrivateData *p, unsigned char displayID, unsigned char flags, unsigned char ch);
void i2c_mcp23008_HD44780_backlight(PrivateData *p, unsigned char state);
void i2c_mcp23008_HD44780_close(PrivateData *p);

#define EN	0x10
#define RS	0x20
#define BL	0x80  //not connected
// note that the above bits are all meant for the data port of MCP23008

#define GPOUT 0x09 //output port register of MCP23008

#define I2C_ADDR_MASK 0x7f

static void
i2c_out(PrivateData *p, unsigned char val)
{
	__u8 data[2];
	int datalen;
	static int no_more_errormsgs=0;
	{ // we have a MCP23008 or similar, that needs a 2-byte command
		data[0]=GPOUT; // command: read/write output port register
		data[1]=val;
		datalen=2;
	}
	if (write(p->fd,data,datalen) != datalen) {
		p->hd44780_functions->drv_report(no_more_errormsgs?RPT_DEBUG:RPT_ERR, "HD44780: I2C: i2c write data %u to address %u failed: %s",
			val, p->port & I2C_ADDR_MASK, strerror(errno));
		no_more_errormsgs=1;
	}
}

#define DEFAULT_DEVICE		"/dev/i2c-1"


/**
 * Initialize the driver.
 * \param drvthis  Pointer to driver structure.
 * \retval 0       Success.
 * \retval -1      Error.
 */
int
hd_init_i2c_mcp23008(Driver *drvthis)
{
	PrivateData *p = (PrivateData*) drvthis->private_data;
	HD44780_functions *hd44780_functions = p->hd44780_functions;

	int enableLines = EN;
	char device[256] = DEFAULT_DEVICE;
	p->backlight_bit = BL;

	/* READ CONFIG FILE */

	/* Get serial device to use */
	strncpy(device, drvthis->config_get_string(drvthis->name, "Device", 0, DEFAULT_DEVICE), sizeof(device));
	device[sizeof(device)-1] = '\0';
	report(RPT_INFO,"HD44780: I2C_MCP23008: Using device '%s' and address %u for a MCP23008",
		device, p->port & I2C_ADDR_MASK);

	// Open the I2C device
	p->fd = open(device, O_RDWR);
	if (p->fd < 0) {
		report(RPT_ERR, "HD44780: I2C: open i2c device '%s' failed: %s", device, strerror(errno));
		return(-1);
	}

	// Set I2C address
	if (ioctl(p->fd,I2C_SLAVE, p->port & I2C_ADDR_MASK) < 0) {
		report(RPT_ERR, "HD44780: I2C: set address to '%i': %s", p->port & I2C_ADDR_MASK, strerror(errno));
		return(-1);
	}


	{ // we have a MCP23008 or similar, that needs special config
		report(RPT_INFO, "HD44780: I2C: initializing MCP23008...");

		__u8 data[2];
		data[0] = 0x09; // clear pins
		data[1] = 0x00; // => all pins are low
		if (write(p->fd,data,2) != 2) {
			report(RPT_ERR, "HD44780: I2C: i2c clear output pins failed: %s", strerror(errno));
		}
		data[0] = 0x00; // command: set output direction
		data[1] = 0x00; // -> all pins are outputs
		if (write(p->fd,data,2) != 2) {
			report(RPT_ERR, "HD44780: I2C: i2c set output direction failed: %s", strerror(errno));
		}
		data[0] = 0x06; // configure pull-ups
		data[1] = 0x00; // -> all pull-ups are disabled
		if (write(p->fd,data,2) != 2) {
			report(RPT_ERR, "HD44780: I2C: i2c configure pull-ups failed: %s", strerror(errno));
		}
		data[0] = 0x01; // set pin polarity
		data[1] = 0x00; // -> equal to logic
		if (write(p->fd,data,2) != 2) {
			report(RPT_ERR, "HD44780: I2C: i2c set pin polarity failed: %s", strerror(errno));
		}

	}

	hd44780_functions->senddata = i2c_mcp23008_HD44780_senddata;
	hd44780_functions->backlight = i2c_mcp23008_HD44780_backlight;
	hd44780_functions->close = i2c_mcp23008_HD44780_close;

	// powerup the lcd now
	/* We'll now send 0x03 a couple of times,
	 * which is in fact (FUNCSET | IF_8BIT) >> 4 */
	i2c_out(p, 0x03);
	if (p->delayBus)
		hd44780_functions->uPause(p, 1);

	i2c_out(p, enableLines | 0x03);
	if (p->delayBus)
		hd44780_functions->uPause(p, 1);
	i2c_out(p, 0x03);
	hd44780_functions->uPause(p, 15000);

	i2c_out(p, enableLines | 0x03);
	if (p->delayBus)
		hd44780_functions->uPause(p, 1);
	i2c_out(p, 0x03);
	hd44780_functions->uPause(p, 5000);

	i2c_out(p, enableLines | 0x03);
	if (p->delayBus)
		hd44780_functions->uPause(p, 1);
	i2c_out(p, 0x03);
	hd44780_functions->uPause(p, 100);

	i2c_out(p, enableLines | 0x03);
	if (p->delayBus)
		hd44780_functions->uPause(p, 1);
	i2c_out(p, 0x03);
	hd44780_functions->uPause(p, 100);

	// now in 8-bit mode...  set 4-bit mode
	i2c_out(p, 0x02);
	if (p->delayBus)
		hd44780_functions->uPause(p, 1);

	i2c_out(p, enableLines | 0x02);
	if (p->delayBus)
		hd44780_functions->uPause(p, 1);
	i2c_out(p, 0x02);
	hd44780_functions->uPause(p, 100);

	// Set up two-line, small character (5x8) mode
	hd44780_functions->senddata(p, 0, RS_INSTR, FUNCSET | IF_4BIT | TWOLINE | SMALLCHAR);
	hd44780_functions->uPause(p, 40);

	common_init(p, IF_4BIT);

	return 0;
}


/**
 * Close the device.
 * \param p          Pointer to driver's private data structure.
 */
void
i2c_mcp23008_HD44780_close(PrivateData *p) {
	if (p->fd >= 0) {
		close(p->fd);
	}
}


/**
 * Send data or commands to the display.
 * \param p          Pointer to driver's private data structure.
 * \param displayID  ID of the display (or 0 for all) to send data to.
 * \param flags      Defines whether to end a command or data.
 * \param ch         The value to send.
 */
void
i2c_mcp23008_HD44780_senddata(PrivateData *p, unsigned char displayID, unsigned char flags, unsigned char ch)
{
	unsigned char enableLines = 0, portControl = 0;
	unsigned char h = (ch >> 4) & 0x0f;     // high and low nibbles
	unsigned char l = ch & 0x0f;

	if (flags == RS_INSTR)
		portControl = 0;
	else //if (flags == RS_DATA)
		portControl = RS;

	portControl |= p->backlight_bit;

	enableLines = EN;

	i2c_out(p, portControl | h);
	if (p->delayBus)
		p->hd44780_functions->uPause(p, 1);
	i2c_out(p, enableLines | portControl | h);
	if (p->delayBus)
		p->hd44780_functions->uPause(p, 1);
	i2c_out(p, portControl | h);

	i2c_out(p, portControl | l);
	if (p->delayBus)
		p->hd44780_functions->uPause(p, 1);
	i2c_out(p, enableLines | portControl | l);
	if (p->delayBus)
		p->hd44780_functions->uPause(p, 1);
	i2c_out(p, portControl | l);
}


/**
 * Turn display backlight on or off.
 * \param p      Pointer to driver's private data structure.
 * \param state  New backlight status.
 */
void i2c_mcp23008_HD44780_backlight(PrivateData *p, unsigned char state)
{
	p->backlight_bit = ((!p->have_backlight||state) ? 0 : BL);

	i2c_out(p, p->backlight_bit);
}
