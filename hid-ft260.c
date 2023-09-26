// SPDX-License-Identifier: GPL-2.0-only
/*
 * hid-ft260.c - FTDI FT260 USB HID to I2C host bridge
 *
 * Copyright (c) 2021, Michael Zaidman <michaelz@xsightlabs.com>
 *
 * Data Sheet:
 *   https://www.ftdichip.com/Support/Documents/DataSheets/ICs/DS_FT260.pdf
 */

#include "hid-ids.h"
#include <linux/hidraw.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/serial.h>
#include <linux/serial_core.h>
#include <linux/kfifo.h>
#include <linux/tty_flip.h>
#include <linux/minmax.h>
#include <asm-generic/unaligned.h>

#define UART_COUNT_MAX		4	/* Number of UARTs this driver can handle */
#define FIFO_SIZE	256
#define TTY_WAKEUP_WATERMARK	(FIFO_SIZE / 2)

#ifdef DEBUG
static int ft260_debug = 1;
#else
static int ft260_debug;
#endif
module_param_named(debug, ft260_debug, int, 0600);
MODULE_PARM_DESC(debug, "Toggle FT260 debugging messages");

#define ft260_dbg(format, arg...)					  \
	do {								  \
		if (ft260_debug)					  \
			pr_info("%s: " format, __func__, ##arg);	  \
	} while (0)

#define FT260_REPORT_MAX_LENGTH (64)
#define FT260_I2C_DATA_REPORT_ID(len) (FT260_I2C_REPORT_MIN + ((len) - 1) / 4)
#define FT260_UART_DATA_REPORT_ID(len) (FT260_UART_REPORT_MIN + ((len) - 1) / 4)

/*
 * The input report format assigns 62 bytes for the data payload, but ft260
 * returns 60 and 2 in two separate transactions. To minimize transfer time
 * in reading chunks mode, set the maximum read payload length to 60 bytes.
 */
#define FT260_RD_DATA_MAX (60)
#define FT260_WR_DATA_MAX (60)

/*
 * Device interface configuration.
 * The FT260 has 2 interfaces that are controlled by DCNF0 and DCNF1 pins.
 * First implementes USB HID to I2C bridge function and
 * second - USB HID to UART bridge function.
 */
enum {
	FT260_MODE_ALL			= 0x00,
	FT260_MODE_I2C			= 0x01,
	FT260_MODE_UART			= 0x02,
	FT260_MODE_BOTH			= 0x03,
};

/* Control pipe */
enum {
	FT260_GET_RQST_TYPE		= 0xA1,
	FT260_GET_REPORT		= 0x01,
	FT260_SET_RQST_TYPE		= 0x21,
	FT260_SET_REPORT		= 0x09,
	FT260_FEATURE			= 0x03,
};

/* Report IDs / Feature In */
enum {
	FT260_CHIP_VERSION		= 0xA0,
	FT260_SYSTEM_SETTINGS		= 0xA1,
	FT260_I2C_STATUS		= 0xC0,
	FT260_I2C_READ_REQ		= 0xC2,
	FT260_I2C_REPORT_MIN		= 0xD0,
	FT260_I2C_REPORT_MAX		= 0xDE,
	FT260_GPIO			= 0xB0,
	FT260_UART_INTERRUPT_STATUS	= 0xB1,
	FT260_UART_STATUS		= 0xE0,
	FT260_UART_RI_DCD_STATUS	= 0xE1,
	FT260_UART_REPORT_MIN		= 0xF0,
	FT260_UART_REPORT_MAX		= 0xFE,
};

/* Feature Out */
enum {
	FT260_SET_CLOCK			= 0x01,
	FT260_SET_I2C_MODE		= 0x02,
	FT260_SET_UART_MODE		= 0x03,
	FT260_ENABLE_INTERRUPT		= 0x05,
	FT260_SELECT_GPIO2_FUNC		= 0x06,
	FT260_ENABLE_UART_DCD_RI	= 0x07,
	FT260_SELECT_GPIOA_FUNC		= 0x08,
	FT260_SELECT_GPIOG_FUNC		= 0x09,
	FT260_SET_INTERRUPT_TRIGGER	= 0x0A,
	FT260_SET_SUSPEND_OUT_POLAR	= 0x0B,
	FT260_ENABLE_UART_RI_WAKEUP	= 0x0C,
	FT260_SET_UART_RI_WAKEUP_CFG	= 0x0D,
	FT260_SET_I2C_RESET		= 0x20,
	FT260_SET_I2C_CLOCK_SPEED	= 0x22,
	FT260_SET_UART_RESET		= 0x40,
	FT260_SET_UART_CONFIG		= 0x41,
	FT260_SET_UART_BAUD_RATE	= 0x42,
	FT260_SET_UART_DATA_BIT		= 0x43,
	FT260_SET_UART_PARITY		= 0x44,
	FT260_SET_UART_STOP_BIT		= 0x45,
	FT260_SET_UART_BREAKING		= 0x46,
	FT260_SET_UART_XON_XOFF		= 0x49,
};

/* Response codes in I2C status report */
enum {
	FT260_I2C_STATUS_SUCCESS	= 0x00,
	FT260_I2C_STATUS_CTRL_BUSY	= 0x01,
	FT260_I2C_STATUS_ERROR		= 0x02,
	FT260_I2C_STATUS_ADDR_NO_ACK	= 0x04,
	FT260_I2C_STATUS_DATA_NO_ACK	= 0x08,
	FT260_I2C_STATUS_ARBITR_LOST	= 0x10,
	FT260_I2C_STATUS_CTRL_IDLE	= 0x20,
	FT260_I2C_STATUS_BUS_BUSY	= 0x40,
};

/* I2C Conditions flags */
enum {
	FT260_FLAG_NONE			= 0x00,
	FT260_FLAG_START		= 0x02,
	FT260_FLAG_START_REPEATED	= 0x03,
	FT260_FLAG_STOP			= 0x04,
	FT260_FLAG_START_STOP		= 0x06,
	FT260_FLAG_START_STOP_REPEATED	= 0x07,
};

/* Return values for ft260_get_interface_type func */
enum {
	FT260_IFACE_NONE,
	FT260_IFACE_I2C,
	FT260_IFACE_UART
};

#define FT260_SET_REQUEST_VALUE(report_id) ((FT260_FEATURE << 8) | (report_id))

/* Feature In reports */

struct ft260_get_chip_version_report {
	__u8 report;		/* FT260_CHIP_VERSION */
	__u8 chip_code[4];	/* FTDI chip identification code */
	__u8 reserved[8];
} __packed;

struct ft260_get_system_status_report {
	__u8 report;		/* FT260_SYSTEM_SETTINGS */
	__u8 chip_mode;		/* DCNF0 and DCNF1 status, bits 0-1 */
	__u8 clock_ctl;		/* 0 - 12MHz, 1 - 24MHz, 2 - 48MHz */
	__u8 suspend_status;	/* 0 - not suspended, 1 - suspended */
	__u8 pwren_status;	/* 0 - FT260 is not ready, 1 - ready */
	__u8 i2c_enable;		/* 0 - disabled, 1 - enabled */
	__u8 uart_mode;		/* 0 - OFF; 1 - RTS_CTS, 2 - DTR_DSR, */
				/* 3 - XON_XOFF, 4 - No flow control */
	__u8 hid_over_i2c_en;	/* 0 - disabled, 1 - enabled */
	__u8 gpio2_function;	/* 0 - GPIO,  1 - SUSPOUT, */
				/* 2 - PWREN, 4 - TX_LED */
	__u8 gpioA_function;	/* 0 - GPIO, 3 - TX_ACTIVE, 4 - TX_LED */
	__u8 gpioG_function;	/* 0 - GPIO, 2 - PWREN, */
				/* 5 - RX_LED, 6 - BCD_DET */
	__u8 suspend_out_pol;	/* 0 - active-high, 1 - active-low */
	__u8 enable_wakeup_int;	/* 0 - disabled, 1 - enabled */
	__u8 intr_cond;		/* Interrupt trigger conditions */
	__u8 power_saving_en;	/* 0 - disabled, 1 - enabled */
	__u8 reserved[10];
} __packed;

struct ft260_get_i2c_status_report {
	__u8 report;		/* FT260_I2C_STATUS */
	__u8 bus_status;		/* I2C bus status */
	__le16 clock;		/* I2C bus clock in range 60-3400 KHz */
	__u8 reserved;
} __packed;

/* Feature Out reports */

struct ft260_set_system_clock_report {
	__u8 report;		/* FT260_SYSTEM_SETTINGS */
	__u8 request;		/* FT260_SET_CLOCK */
	__u8 clock_ctl;		/* 0 - 12MHz, 1 - 24MHz, 2 - 48MHz */
} __packed;

struct ft260_set_i2c_mode_report {
	__u8 report;		/* FT260_SYSTEM_SETTINGS */
	__u8 request;		/* FT260_SET_I2C_MODE */
	__u8 i2c_enable;		/* 0 - disabled, 1 - enabled */
} __packed;

struct ft260_set_uart_mode_report {
	__u8 report;		/* FT260_SYSTEM_SETTINGS */
	__u8 request;		/* FT260_SET_UART_MODE */
	__u8 uart_mode;		/* 0 - OFF; 1 - RTS_CTS, 2 - DTR_DSR, */
				/* 3 - XON_XOFF, 4 - No flow control */
} __packed;

struct ft260_set_i2c_reset_report {
	__u8 report;		/* FT260_SYSTEM_SETTINGS */
	__u8 request;		/* FT260_SET_I2C_RESET */
} __packed;

struct ft260_set_i2c_speed_report {
	__u8 report;		/* FT260_SYSTEM_SETTINGS */
	__u8 request;		/* FT260_SET_I2C_CLOCK_SPEED */
	__le16 clock;		/* I2C bus clock in range 60-3400 KHz */
} __packed;

/* Data transfer reports */

struct ft260_i2c_write_request_report {
	__u8 report;		/* FT260_I2C_REPORT */
	__u8 address;		/* 7-bit I2C address */
	__u8 flag;		/* I2C transaction condition */
	__u8 length;		/* data payload length */
	__u8 data[FT260_WR_DATA_MAX]; /* data payload */
} __packed;

struct ft260_i2c_read_request_report {
	__u8 report;		/* FT260_I2C_READ_REQ */
	__u8 address;		/* 7-bit I2C address */
	__u8 flag;		/* I2C transaction condition */
	__le16 length;		/* data payload length */
} __packed;

struct ft260_input_report {
	__u8 report;		/* FT260_I2C_REPORT or FT260_UART_REPORT */
	__u8 length;		/* data payload length */
	__u8 data[2];		/* data payload */
} __packed;

/* UART reports */
struct ft260_uart_write_request_report {
	__u8 report;		/* FT260_UART_REPORT */
	__u8 length;		/* data payload length */
	__u8 data[];		/* data payload */
} __packed;

struct ft260_configure_uart_request {
	__u8 report;		/* FT260_SYSTEM_SETTINGS */
	__u8 request;		/* FT260_SET_UART_CONFIG */
	__u8 flow_ctrl;		/* 0: OFF, 1: RTS_CTS, 2: DTR_DSR */
				/* 3: XON_XOFF, 4: No flow ctrl */
	/* The baudrate field is unaligned: */
	__le32 baudrate;	/* little endian, 9600 = 0x2580, 19200 = 0x4B00 */
	__u8 data_bit;		/* 7 or 8 */
	__u8 parity;		/* 0: no parity, 1: odd, 2: even, 3: high, 4: low */
	__u8 stop_bit;		/* 0: one stop bit, 2: 2 stop bits */
	__u8 breaking;		/* 0: no break */
} __packed;

/* UART interface configuration */
enum {
	FT260_CFG_FLOW_CTRL_OFF		= 0x00,
	FT260_CFG_FLOW_CTRL_RTS_CTS	= 0x01,
	FT260_CFG_FLOW_CTRL_DTR_DSR	= 0x02,
	FT260_CFG_FLOW_CTRL_XON_XOFF	= 0x03,
	FT260_CFG_FLOW_CTRL_NONE	= 0x04,

	FT260_CFG_DATA_BITS_7		= 0x07,
	FT260_CFG_DATA_BITS_8		= 0x08,

	FT260_CFG_PAR_NO		= 0x00,
	FT260_CFG_PAR_ODD		= 0x01,
	FT260_CFG_PAR_EVEN		= 0x02,
	FT260_CFG_PAR_HIGH		= 0x03,
	FT260_CFG_PAR_LOW		= 0x04,

	FT260_CFG_STOP_ONE_BIT		= 0x00,
	FT260_CFG_STOP_TWO_BIT		= 0x02,

	FT260_CFG_BREAKING_NO		= 0x00,
	FT260_CFG_BEAKING_YES		= 0x01,

	FT260_CFG_BAUD_MIN		= 1200,
	FT260_CFG_BAUD_MAX		= 12000000,
};

static const struct hid_device_id ft260_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_FUTURE_TECHNOLOGY,
			 USB_DEVICE_ID_FT260) },
	{ /* END OF LIST */ }
};
MODULE_DEVICE_TABLE(hid, ft260_devices);

struct ft260_device {
	struct i2c_adapter adap;
	struct hid_device *hdev;

	int ft260_is_serial;
	struct tty_port port;
	unsigned int index;
	struct kfifo xmit_fifo;
	spinlock_t write_lock;
	struct uart_icount icount;

	struct completion wait;
	struct mutex lock;
	u8 write_buf[FT260_REPORT_MAX_LENGTH];
	u8 *read_buf;
	u16 read_idx;
	u16 read_len;
	u16 clock;
};

static int ft260_hid_feature_report_get(struct hid_device *hdev,
					unsigned char report_id, u8 *data,
					size_t len)
{
	u8 *buf;
	int ret;

	buf = kmalloc(len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = hid_hw_raw_request(hdev, report_id, buf, len, HID_FEATURE_REPORT,
				 HID_REQ_GET_REPORT);
	if (likely(ret == len))
		memcpy(data, buf, len);
	else if (ret >= 0)
		ret = -EIO;
	kfree(buf);
	return ret;
}

static int ft260_hid_feature_report_set(struct hid_device *hdev, u8 *data,
					size_t len)
{
	u8 *buf;
	int ret;

	buf = kmemdup(data, len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf[0] = FT260_SYSTEM_SETTINGS;

	ret = hid_hw_raw_request(hdev, buf[0], buf, len, HID_FEATURE_REPORT,
				 HID_REQ_SET_REPORT);

	kfree(buf);
	return ret;
}

static int ft260_i2c_reset(struct hid_device *hdev)
{
	struct ft260_set_i2c_reset_report report;
	int ret;

	report.request = FT260_SET_I2C_RESET;

	ret = ft260_hid_feature_report_set(hdev, (u8 *)&report, sizeof(report));
	if (ret < 0) {
		hid_err(hdev, "failed to reset I2C controller: %d\n", ret);
		return ret;
	}

	ft260_dbg("done\n");
	return ret;
}

static int ft260_xfer_status(struct ft260_device *dev)
{
	struct hid_device *hdev = dev->hdev;
	struct ft260_get_i2c_status_report report;
	int ret;

	ret = ft260_hid_feature_report_get(hdev, FT260_I2C_STATUS,
					   (u8 *)&report, sizeof(report));
	if (unlikely(ret < 0)) {
		hid_err(hdev, "failed to retrieve status: %d\n", ret);
		return ret;
	}

	dev->clock = le16_to_cpu(report.clock);
	ft260_dbg("bus_status %#02x, clock %u\n", report.bus_status,
		  dev->clock);

	if (report.bus_status & FT260_I2C_STATUS_CTRL_BUSY)
		return -EAGAIN;

	if (report.bus_status & FT260_I2C_STATUS_BUS_BUSY)
		return -EBUSY;

	if (report.bus_status & FT260_I2C_STATUS_ERROR)
		return -EIO;

	ret = -EIO;

	if (report.bus_status & FT260_I2C_STATUS_ADDR_NO_ACK)
		ft260_dbg("unacknowledged address\n");

	if (report.bus_status & FT260_I2C_STATUS_DATA_NO_ACK)
		ft260_dbg("unacknowledged data\n");

	if (report.bus_status & FT260_I2C_STATUS_ARBITR_LOST)
		ft260_dbg("arbitration loss\n");

	if (report.bus_status & FT260_I2C_STATUS_CTRL_IDLE)
		ret = 0;

	return ret;
}

static int ft260_hid_output_report(struct hid_device *hdev, u8 *data,
				   size_t len)
{
	u8 *buf;
	int ret;

	buf = kmemdup(data, len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = hid_hw_output_report(hdev, buf, len);

	kfree(buf);
	return ret;
}

static int ft260_hid_output_report_check_status(struct ft260_device *dev,
						u8 *data, int len)
{
	int ret, usec, try = 3;
	struct hid_device *hdev = dev->hdev;

	ret = ft260_hid_output_report(hdev, data, len);
	if (ret < 0) {
		hid_err(hdev, "%s: failed to start transfer, ret %d\n",
			__func__, ret);
		ft260_i2c_reset(hdev);
		return ret;
	}

	/* transfer time = 1 / clock(KHz) * 10 bits * bytes */
	usec = 10000 / dev->clock * len;
	usleep_range(usec, usec + 100);
	ft260_dbg("wait %d usec, len %d\n", usec, len);
	do {
		ret = ft260_xfer_status(dev);
		if (ret != -EAGAIN)
			break;
	} while (--try);

	if (ret == 0 || ret == -EBUSY)
		return 0;

	ft260_i2c_reset(hdev);
	return -EIO;
}

static int ft260_i2c_write(struct ft260_device *dev, u8 addr, u8 *data,
			   int data_len, u8 flag)
{
	int len, ret, idx = 0;
	struct hid_device *hdev = dev->hdev;
	struct ft260_i2c_write_request_report *rep =
		(struct ft260_i2c_write_request_report *)dev->write_buf;

	do {
		if (data_len <= FT260_WR_DATA_MAX)
			len = data_len;
		else
			len = FT260_WR_DATA_MAX;

		rep->report = FT260_I2C_DATA_REPORT_ID(len);
		rep->address = addr;
		rep->length = len;
		rep->flag = flag;

		memcpy(rep->data, &data[idx], len);

		ft260_dbg("rep %#02x addr %#02x off %d len %d d[0] %#02x\n",
			  rep->report, addr, idx, len, data[0]);

		ret = ft260_hid_output_report_check_status(dev, (u8 *)rep,
							   len + 4);
		if (ret < 0) {
			hid_err(hdev, "%s: failed to start transfer, ret %d\n",
				__func__, ret);
			return ret;
		}

		data_len -= len;
		idx += len;

	} while (data_len > 0);

	return 0;
}

static int ft260_smbus_write(struct ft260_device *dev, u8 addr, u8 cmd,
			     u8 *data, u8 data_len, u8 flag)
{
	int ret = 0;
	int len = 4;

	struct ft260_i2c_write_request_report *rep =
		(struct ft260_i2c_write_request_report *)dev->write_buf;

	if (data_len >= sizeof(rep->data))
		return -EINVAL;

	rep->address = addr;
	rep->data[0] = cmd;
	rep->length = data_len + 1;
	rep->flag = flag;
	len += rep->length;

	rep->report = FT260_I2C_DATA_REPORT_ID(len);

	if (data_len > 0)
		memcpy(&rep->data[1], data, data_len);

	ft260_dbg("rep %#02x addr %#02x cmd %#02x datlen %d replen %d\n",
		  rep->report, addr, cmd, rep->length, len);

	ret = ft260_hid_output_report_check_status(dev, (u8 *)rep, len);

	return ret;
}

static int ft260_i2c_read(struct ft260_device *dev, u8 addr, u8 *data,
			  u16 len, u8 flag)
{
	struct ft260_i2c_read_request_report rep;
	struct hid_device *hdev = dev->hdev;
	int timeout;
	int ret;

	if (len > FT260_RD_DATA_MAX) {
		hid_err(hdev, "%s: unsupported rd len: %d\n", __func__, len);
		return -EINVAL;
	}

	dev->read_idx = 0;
	dev->read_buf = data;
	dev->read_len = len;

	rep.report = FT260_I2C_READ_REQ;
	rep.length = cpu_to_le16(len);
	rep.address = addr;
	rep.flag = flag;

	ft260_dbg("rep %#02x addr %#02x len %d\n", rep.report, rep.address,
		  rep.length);

	reinit_completion(&dev->wait);

	ret = ft260_hid_output_report(hdev, (u8 *)&rep, sizeof(rep));
	if (ret < 0) {
		hid_err(hdev, "%s: failed to start transaction, ret %d\n",
			__func__, ret);
		return ret;
	}

	timeout = msecs_to_jiffies(5000);
	if (!wait_for_completion_timeout(&dev->wait, timeout)) {
		ft260_i2c_reset(hdev);
		return -ETIMEDOUT;
	}

	ret = ft260_xfer_status(dev);
	if (ret == 0)
		return 0;

	ft260_i2c_reset(hdev);
	return -EIO;
}

/*
 * A random read operation is implemented as a dummy write operation, followed
 * by a current address read operation. The dummy write operation is used to
 * load the target byte address into the current byte address counter, from
 * which the subsequent current address read operation then reads.
 */
static int ft260_i2c_write_read(struct ft260_device *dev, struct i2c_msg *msgs)
{
	int len, ret;
	u16 left_len = msgs[1].len;
	u8 *read_buf = msgs[1].buf;
	u8 addr = msgs[0].addr;
	u16 read_off = 0;
	struct hid_device *hdev = dev->hdev;

	if (msgs[0].len > 2) {
		hid_err(hdev, "%s: unsupported wr len: %d\n", __func__,
			msgs[0].len);
		return -EOPNOTSUPP;
	}

	memcpy(&read_off, msgs[0].buf, msgs[0].len);

	do {
		if (left_len <= FT260_RD_DATA_MAX)
			len = left_len;
		else
			len = FT260_RD_DATA_MAX;

		ft260_dbg("read_off %#x left_len %d len %d\n", read_off,
			  left_len, len);

		ret = ft260_i2c_write(dev, addr, (u8 *)&read_off, msgs[0].len,
				      FT260_FLAG_START);
		if (ret < 0)
			return ret;

		ret = ft260_i2c_read(dev, addr, read_buf, len,
				     FT260_FLAG_START_STOP);
		if (ret < 0)
			return ret;

		left_len -= len;
		read_buf += len;
		read_off += len;

	} while (left_len > 0);

	return 0;
}

static int ft260_i2c_xfer(struct i2c_adapter *adapter, struct i2c_msg *msgs,
			  int num)
{
	int ret;
	struct ft260_device *dev = i2c_get_adapdata(adapter);
	struct hid_device *hdev = dev->hdev;

	mutex_lock(&dev->lock);

	ret = hid_hw_power(hdev, PM_HINT_FULLON);
	if (ret < 0) {
		hid_err(hdev, "failed to enter FULLON power mode: %d\n", ret);
		mutex_unlock(&dev->lock);
		return ret;
	}

	if (num == 1) {
		if (msgs->flags & I2C_M_RD)
			ret = ft260_i2c_read(dev, msgs->addr, msgs->buf,
					     msgs->len, FT260_FLAG_START_STOP);
		else
			ret = ft260_i2c_write(dev, msgs->addr, msgs->buf,
					      msgs->len, FT260_FLAG_START_STOP);
		if (ret < 0)
			goto i2c_exit;

	} else {
		/* Combined write then read message */
		ret = ft260_i2c_write_read(dev, msgs);
		if (ret < 0)
			goto i2c_exit;
	}

	ret = num;
i2c_exit:
	hid_hw_power(hdev, PM_HINT_NORMAL);
	mutex_unlock(&dev->lock);
	return ret;
}

static int ft260_smbus_xfer(struct i2c_adapter *adapter, u16 addr, u16 flags,
			    char read_write, u8 cmd, int size,
			    union i2c_smbus_data *data)
{
	int ret;
	struct ft260_device *dev = i2c_get_adapdata(adapter);
	struct hid_device *hdev = dev->hdev;

	ft260_dbg("smbus size %d\n", size);

	mutex_lock(&dev->lock);

	ret = hid_hw_power(hdev, PM_HINT_FULLON);
	if (ret < 0) {
		hid_err(hdev, "power management error: %d\n", ret);
		mutex_unlock(&dev->lock);
		return ret;
	}

	switch (size) {
	case I2C_SMBUS_QUICK:
		if (read_write == I2C_SMBUS_READ)
			ret = ft260_i2c_read(dev, addr, &data->byte, 0,
					     FT260_FLAG_START_STOP);
		else
			ret = ft260_smbus_write(dev, addr, cmd, NULL, 0,
						FT260_FLAG_START_STOP);
		break;
	case I2C_SMBUS_BYTE:
		if (read_write == I2C_SMBUS_READ)
			ret = ft260_i2c_read(dev, addr, &data->byte, 1,
					     FT260_FLAG_START_STOP);
		else
			ret = ft260_smbus_write(dev, addr, cmd, NULL, 0,
						FT260_FLAG_START_STOP);
		break;
	case I2C_SMBUS_BYTE_DATA:
		if (read_write == I2C_SMBUS_READ) {
			ret = ft260_smbus_write(dev, addr, cmd, NULL, 0,
						FT260_FLAG_START);
			if (ret)
				goto smbus_exit;

			ret = ft260_i2c_read(dev, addr, &data->byte, 1,
					     FT260_FLAG_START_STOP_REPEATED);
		} else {
			ret = ft260_smbus_write(dev, addr, cmd, &data->byte, 1,
						FT260_FLAG_START_STOP);
		}
		break;
	case I2C_SMBUS_WORD_DATA:
		if (read_write == I2C_SMBUS_READ) {
			ret = ft260_smbus_write(dev, addr, cmd, NULL, 0,
						FT260_FLAG_START);
			if (ret)
				goto smbus_exit;

			ret = ft260_i2c_read(dev, addr, (u8 *)&data->word, 2,
					     FT260_FLAG_START_STOP_REPEATED);
		} else {
			ret = ft260_smbus_write(dev, addr, cmd,
						(u8 *)&data->word, 2,
						FT260_FLAG_START_STOP);
		}
		break;
	case I2C_SMBUS_BLOCK_DATA:
		if (read_write == I2C_SMBUS_READ) {
			ret = ft260_smbus_write(dev, addr, cmd, NULL, 0,
						FT260_FLAG_START);
			if (ret)
				goto smbus_exit;

			ret = ft260_i2c_read(dev, addr, data->block,
					     data->block[0] + 1,
					     FT260_FLAG_START_STOP_REPEATED);
		} else {
			ret = ft260_smbus_write(dev, addr, cmd, data->block,
						data->block[0] + 1,
						FT260_FLAG_START_STOP);
		}
		break;
	case I2C_SMBUS_I2C_BLOCK_DATA:
		if (read_write == I2C_SMBUS_READ) {
			ret = ft260_smbus_write(dev, addr, cmd, NULL, 0,
						FT260_FLAG_START);
			if (ret)
				goto smbus_exit;

			ret = ft260_i2c_read(dev, addr, data->block + 1,
					     data->block[0],
					     FT260_FLAG_START_STOP_REPEATED);
		} else {
			ret = ft260_smbus_write(dev, addr, cmd, data->block + 1,
						data->block[0],
						FT260_FLAG_START_STOP);
		}
		break;
	default:
		hid_err(hdev, "unsupported smbus transaction size %d\n", size);
		ret = -EOPNOTSUPP;
	}

smbus_exit:
	hid_hw_power(hdev, PM_HINT_NORMAL);
	mutex_unlock(&dev->lock);
	return ret;
}

static u32 ft260_functionality(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_BYTE | I2C_FUNC_SMBUS_QUICK |
	       I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA |
	       I2C_FUNC_SMBUS_BLOCK_DATA | I2C_FUNC_SMBUS_I2C_BLOCK;
}

static const struct i2c_adapter_quirks ft260_i2c_quirks = {
	.flags = I2C_AQ_COMB_WRITE_THEN_READ,
	.max_comb_1st_msg_len = 2,
};

static const struct i2c_algorithm ft260_i2c_algo = {
	.master_xfer = ft260_i2c_xfer,
	.smbus_xfer = ft260_smbus_xfer,
	.functionality = ft260_functionality,
};

static int ft260_get_system_config(struct hid_device *hdev,
				   struct ft260_get_system_status_report *cfg)
{
	int ret;
	int len = sizeof(struct ft260_get_system_status_report);

	ret = ft260_hid_feature_report_get(hdev, FT260_SYSTEM_SETTINGS,
					   (u8 *)cfg, len);
	if (ret < 0) {
		hid_err(hdev, "failed to retrieve system status\n");
		return ret;
	}
	return 0;
}

static int ft260_get_interface_type(struct hid_device *hdev, struct ft260_device *dev)
{
	struct ft260_get_system_status_report cfg;
	struct usb_interface *usbif = to_usb_interface(hdev->dev.parent);
	int interface = usbif->cur_altsetting->desc.bInterfaceNumber;
	int ret;

	ret = ft260_get_system_config(hdev, &cfg);
	if (ret < 0)
		return ret;

	ft260_dbg("interface:  0x%02x\n", interface);
	ft260_dbg("chip mode:  0x%02x\n", cfg.chip_mode);
	ft260_dbg("clock_ctl:  0x%02x\n", cfg.clock_ctl);
	ft260_dbg("i2c_enable: 0x%02x\n", cfg.i2c_enable);
	ft260_dbg("uart_mode:  0x%02x\n", cfg.uart_mode);

	switch (cfg.chip_mode) {
	case FT260_MODE_ALL:
	case FT260_MODE_BOTH:
		if (interface == 1) {
			ret = FT260_IFACE_UART;
			dev->ft260_is_serial = 1;
		} else {
			ret = FT260_IFACE_I2C;
		}
		break;
	case FT260_MODE_UART:
		ret = FT260_IFACE_UART;
		dev->ft260_is_serial = 1;
		break;
	case FT260_MODE_I2C:
		ret = FT260_IFACE_I2C;
		break;
	}

	return ret;
}

static int ft260_byte_show(struct hid_device *hdev, int id, u8 *cfg, int len,
			   u8 *field, u8 *buf)
{
	int ret;

	ret = ft260_hid_feature_report_get(hdev, id, cfg, len);
	if (ret < 0)
		return ret;

	return scnprintf(buf, PAGE_SIZE, "%d\n", *field);
}

static int ft260_word_show(struct hid_device *hdev, int id, u8 *cfg, int len,
			   u16 *field, u8 *buf)
{
	int ret;

	ret = ft260_hid_feature_report_get(hdev, id, cfg, len);
	if (ret < 0)
		return ret;

	return scnprintf(buf, PAGE_SIZE, "%d\n", le16_to_cpu(*field));
}

#define FT260_ATTR_SHOW(name, reptype, id, type, func)			       \
	static ssize_t name##_show(struct device *kdev,			       \
				   struct device_attribute *attr, char *buf)   \
	{								       \
		struct reptype rep;					       \
		struct hid_device *hdev = to_hid_device(kdev);		       \
		type *field = &rep.name;				       \
		int len = sizeof(rep);					       \
									       \
		return func(hdev, id, (u8 *)&rep, len, field, buf);	       \
	}

#define FT260_SSTAT_ATTR_SHOW(name)					       \
		FT260_ATTR_SHOW(name, ft260_get_system_status_report,	       \
				FT260_SYSTEM_SETTINGS, u8, ft260_byte_show)

#define FT260_I2CST_ATTR_SHOW(name)					       \
		FT260_ATTR_SHOW(name, ft260_get_i2c_status_report,	       \
				FT260_I2C_STATUS, u16, ft260_word_show)

#define FT260_ATTR_STORE(name, reptype, id, req, type, func)		       \
	static ssize_t name##_store(struct device *kdev,		       \
				    struct device_attribute *attr,	       \
				    const char *buf, size_t count)	       \
	{								       \
		struct reptype rep;					       \
		struct hid_device *hdev = to_hid_device(kdev);		       \
		type name;						       \
		int ret;						       \
									       \
		if (!func(buf, 10, &name)) {				       \
			rep.name = name;				       \
			rep.report = id;				       \
			rep.request = req;				       \
			ret = ft260_hid_feature_report_set(hdev, (u8 *)&rep,   \
							   sizeof(rep));       \
			if (!ret)					       \
				ret = count;				       \
		} else {						       \
			ret = -EINVAL;					       \
		}							       \
		return ret;						       \
	}

#define FT260_BYTE_ATTR_STORE(name, reptype, req)			       \
		FT260_ATTR_STORE(name, reptype, FT260_SYSTEM_SETTINGS, req,    \
				 u8, kstrtou8)

#define FT260_WORD_ATTR_STORE(name, reptype, req)			       \
		FT260_ATTR_STORE(name, reptype, FT260_SYSTEM_SETTINGS, req,    \
				 u16, kstrtou16)

FT260_SSTAT_ATTR_SHOW(chip_mode);
static DEVICE_ATTR_RO(chip_mode);

FT260_SSTAT_ATTR_SHOW(pwren_status);
static DEVICE_ATTR_RO(pwren_status);

FT260_SSTAT_ATTR_SHOW(suspend_status);
static DEVICE_ATTR_RO(suspend_status);

FT260_SSTAT_ATTR_SHOW(hid_over_i2c_en);
static DEVICE_ATTR_RO(hid_over_i2c_en);

FT260_SSTAT_ATTR_SHOW(power_saving_en);
static DEVICE_ATTR_RO(power_saving_en);

FT260_SSTAT_ATTR_SHOW(i2c_enable);
FT260_BYTE_ATTR_STORE(i2c_enable, ft260_set_i2c_mode_report,
		      FT260_SET_I2C_MODE);
static DEVICE_ATTR_RW(i2c_enable);

FT260_SSTAT_ATTR_SHOW(uart_mode);
FT260_BYTE_ATTR_STORE(uart_mode, ft260_set_uart_mode_report,
		      FT260_SET_UART_MODE);
static DEVICE_ATTR_RW(uart_mode);

FT260_SSTAT_ATTR_SHOW(clock_ctl);
FT260_BYTE_ATTR_STORE(clock_ctl, ft260_set_system_clock_report,
		      FT260_SET_CLOCK);
static DEVICE_ATTR_RW(clock_ctl);

FT260_I2CST_ATTR_SHOW(clock);
FT260_WORD_ATTR_STORE(clock, ft260_set_i2c_speed_report,
		      FT260_SET_I2C_CLOCK_SPEED);
static DEVICE_ATTR_RW(clock);

static ssize_t i2c_reset_store(struct device *kdev,
			       struct device_attribute *attr, const char *buf,
			       size_t count)
{
	struct hid_device *hdev = to_hid_device(kdev);
	int ret = ft260_i2c_reset(hdev);

	if (ret)
		return ret;
	return count;
}
static DEVICE_ATTR_WO(i2c_reset);

static const struct attribute_group ft260_attr_group = {
	.attrs = (struct attribute *[]) {
		  &dev_attr_chip_mode.attr,
		  &dev_attr_pwren_status.attr,
		  &dev_attr_suspend_status.attr,
		  &dev_attr_hid_over_i2c_en.attr,
		  &dev_attr_power_saving_en.attr,
		  &dev_attr_i2c_enable.attr,
		  &dev_attr_uart_mode.attr,
		  &dev_attr_clock_ctl.attr,
		  &dev_attr_i2c_reset.attr,
		  &dev_attr_clock.attr,
		  NULL
	}
};

/***
 * START Serial dev part
 */
static struct ft260_device *ft260_uart_table[UART_COUNT_MAX];
static DEFINE_SPINLOCK(ft260_uart_table_lock);

static int ft260_uart_add_port(struct ft260_device *port)
{
	int index, ret = -EBUSY;

	spin_lock_init(&port->write_lock);
	if (kfifo_alloc(&port->xmit_fifo, FIFO_SIZE, GFP_KERNEL))
		return -ENOMEM;

	spin_lock(&ft260_uart_table_lock);
	for (index = 0; index < UART_COUNT_MAX; index++) {
		if (!ft260_uart_table[index]) {
			port->index = index;
			ft260_uart_table[index] = port;
			ret = 0;
			break;
		}
	}
	spin_unlock(&ft260_uart_table_lock);

	return ret;
}

static void ft260_uart_port_remove(struct ft260_device *port)
{
	spin_lock(&ft260_uart_table_lock);
	ft260_uart_table[port->index] = NULL;
	spin_unlock(&ft260_uart_table_lock);

	mutex_lock(&port->port.mutex);
	/* tty_hangup is async so is this safe as is ?? */
	tty_port_tty_hangup(&port->port, false);
	mutex_unlock(&port->port.mutex);

	kfifo_free(&port->xmit_fifo);
}

static struct ft260_device *ft260_uart_port_get(unsigned int index)
{
	struct ft260_device *port;

	if (index >= UART_COUNT_MAX)
		return NULL;

	spin_lock(&ft260_uart_table_lock);
	port = ft260_uart_table[index];
	if (port)
		tty_port_get(&port->port);
	spin_unlock(&ft260_uart_table_lock);

	return port;
}

static void ft260_uart_port_put(struct ft260_device *port)
{
	tty_port_put(&port->port);
}

static int ft260_uart_open(struct tty_struct *tty, struct file *filp)
{
	int ret;
	struct ft260_device *port = tty->driver_data;

	ret = tty_port_open(&port->port, tty, filp);

	return ret;
}

static void ft260_uart_close(struct tty_struct *tty, struct file *filp)
{
	struct ft260_device *port = tty->driver_data;

	tty_port_close(&port->port, tty, filp);
}

static void ft260_uart_hangup(struct tty_struct *tty)
{
	struct ft260_device *port = tty->driver_data;

	tty_port_hangup(&port->port);
}

static int ft260_uart_transmit_chars(struct ft260_device *port)
{
	struct hid_device *hdev = port->hdev;
	struct kfifo *xmit = &port->xmit_fifo;
	struct tty_struct *tty;
	struct ft260_uart_write_request_report *rep;
	int len, data_len, ret = 0;

	tty = tty_port_tty_get(&port->port);

	data_len = kfifo_len(xmit);
	if (!tty || !data_len) {
		ret = -EINVAL;
		goto tty_out;
	}

	rep = (struct ft260_uart_write_request_report *)port->write_buf;

	do {
		len = min(data_len, FT260_WR_DATA_MAX);

		rep->report = FT260_UART_DATA_REPORT_ID(len);
		rep->length = len;

		len = kfifo_out_locked(xmit, rep->data, len, &port->write_lock);

		ret = ft260_hid_output_report(hdev, (u8 *)rep, len + sizeof(*rep));
		if (ret < 0) {
			hid_err(hdev, "Failed to start transfer, ret %d\n", ret);
			goto tty_out;
		}

		data_len -= len;
		port->icount.tx += len;
	} while (data_len > 0);

	len = kfifo_len(xmit);
	if ((FIFO_SIZE - len) > TTY_WAKEUP_WATERMARK)
		tty_wakeup(tty);

	ret = 0;

tty_out:
	tty_kref_put(tty);
	return ret;
}

static int ft260_uart_receive_chars(struct ft260_device *port,
				    u8 *data, u8 length)
{
	struct hid_device *hdev = port->hdev;
	int ret = 0;

	if (length > FT260_RD_DATA_MAX) {
		hid_err(hdev, "Received too much data (%d)\n", length);
		return -EBADR;
	}

	ret = tty_insert_flip_string(&port->port, data, length);
	if (ret != length)
		hid_err(hdev, "%d char not inserted to flip buffer\n", length - ret);
	port->icount.rx += ret;

	if (ret)
		tty_flip_buffer_push(&port->port);
	return ret;
}

static int ft260_uart_write(struct tty_struct *tty, const unsigned char *buf,
			    int count)
{
	struct ft260_device *port = tty->driver_data;
	struct hid_device *hdev = port->hdev;
	int ret;

	ret = kfifo_in_locked(&port->xmit_fifo, buf, count, &port->write_lock);
	if (ft260_uart_transmit_chars(port) != kfifo_len(&port->xmit_fifo))
		hid_err(hdev, "Failed sending all kfifo data bytes\n");

	return ret;
}

static unsigned int ft260_uart_write_room(struct tty_struct *tty)
{
	struct ft260_device *port = tty->driver_data;

	return FIFO_SIZE - kfifo_len(&port->xmit_fifo);
}

static unsigned int ft260_uart_chars_in_buffer(struct tty_struct *tty)
{
	struct ft260_device *port = tty->driver_data;

	return kfifo_len(&port->xmit_fifo);
}

static int ft260_uart_change_speed(struct ft260_device *port,
				   struct ktermios *termios,
				    struct ktermios *old)
{
	struct hid_device *hdev = port->hdev;
	unsigned int baud;
	struct ft260_configure_uart_request req;
	int ret;
	struct tty_struct *tty = tty_port_tty_get(&port->port);

	memset(&req, 0, sizeof(req));

	req.report = FT260_SYSTEM_SETTINGS;
	req.request = FT260_SET_UART_CONFIG;

	switch (termios->c_cflag & CSIZE) {
	case CS7:
		req.data_bit	= FT260_CFG_DATA_BITS_7;
		break;
	case CS5:
	case CS6:
		hid_err(hdev, "Invalid data bit size, setting to default (8 bit)\n");
		req.data_bit		= FT260_CFG_DATA_BITS_8;
		termios->c_cflag	= CS8;
		break;
	default:
	case CS8:
		req.data_bit	= FT260_CFG_DATA_BITS_8;
		break;
	}

	req.stop_bit = (termios->c_cflag & CSTOPB) ?
		FT260_CFG_STOP_TWO_BIT : FT260_CFG_STOP_ONE_BIT;

	if (termios->c_cflag & PARENB) {
		req.parity = (termios->c_cflag & PARODD) ?
			FT260_CFG_PAR_ODD : FT260_CFG_PAR_EVEN;
	} else {
		req.parity = FT260_CFG_PAR_NO;
	}

	baud = tty_termios_baud_rate(termios);
	if (baud == 0 || baud < FT260_CFG_BAUD_MIN || baud > FT260_CFG_BAUD_MAX) {
		hid_err(hdev, "Invalid baud rate %d\n", baud);
		baud = 9600;
		tty_encode_baud_rate(tty, baud, baud);
	}
	put_unaligned_le32(cpu_to_le32(baud), &req.baudrate);

	if (termios->c_cflag & CRTSCTS)
		req.flow_ctrl = FT260_CFG_FLOW_CTRL_RTS_CTS;
	else
		req.flow_ctrl = FT260_CFG_FLOW_CTRL_OFF;

	ft260_dbg("Configured termios: flow control: %d, baudrate: %d, ",
		  req.flow_ctrl, baud);
	ft260_dbg("data_bit: %d, parity: %d, stop_bit: %d, breaking: %d\n",
		  req.data_bit, req.parity,
		  req.stop_bit, req.breaking);

	req.flow_ctrl	= FT260_CFG_FLOW_CTRL_NONE;
	req.breaking	= FT260_CFG_BREAKING_NO;

	ret = ft260_hid_feature_report_set(hdev, (u8 *)&req, sizeof(req));
	if (ret < 0) {
		hid_err(hdev, "ft260_hid_feature_report_set failed: %d\n", ret);
		return ret;
	}

	return ret;
}

static void ft260_uart_set_termios(struct tty_struct *tty,
				   const struct ktermios *old_termios)
{
	struct ft260_device *port = tty->driver_data;

	ft260_uart_change_speed(port, &tty->termios, NULL);
}

static int ft260_uart_install(struct tty_driver *driver, struct tty_struct *tty)
{
	int idx = tty->index;
	struct ft260_device *port = ft260_uart_port_get(idx);
	int ret = tty_standard_install(driver, tty);

	if (ret == 0)
		/* This is the ref ft260_uart_port get provided */
		tty->driver_data = port;
	else
		ft260_uart_port_put(port);

	return ret;
}

static void ft260_uart_cleanup(struct tty_struct *tty)
{
	struct ft260_device *port = tty->driver_data;

	tty->driver_data = NULL;	/* Bug trap */
	ft260_uart_port_put(port);
}

static int ft260_uart_proc_show(struct seq_file *m, void *v)
{
	return -EINVAL;
}

static const struct tty_operations ft260_uart_ops = {
	.open			= ft260_uart_open,
	.close			= ft260_uart_close,
	.write			= ft260_uart_write,
	.write_room		= ft260_uart_write_room,
	.chars_in_buffer	= ft260_uart_chars_in_buffer,
	.set_termios		= ft260_uart_set_termios,
	.hangup			= ft260_uart_hangup,
	.install		= ft260_uart_install,
	.cleanup		= ft260_uart_cleanup,
	.proc_show		= ft260_uart_proc_show,
};

static void uart_dtr_rts(struct tty_port *tport, int onoff)
{
}

static int uart_carrier_raised(struct tty_port *tport)
{
	return -EINVAL;
}

static void ft260_uart_shutdown(struct tty_port *tport)
{
}

static int ft260_uart_activate(struct tty_port *tport, struct tty_struct *tty)
{
	struct ft260_device *port =
		container_of(tport, struct ft260_device, port);

	/*
	 * Set the TTY IO error marker - we will only clear this
	 * once we have successfully opened the port.
	 */
	set_bit(TTY_IO_ERROR, &tty->flags);

	kfifo_reset(&port->xmit_fifo);

	ft260_uart_change_speed(port, &tty->termios, NULL);

	clear_bit(TTY_IO_ERROR, &tty->flags);

	return 0;
}

static void ft260_uart_port_destroy(struct tty_port *tport)
{
}

static const struct tty_port_operations ft260_uart_port_ops = {
	.dtr_rts = uart_dtr_rts,
	.carrier_raised = uart_carrier_raised,
	.shutdown = ft260_uart_shutdown,
	.activate = ft260_uart_activate,
	.destruct = ft260_uart_port_destroy,
};

static struct tty_driver *ft260_tty_driver;
/***
 * END Serial dev part
 */

static int ft260_i2c_probe(struct hid_device *hdev, struct ft260_device *dev)
{
	int ret;

	dev->hdev = hdev;
	dev->adap.owner = THIS_MODULE;
	dev->adap.class = I2C_CLASS_HWMON;
	dev->adap.algo = &ft260_i2c_algo;
	dev->adap.quirks = &ft260_i2c_quirks;
	dev->adap.dev.parent = &hdev->dev;
	snprintf(dev->adap.name, sizeof(dev->adap.name),
		 "FT260 usb-i2c bridge on hidraw%d",
		 ((struct hidraw *)hdev->hidraw)->minor);

	ret = ft260_xfer_status(dev);
	if (ret)
		ft260_i2c_reset(hdev);

	i2c_set_adapdata(&dev->adap, dev);
	ret = i2c_add_adapter(&dev->adap);
	if (ret) {
		hid_err(hdev, "failed to add i2c adapter\n");
		return ret;
	}

	ret = sysfs_create_group(&hdev->dev.kobj, &ft260_attr_group);
	if (ret < 0) {
		hid_err(hdev, "failed to create sysfs attrs\n");
		goto err_i2c_free;
	}
err_i2c_free:
	i2c_del_adapter(&dev->adap);
	return ret;
}

static int ft260_uart_probe(struct hid_device *hdev, struct ft260_device *dev)
{
	struct ft260_configure_uart_request req;
	int ret;
	struct device *devt;

	tty_port_init(&dev->port);
	dev->port.ops = &ft260_uart_port_ops;

	ret = ft260_uart_add_port(dev);
	if (ret) {
		hid_err(hdev, "failed to add port\n");
		return ret;
	}
	hid_info(hdev, "tty driver: %px, ports: %px, hdev->dev: %px\n",
		ft260_tty_driver, ft260_tty_driver ?
		ft260_tty_driver->ports : NULL,
		&hdev->dev);
	devt = tty_port_register_device_attr(&dev->port,
					     ft260_tty_driver,
					     dev->index, &hdev->dev,
					     dev, NULL);
	if (IS_ERR(devt)) {
		hid_err(hdev, "failed to register tty port\n");
		ret = PTR_ERR(devt);
		goto err_register_tty;
	}

	/* Send Feature Report to Configure FT260 as UART 9600-8-N-1 */
	req.report	= FT260_SYSTEM_SETTINGS;
	req.request	= FT260_SET_UART_CONFIG;
	req.flow_ctrl	= FT260_CFG_FLOW_CTRL_NONE;
	put_unaligned_le32(cpu_to_le32(9600), &req.baudrate);
	req.data_bit	= FT260_CFG_DATA_BITS_8;
	req.parity	= FT260_CFG_PAR_NO;
	req.stop_bit	= FT260_CFG_STOP_ONE_BIT;
	req.breaking	= FT260_CFG_BREAKING_NO;

	ret = ft260_hid_feature_report_set(hdev, (u8 *)&req, sizeof(req));
	if (ret < 0) {
		hid_err(hdev, "ft260_hid_feature_report_set failed: %d\n", ret);
		goto err_hid_report;
	}
	return 0;

err_hid_report:
	tty_port_unregister_device(&dev->port, ft260_tty_driver, dev->index);
err_register_tty:
	ft260_uart_port_remove(dev);
	return ret;
}

static int ft260_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct ft260_device *dev;
	struct ft260_get_chip_version_report version;
	int ret;

	if (!hid_is_usb(hdev))
		return -EINVAL;

	dev = devm_kzalloc(&hdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "failed to parse HID\n");
		return ret;
	}

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret) {
		hid_err(hdev, "failed to start HID HW\n");
		return ret;
	}

	ret = hid_hw_open(hdev);
	if (ret) {
		hid_err(hdev, "failed to open HID HW\n");
		goto err_hid_stop;
	}

	ret = ft260_hid_feature_report_get(hdev, FT260_CHIP_VERSION,
					   (u8 *)&version, sizeof(version));
	if (ret < 0) {
		hid_err(hdev, "failed to retrieve chip version\n");
		goto err_hid_close;
	}

	hid_info(hdev, "chip code: %02x%02x %02x%02x\n",
		 version.chip_code[0], version.chip_code[1],
		 version.chip_code[2], version.chip_code[3]);

	ret = ft260_get_interface_type(hdev, dev);
	if (ret <= FT260_IFACE_NONE) {
		goto err_hid_close;
	}

	hid_set_drvdata(hdev, dev);
	dev->hdev = hdev;
	mutex_init(&dev->lock);
	init_completion(&dev->wait);

	if (!dev->ft260_is_serial) {
		ret = ft260_i2c_probe(hdev, dev);
		if (ret)
			goto err_hid_close;
	} else {
		ret = ft260_uart_probe(hdev, dev);
		if (ret)
			goto err_hid_close;
	}

	return 0;

err_hid_close:
	hid_hw_close(hdev);
err_hid_stop:
	hid_hw_stop(hdev);
	return ret;
}

static void ft260_remove(struct hid_device *hdev)
{
	struct ft260_device *dev = hid_get_drvdata(hdev);

	if (!dev)
		return;

	if (dev->ft260_is_serial) {
		tty_port_unregister_device(&dev->port, ft260_tty_driver, dev->index);
		ft260_uart_port_remove(dev);
	} else {
		sysfs_remove_group(&hdev->dev.kobj, &ft260_attr_group);
		i2c_del_adapter(&dev->adap);
	}

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static int ft260_raw_event(struct hid_device *hdev, struct hid_report *report,
			   u8 *data, int size)
{
	struct ft260_device *dev = hid_get_drvdata(hdev);
	struct ft260_input_report *xfer = (void *)data;
	int ret = 1;

	if (xfer->report >= FT260_I2C_REPORT_MIN &&
	    xfer->report <= FT260_I2C_REPORT_MAX) {
		memcpy(&dev->read_buf[dev->read_idx], &xfer->data,
		       xfer->length);
		dev->read_idx += xfer->length;

		if (dev->read_idx == dev->read_len)
			complete(&dev->wait);
	} else if (xfer->length > FT260_RD_DATA_MAX) {
		hid_err(hdev, "Received data too long (%d)\n", xfer->length);
		return -EBADR;
	} else if (xfer->report >= FT260_UART_REPORT_MIN &&
		   xfer->report <= FT260_UART_REPORT_MAX) {
		ret = ft260_uart_receive_chars(dev, xfer->data, xfer->length);
	} else {
		hid_err(hdev, "unknown report: %#02x\n", xfer->report);
		return 0;
	}
	return ret;
}

static struct hid_driver ft260_driver = {
	.name		= "ft260",
	.id_table	= ft260_devices,
	.probe		= ft260_probe,
	.remove		= ft260_remove,
	.raw_event	= ft260_raw_event,
};

static int __init ft260_driver_init(void)
{
	int ret;

	ft260_tty_driver = tty_alloc_driver(UART_COUNT_MAX,
					    TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV);

	ft260_tty_driver->driver_name = "ft260_ser";
	ft260_tty_driver->name = "ttyFT";
	ft260_tty_driver->major = 0;
	ft260_tty_driver->minor_start = 0;
	ft260_tty_driver->type = TTY_DRIVER_TYPE_SERIAL;
	ft260_tty_driver->subtype = SERIAL_TYPE_NORMAL;
	ft260_tty_driver->init_termios = tty_std_termios;
	ft260_tty_driver->init_termios.c_cflag = B9600 | CS8 | CREAD | HUPCL | CLOCAL;
	ft260_tty_driver->init_termios.c_ispeed = 9600;
	ft260_tty_driver->init_termios.c_ospeed = 9600;
	tty_set_operations(ft260_tty_driver, &ft260_uart_ops);

	ret = tty_register_driver(ft260_tty_driver);
	if (ret) {
		pr_err("tty_register_driver failed: %d\n", ret);
		goto err_reg_driver;
	}

	ret = hid_register_driver(&(ft260_driver));
	if (ret) {
		pr_err("hid_register_driver failed: %d\n", ret);
		goto err_reg_hid;
	}

	return 0;

err_reg_hid:
	tty_unregister_driver(ft260_tty_driver);
err_reg_driver:
	tty_driver_kref_put(ft260_tty_driver);

	return ret;
}

static void __exit ft260_driver_exit(void)
{
	hid_unregister_driver(&(ft260_driver));
	tty_unregister_driver(ft260_tty_driver);
	tty_driver_kref_put(ft260_tty_driver);
}

module_init(ft260_driver_init);
module_exit(ft260_driver_exit);

MODULE_DESCRIPTION("FTDI FT260 USB HID to I2C host bridge and TTY driver");
MODULE_AUTHOR("Michael Zaidman <michael.zaidman@gmail.com>");
MODULE_LICENSE("GPL");
