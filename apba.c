/*
 * Copyright (C) 2015 Motorola Mobility LLC
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kfifo.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>
#include <linux/clk.h>
#include <linux/mtd/mtd.h>
#include <linux/workqueue.h>

#include "apba.h"
#include "kernel_ver.h"
#include "cust_kernel_ver.h"
#include "mods_nw.h"
#include "mods_protocols.h"
#include "mods_uart.h"
#include "mods_uart_pm.h"
#include "muc.h"

#define MAX_PARTITION_NAME (16)

#define FFFF_EXT (".ffff")
#define BIN_EXT  (".bin")

#define APBA_FIRMWARE_PARTITION ("apba")
#define APBA_FIRMWARE_NAME ("apba.ffff")

#define APBA_NUM_GPIOS (8)
#define APBA_MAX_SEQ   (APBA_NUM_GPIOS*3*2)

#define APBE_RESET_DELAY (250)

struct apba_seq {
	u32 val[APBA_MAX_SEQ];
	size_t len;
};

struct apba_ctrl {
	struct device *dev;
	struct clk *mclk;
	struct pinctrl *pinctrl;
	struct pinctrl_state *pinctrl_state_default;
	struct pinctrl_state *pinctrl_state_active;
	int gpio_cnt;
	int gpios[APBA_NUM_GPIOS];
	const char *gpio_labels[APBA_NUM_GPIOS];
	int int_index;
	int irq;
	struct apba_seq enable_seq;
	struct apba_seq disable_seq;
	struct apba_seq wake_assert_seq;
	struct apba_seq wake_deassert_seq;
	struct apba_seq flash_start_seq;
	struct apba_seq flash_end_seq;
	void *mods_uart;
	int desired_on;
	struct mutex log_mutex;
	struct completion comp;
	struct completion baud_comp;
	struct completion mode_comp;
	uint8_t master_intf;
	uint8_t mode;
	bool flash_dev_populated;
} *g_ctrl;

/* message from APBA Ctrl driver in kernel */
#pragma pack(push, 1)
struct apba_ctrl_int_reason_resp {
	struct apba_ctrl_msg_hdr hdr;
	__le16 reason;
};
#pragma pack(pop)

/* message to APBA for mode request */
#pragma pack(push, 1)
struct apba_mode_req {
	struct apba_ctrl_msg_hdr hdr;
	uint8_t mode;
};
#pragma pack(pop)

/* message to APBA for UART baud update request */
#pragma pack(push, 1)
struct apba_baud_req {
	struct apba_ctrl_msg_hdr hdr;
	__le32 baud;
};
#pragma pack(pop)

/* message from APBA acknowledging UART baud update */
#pragma pack(push, 1)
struct apba_baud_ack {
	struct apba_ctrl_msg_hdr hdr;
	__le32 baud;
	uint8_t accepted;
};
#pragma pack(pop)

enum {
	APBA_INT_REASON_NONE,
	APBA_INT_APBE_ON,
	APBA_INT_APBE_RESET,
	APBA_INT_APBE_CONNECTED,
	APBA_INT_APBE_DISCONNECTED,
};

#define APBA_LOG_SIZE	SZ_16K
static DEFINE_KFIFO(apba_log_fifo, char, APBA_LOG_SIZE);

/* used as temporary buffer to pop out content from FIFO */
static char fifo_overflow[APBA_MSG_SIZE_MAX];

#define APBA_LOG_REQ_TIMEOUT	1000 /* ms */
#define APBA_MODE_REQ_TIMEOUT	1000 /* ms */
#define APBA_BAUD_REQ_TIMEOUT	1000 /* ms */

#define MIN(a, b) (((a) < (b)) ? (a) : (b))

struct apbe_attach_work_struct {
	struct work_struct work;
	int present;
};

static struct delayed_work apba_disable_work;

static int apba_mtd_erase(struct mtd_info *mtd_info,
	 unsigned int start, unsigned int len)
{
	int err;
	struct erase_info ei = {0};

	ei.addr = start;
	ei.len = len;
	ei.mtd = mtd_info;
	err = mtd_info->_erase(mtd_info, &ei);
	return err;
}

static struct mtd_info * apba_init_mtd_module(const char *partition_name)
{
	struct mtd_info *mtd_info;
	int num;

	for (num = 0; num < 16; num++) {
		mtd_info = get_mtd_device(NULL, num);
		if (IS_ERR(mtd_info)) {
			pr_debug("%s: No device for num %d\n", __func__, num);
			continue;
		}

		if (mtd_info->type == MTD_ABSENT) {
			put_mtd_device(mtd_info);
			continue;
		}

		if (strcmp(mtd_info->name, partition_name)) {
			put_mtd_device(mtd_info);
			continue;
		}

		pr_debug("%s: MTD name: %s\n", __func__, mtd_info->name);
		pr_debug("%s: MTD type: %d\n", __func__, mtd_info->type);
		pr_debug("%s: MTD total size : %ld bytes\n", __func__,
			 (long)mtd_info->size);
		pr_debug("%s: MTD erase size : %ld bytes\n", __func__,
			 (long)mtd_info->erasesize);

		return mtd_info;
	}

	return NULL;
}

static int apba_parse_seq(struct device *dev, const char *name,
	struct apba_seq *seq)
{
	int ret;
	int cnt = 0;
	struct property *pp = of_find_property(dev->of_node, name, &cnt);

	cnt /= sizeof(u32);
	if (!pp || cnt == 0 || cnt > seq->len || cnt % 3) {
		pr_err("%s: error reading property %s, cnt = %d\n",
			__func__, name, cnt);
		ret = -EINVAL;
	} else {
		ret = of_property_read_u32_array(dev->of_node, name,
			seq->val, cnt);
		if (ret) {
			pr_err("%s: unable to read %s, ret = %d\n",
				__func__, name, ret);
		} else {
			seq->len = cnt;
		}
	}

	return ret;
}

static void apba_seq(struct apba_ctrl *ctrl, struct apba_seq *seq)
{
	size_t i;

	for (i = 0; i < seq->len; i += 3) {
		u32 index = seq->val[i];
		int value = (int)seq->val[i+1];
		unsigned long delay = (unsigned long)seq->val[i+2];

		/* Set a gpio (if valid). */
		if (index < ARRAY_SIZE(ctrl->gpios)) {
			int gpio = ctrl->gpios[index];

			if (gpio_is_valid(gpio)) {
				pr_debug("%s: set gpio=%d, value=%u\n",
					__func__, gpio, value);
				gpio_set_value(gpio, value);
			}
		}

		/* Delay (if valid). */
		if (delay) {
			usleep_range(delay * 1000, delay * 1000);
			pr_debug("%s: delay=%lu\n",
				__func__, delay);
		}
	}
}

static void apba_on(struct apba_ctrl *ctrl, bool on)
{
	pr_info("%s: %s\n", __func__, on ? "on" : "off");

	mods_ext_bus_vote(on);

	if (on) {
		if (ctrl->mods_uart)
			mods_uart_open(ctrl->mods_uart);
		apba_seq(ctrl, &ctrl->enable_seq);
		if (ctrl->mods_uart)
			mods_uart_pm_on(ctrl->mods_uart, true);
	} else {
		ctrl->mode = 0;
		apba_seq(ctrl, &ctrl->disable_seq);
		if (ctrl->mods_uart) {
			mods_uart_pm_on(ctrl->mods_uart, false);
			mods_uart_close(ctrl->mods_uart);
		}
	}
}

static void populate_transports_node(struct apba_ctrl *ctrl)
{
	struct device_node *np;

	np = of_find_node_by_name(ctrl->dev->of_node, "transports");
	if (!np) {
		dev_warn(ctrl->dev, "transports node not present\n");
		return;
	}

	np = of_find_compatible_node(np, NULL, "moto,apba-spi-transfer");
	if (!np) {
		dev_warn(ctrl->dev, "SPI transport device not present\n");
		return;
	}

	dev_dbg(ctrl->dev, "%s: creating platform device\n", __func__);
	if (!of_platform_device_create(np, NULL, ctrl->dev)) {
		dev_warn(ctrl->dev, "failed to populate transport devices\n");
	} else {
		ctrl->flash_dev_populated = true;
	}
}

/*
 * for flash on:
 *   Configure SPI interface pin control for use of the SPI interface to
 *    the flash part.
 *   Set the PMIC GPIO and any other GPIOs needed for AP to interface with
 *    the flash part on the SPI interface.
 *   Search device tree entry for a transport node which holds the SPI
 *    interface info.
 *   If the transport node is found, probe the flash device on the SPI
 *    interface.
 *
 * for flash off:
 *   reverse the above conditions.
 */
static void apba_flash_on(struct apba_ctrl *ctrl, bool on)
{
	int ret;

	if (on) {
		if (!IS_ERR(ctrl->pinctrl_state_active)) {
			dev_dbg(ctrl->dev, "%s: Pinctrl set active\n", __func__);
			ret = pinctrl_select_state(ctrl->pinctrl,
						   ctrl->pinctrl_state_active);
			if (ret)
				dev_err(ctrl->dev,
					"%s: Pinctrl set failed %d\n",
					__func__, ret);
		}

		apba_seq(ctrl, &ctrl->flash_start_seq);

		/* Register SPI transport for shared muc_spi and spi_flash */
		muc_register_spi_flash();

		populate_transports_node(ctrl);
	} else {
		if (ctrl->flash_dev_populated) {
			of_platform_depopulate(ctrl->dev);
			ctrl->flash_dev_populated = false;
		}

		apba_seq(ctrl, &ctrl->flash_end_seq);

		muc_deregister_spi_flash();

		dev_dbg(ctrl->dev, "%s: Pinctrl set default\n", __func__);
		ret = pinctrl_select_state(ctrl->pinctrl,
					   ctrl->pinctrl_state_default);
		if (ret)
			dev_err(ctrl->dev,
				"%s: Pinctrl set default failed %d\n",
				__func__, ret);
	}
}

static int apba_erase_partition(struct apba_ctrl *ctrl, const char *partition)
{
	struct mtd_info *mtd_info;
	int err;

	if (!ctrl)
		return -EINVAL;

	/* Disable the APBA so that it does not access the flash. */
	apba_on(ctrl, false);

	apba_flash_on(ctrl, true);

	mtd_info = apba_init_mtd_module(partition);
	if (!mtd_info) {
		pr_err("%s: mtd init module failed for %s, err=%d\n",
			__func__, partition, err);
		err = -ENODEV;
		goto no_mtd;
	}

	/* Erase the flash */
	err = apba_mtd_erase(mtd_info, 0, mtd_info->size);
	if (err < 0) {
		pr_err("%s: mtd erase failed for %s, err=%d\n",
			__func__, partition, err);
		goto cleanup;
	}

	pr_debug("%s: %s complete\n", __func__, partition);

cleanup:
	put_mtd_device(mtd_info);

no_mtd:
	apba_flash_on(ctrl, false);
	if (ctrl->desired_on)
		apba_on(ctrl, true);

	return err;
}

static ssize_t erase_partition_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct apba_ctrl *ctrl = platform_get_drvdata(pdev);
	char partition[MAX_PARTITION_NAME + 1];
	size_t partition_name_sz;
	int err;

	partition_name_sz = count;
	if (partition_name_sz && buf[partition_name_sz - 1] == '\n')
		partition_name_sz--;

	if (!partition_name_sz || (partition_name_sz >= sizeof(partition))) {
		pr_err("%s: partition name too large %s\n",
			__func__, buf);
		return -EINVAL;
	}

	memcpy(partition, buf, partition_name_sz);
	partition[partition_name_sz] = 0;
	pr_debug("%s: partition=%s\n", __func__, partition);

	err = apba_erase_partition(ctrl, partition);
	if (err < 0)
		pr_err("%s: flashing erase err=%d\n", __func__, err);

	return err ? err : count;
}

static DEVICE_ATTR_WO(erase_partition);

static int apba_compare_partition(struct mtd_info *mtd_info,
	const struct firmware *fw)
{
	int err;
	void *ffff;
	size_t retlen = 0;
	/* Assume different */
	int compare_result = 1;

	ffff = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!ffff)
		goto skip_compare;

	err = mtd_info->_read(mtd_info, 0, PAGE_SIZE, &retlen, ffff);
	if (err < 0)
		goto cleanup;

	if (retlen < PAGE_SIZE)
		goto cleanup;

	compare_result = memcmp(ffff, fw->data, PAGE_SIZE);

cleanup:
	kfree(ffff);

skip_compare:
	return compare_result;
}

static int apba_flash_partition(struct apba_ctrl *ctrl,
	const char *partition, const struct firmware *fw)
{
	struct mtd_info *mtd_info;
	int err;
	size_t retlen = 0;
	int compare_result;

	if (!fw || !ctrl)
		return -EINVAL;

	/* Disable the APBA so that it does not access the flash. */
	apba_on(ctrl, false);

	apba_flash_on(ctrl, true);

	mtd_info = apba_init_mtd_module(partition);
	if (!mtd_info) {
		pr_err("%s: mtd init module failed for %s, err=%d\n",
			__func__, partition, err);
		err = -ENODEV;
		goto no_mtd;
	}

	/* Before erasing and flashing, compare the firmware and the partition
	 * If they match, skip the process.  If anything fails during the
	 * comparison, then flash.
	 */
	compare_result = apba_compare_partition(mtd_info, fw);
	if (compare_result == 0) {
		pr_info("%s: firmware unchanged, skipping flash\n", __func__);
		err = 0;
		goto cleanup;
	}

	/* Erase the flash */
	err = apba_mtd_erase(mtd_info, 0, mtd_info->size);
	if (err < 0) {
		pr_err("%s: mtd flash failed for %s, err=%d\n",
			__func__, partition, err);
		goto cleanup;
	}

	err = mtd_info->_write(mtd_info, 0, fw->size, &retlen, fw->data);

	pr_debug("%s: %s complete\n", __func__, partition);

cleanup:
	put_mtd_device(mtd_info);

no_mtd:
	apba_flash_on(ctrl, false);
	if (ctrl->desired_on)
		apba_on(ctrl, true);

	return err;
}

static ssize_t flash_partition_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct apba_ctrl *ctrl = platform_get_drvdata(pdev);
	char partition[MAX_PARTITION_NAME + 1];
	size_t partition_name_sz;
	/* Null-termination accounted for in *_EXT macros. */
	char fw_name[MAX_PARTITION_NAME + max(sizeof(FFFF_EXT), sizeof(BIN_EXT))];
	const struct firmware *fw = NULL;
	int err;

	partition_name_sz = count;
	if (partition_name_sz && buf[partition_name_sz - 1] == '\n')
		partition_name_sz--;

	if (!partition_name_sz || (partition_name_sz >= sizeof(partition))) {
		pr_err("%s: partition name too large %s\n",
			__func__, buf);
		return -EINVAL;
	}

	/* Try .ffff extension first. */
	memcpy(fw_name, buf, partition_name_sz);
	memcpy(fw_name + partition_name_sz, FFFF_EXT, sizeof(FFFF_EXT));

	memcpy(partition, buf, partition_name_sz);
	partition[partition_name_sz] = 0;

	err = request_firmware(&fw, fw_name, ctrl->dev);
	if (err < 0) {
		pr_debug("%s: request firmware failed for %s, err=%d\n",
			__func__, partition, err);

		/* Fallback and try .bin extension. */
		memcpy(fw_name + partition_name_sz, BIN_EXT, sizeof(BIN_EXT));
		err = request_firmware(&fw, fw_name, ctrl->dev);
	}

	if (err < 0) {
		pr_err("%s: request firmware failed for %s, err=%d\n",
			__func__, partition, err);
		return err;
	}

	if (!fw || !fw->size) {
		pr_err("%s: firmware invalid for %s\n",
			__func__, partition);
		return -EINVAL;
	}

	pr_debug("%s: partition=%s, fw=%s, size=%zu\n",
		__func__, partition, fw_name, fw->size);

	err = apba_flash_partition(ctrl, partition, fw);
	if (err < 0)
		pr_err("%s: flashing failed for %s, err=%d\n",
			__func__, partition, err);

	release_firmware(fw);
	return err ? err : count;
}

static DEVICE_ATTR_WO(flash_partition);

static ssize_t apba_enable_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	if (!g_ctrl)
		return 0;

	return scnprintf(buf, PAGE_SIZE, "%d\n", g_ctrl->desired_on);
}

static ssize_t flash_enable_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	if (!g_ctrl)
		return 0;

	return scnprintf(buf, PAGE_SIZE, "%d\n", (int)g_ctrl->flash_dev_populated);
}

static ssize_t flash_enable_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned long val;

	if (kstrtoul(buf, 10, &val) < 0)
		return -EINVAL;
	else if (val != 0 && val != 1)
		return -EINVAL;

	apba_flash_on(g_ctrl, val ? true : false);

	return count;
}

static DEVICE_ATTR_RW(flash_enable);

static ssize_t apba_enable_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned long val;

	if (kstrtoul(buf, 10, &val) < 0)
		return -EINVAL;
	else if (val != 0 && val != 1)
		return -EINVAL;

	if (val)
		apba_enable();
	else
		apba_disable();

	return count;
}

static DEVICE_ATTR_RW(apba_enable);

static ssize_t apba_mode_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	if (!g_ctrl)
		return 0;

	return scnprintf(buf, 4, "%d\n", g_ctrl->mode);
}

static ssize_t apba_mode_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned long val;
	struct apba_mode_req msg;

	if (!g_ctrl || !g_ctrl->mods_uart)
		return 0;

	if (kstrtoul(buf, 10, &val) < 0)
		return -EINVAL;

	msg.hdr.type = cpu_to_le16(APBA_CTRL_MODE_REQUEST);
	msg.hdr.size = cpu_to_le16(sizeof(msg.mode));
	msg.mode = (uint8_t)val;

	if (mods_uart_apba_send(g_ctrl->mods_uart,
				(uint8_t *)&msg, sizeof(msg), 0) != 0) {
		pr_err("%s: failed to send MODE\n", __func__);
		return count;
	}

	if (!wait_for_completion_timeout(
		    &g_ctrl->mode_comp,
		    msecs_to_jiffies(APBA_MODE_REQ_TIMEOUT))) {
		pr_err("%s: timeout for MODE\n", __func__);
		return count;
	}

	g_ctrl->mode = val;

	return count;
}

static DEVICE_ATTR_RW(apba_mode);

static ssize_t apba_baud_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	if (!g_ctrl || !g_ctrl->mods_uart)
		return 0;

	return scnprintf(buf, PAGE_SIZE, "%d\n", mods_uart_get_baud(g_ctrl->mods_uart));
}

static ssize_t apba_baud_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned long val;
	struct apba_baud_req msg;

	if (!g_ctrl || !g_ctrl->mods_uart)
		return -ENODEV;

	if (kstrtoul(buf, 10, &val) < 0)
		return -EINVAL;

	msg.hdr.type = cpu_to_le16(APBA_CTRL_BAUD_REQUEST);
	msg.hdr.size = cpu_to_le16(sizeof(msg.baud));
	msg.baud = cpu_to_le32(val);

	if (mods_uart_apba_send(g_ctrl->mods_uart,
				(uint8_t *)&msg, sizeof(msg), 0) != 0) {
		pr_err("%s: failed to send BAUD\n", __func__);
		return -EIO;
	}

	/* Prevent further transmissions until we receive the baud
	 * change ACK and change the baud rate.
	 */
	mods_uart_lock_tx(g_ctrl->mods_uart, true);
	if (!wait_for_completion_timeout(
		    &g_ctrl->baud_comp,
		    msecs_to_jiffies(APBA_BAUD_REQ_TIMEOUT))) {
		pr_err("%s: timeout for BAUD\n", __func__);
		mods_uart_lock_tx(g_ctrl->mods_uart, false);
		return -EAGAIN;
	}
	mods_uart_lock_tx(g_ctrl->mods_uart, false);

	return count;
}

static DEVICE_ATTR_RW(apba_baud);

static ssize_t apba_log_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct apba_ctrl_msg_hdr msg;
	int count;

	if (!g_ctrl)
		return 0;

	msg.type = cpu_to_le16(APBA_CTRL_LOG_REQUEST);
	msg.size = 0;

	if (mods_uart_apba_send(g_ctrl->mods_uart,
				(uint8_t *)&msg, sizeof(msg), 0) != 0) {
		pr_err("%s: failed to send LOG REQUEST\n", __func__);
		return 0;
	}

	if (!wait_for_completion_timeout(
		    &g_ctrl->comp,
		    msecs_to_jiffies(APBA_LOG_REQ_TIMEOUT))) {
		pr_err("%s: timeout from LOG REQUEST\n", __func__);
		return 0;
	}

	mutex_lock(&g_ctrl->log_mutex);
	count = kfifo_out(&apba_log_fifo, buf, PAGE_SIZE - 1);
	mutex_unlock(&g_ctrl->log_mutex);

	return count;
}

static DEVICE_ATTR_RO(apba_log);

static ssize_t apbe_power_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned long val;

	if (!g_ctrl)
		return -EINVAL;

	if (!g_ctrl->master_intf)
		return -EINVAL;

	if (kstrtoul(buf, 10, &val) < 0)
		return -EINVAL;
	else if (val != 0 && val != 1)
		return -EINVAL;

	mods_slave_ctrl_power(g_ctrl->master_intf, val,
		MB_CONTROL_SLAVE_MASK_APBE);

	return count;
}

static DEVICE_ATTR_WO(apbe_power);

static struct attribute *apba_attrs[] = {
	&dev_attr_erase_partition.attr,
	&dev_attr_flash_partition.attr,
	&dev_attr_flash_enable.attr,
	&dev_attr_apba_enable.attr,
	&dev_attr_apba_baud.attr,
	&dev_attr_apba_log.attr,
	&dev_attr_apba_mode.attr,
	&dev_attr_apbe_power.attr,
	NULL,
};

ATTRIBUTE_GROUPS(apba);

static void apba_firmware_callback(const struct firmware *fw,
					 void *context)
{
	struct apba_ctrl *ctrl = (struct apba_ctrl *)context;
	int err;

	if (!ctrl) {
		pr_err("%s: invalid ctrl\n", __func__);
		return;
	}

	if (!fw) {
		pr_err("%s: no firmware available\n", __func__);
		apba_flash_on(ctrl, false);
		if (ctrl->desired_on)
			apba_on(ctrl, true);
	} else {
		pr_debug("%s: size=%zu data=%p\n", __func__, fw->size,
			fw->data);

		err = apba_flash_partition(ctrl, APBA_FIRMWARE_PARTITION, fw);
		if (err < 0)
			pr_err("%s: flashing failed err=%d\n", __func__, err);

		/* TODO: notify system, in case of error */
		release_firmware(fw);
	}

	/* Flashing is done, let's let muc core probe finish. */
	muc_enable_det();
}

static irqreturn_t apba_isr(int irq, void *data)
{
	struct apba_ctrl *ctrl = (struct apba_ctrl *)data;
	int value = gpio_get_value(ctrl->gpios[ctrl->int_index]);

	pr_debug("%s: ctrl=%p, value=%d\n", __func__, ctrl, value);


	if (!ctrl->desired_on || !ctrl->mods_uart) {
		pr_err("%s: int ignored\n", __func__);
		return IRQ_HANDLED;
	}

	mods_uart_pm_handle_wake_interrupt(ctrl->mods_uart);

	return IRQ_HANDLED;
}

static int apba_int_setup(struct apba_ctrl *ctrl,
	struct device *dev)
{
	int ret;
	int gpio;
	unsigned int flags = IRQF_TRIGGER_FALLING | IRQF_ONESHOT;

	ret = of_property_read_u32(dev->of_node,
		"mmi,int-index", &ctrl->int_index);
	if (ret) {
		dev_err(dev, "failed to read int index.\n");
		return ret;
	}

	if (ctrl->int_index < 0 || ctrl->int_index >= ctrl->gpio_cnt) {
		dev_err(dev, "int index out of range: %d\n", ctrl->int_index);
		return -EINVAL;
	}

	gpio = ctrl->gpios[ctrl->int_index];
	ctrl->irq = gpio_to_irq(gpio);
	dev_dbg(dev, "irq: gpio=%d irq=%d\n", gpio, ctrl->irq);

	ret = devm_request_threaded_irq(dev, ctrl->irq, NULL /* handler */,
		apba_isr, flags, "apba_ctrl", ctrl);
	if (ret) {
		dev_err(dev, "irq request failed: %d\n", ret);
		return ret;
	}

	enable_irq_wake(ctrl->irq);

	return ret;
}

static void apba_gpio_free(struct apba_ctrl *ctrl, struct device *dev)
{
	int i;

	for (i = 0; i < ctrl->gpio_cnt; i++) {
		sysfs_remove_link(&dev->kobj, ctrl->gpio_labels[i]);
		gpio_unexport(ctrl->gpios[i]);
	}
}

static int apba_gpio_setup(struct apba_ctrl *ctrl, struct device *dev)
{
	int i;
	int gpio_cnt = of_gpio_count(dev->of_node);
	const char *label_prop = "mmi,gpio-labels";
	int label_cnt = of_property_count_strings(dev->of_node, label_prop);
	int ret;

	if (gpio_cnt <= 0) {
		dev_err(dev, "No GPIOs were defined\n");
		return -EINVAL;
	}

	if (gpio_cnt > ARRAY_SIZE(ctrl->gpios)) {
		dev_err(dev, "%s: gpio count is greater than %zu.\n",
			__func__, ARRAY_SIZE(ctrl->gpios));
		return -EINVAL;
	}

	if (label_cnt != gpio_cnt) {
		dev_err(dev, "%s: label count does not match gpio count.\n",
			__func__);
		return -EINVAL;
	}

	for (i = 0; i < gpio_cnt; i++) {
		enum of_gpio_flags flags = 0;
		int gpio;
		const char *label = NULL;

		gpio = of_get_gpio_flags(dev->of_node, i, &flags);
		if (!gpio_is_valid(gpio)) {
			dev_err(dev, "of_get_gpio failed: %d\n", gpio);
			ret = -EINVAL;
			goto gpio_cleanup;
		}

		ret = of_property_read_string_index(dev->of_node,
					label_prop, i, &label);
		if (ret) {
			dev_err(dev, "reading label failed: %d\n", ret);
			goto gpio_cleanup;
		}

		ret = devm_gpio_request_one(dev, gpio, flags, label);
		if (ret)
			goto gpio_cleanup;

		ret = gpio_export(gpio, true);
		if (ret)
			goto gpio_cleanup;

		ret = gpio_export_link(dev, label, gpio);
		if (ret) {
			gpio_unexport(gpio);
			goto gpio_cleanup;
		}

		dev_dbg(dev, "%s: gpio=%d, flags=0x%x, label=%s\n",
			__func__, gpio, flags, label);

		ctrl->gpios[i] = gpio;
		ctrl->gpio_labels[i] = label;
		ctrl->gpio_cnt++;
	}

	return 0;

gpio_cleanup:
	apba_gpio_free(ctrl, dev);

	return ret;
}

void apba_wake_assert(bool assert)
{
	if (!g_ctrl)
		return;

	if (assert)
		apba_seq(g_ctrl, &g_ctrl->wake_assert_seq);
	else
		apba_seq(g_ctrl, &g_ctrl->wake_deassert_seq);
}

int apba_uart_register(void *mods_uart)
{
	if (!g_ctrl)
		return -ENODEV;

	g_ctrl->mods_uart = mods_uart;
	return 0;
}

static void apba_apbe_attach_work_func(struct work_struct *work)
{
	struct apbe_attach_work_struct *apbe;

	apbe = container_of(work, struct apbe_attach_work_struct, work);

	if (g_ctrl && g_ctrl->mods_uart)
		mod_attach(g_ctrl->mods_uart, apbe->present);

	kfree(apbe);
}

static void apba_disable_work_func(struct work_struct *work)
{
	apba_disable();
}

static void apba_notify_abpe_attach(int present)
{
	struct apbe_attach_work_struct *apbe;

	if (!g_ctrl)
		return;

	apbe = kzalloc(sizeof(struct apbe_attach_work_struct), GFP_KERNEL);

	if (!apbe)
		return;

	apbe->present = present;
	INIT_WORK(&apbe->work, apba_apbe_attach_work_func);
	schedule_work(&apbe->work);
}

static void apba_action_on_int_reason(uint16_t reason)
{
	pr_info("%s: %d\n", __func__, reason);

	if (!g_ctrl || !g_ctrl->master_intf)
		return;

	switch (reason) {
	case APBA_INT_APBE_ON:
		mods_slave_ctrl_power(g_ctrl->master_intf,
			MB_CONTROL_SLAVE_POWER_ON, MB_CONTROL_SLAVE_MASK_APBE);
		break;
	case APBA_INT_APBE_RESET:
		mods_slave_ctrl_power(g_ctrl->master_intf,
			MB_CONTROL_SLAVE_POWER_OFF, MB_CONTROL_SLAVE_MASK_APBE);
		msleep(APBE_RESET_DELAY);
		mods_slave_ctrl_power(g_ctrl->master_intf,
			MB_CONTROL_SLAVE_POWER_ON, MB_CONTROL_SLAVE_MASK_APBE);
		break;
	case APBA_INT_APBE_CONNECTED:
		apba_notify_abpe_attach(1);
		break;
	case APBA_INT_APBE_DISCONNECTED:
		mods_slave_ctrl_power(g_ctrl->master_intf,
			MB_CONTROL_SLAVE_POWER_OFF, MB_CONTROL_SLAVE_MASK_APBE);
		apba_notify_abpe_attach(0);
		break;
	default:
		pr_debug("%s: Unknown int reason (%d) received.\n",
			 __func__, reason);
		break;
	}
}

void apba_handle_message(uint8_t *payload, size_t len)
{
	int of;
	int ret;

	struct apba_ctrl_msg_hdr *msg;
	struct apba_baud_ack *baud_ack;

	if (!g_ctrl)
		return;

	if (len < sizeof(struct apba_ctrl_msg_hdr)) {
		pr_err("%s: Invalid message received.\n", __func__);
		return;
	}

	msg = (struct apba_ctrl_msg_hdr *)payload;

	switch (le16_to_cpu(msg->type)) {
	case APBA_CTRL_INT_REASON:
		if (len >= sizeof(struct apba_ctrl_int_reason_resp)) {
			struct apba_ctrl_int_reason_resp *resp;

			resp = (struct apba_ctrl_int_reason_resp *)payload;
			apba_action_on_int_reason(le16_to_cpu(resp->reason));
		}
		break;
	case APBA_CTRL_PM_WAKE_ACK:
	case APBA_CTRL_PM_SLEEP_ACK:
	case APBA_CTRL_PM_SLEEP_IND:
		mods_uart_pm_handle_events(g_ctrl->mods_uart,
					   le16_to_cpu(msg->type));
		break;
	case APBA_CTRL_LOG_IND:
		mutex_lock(&g_ctrl->log_mutex);
		of = kfifo_len(&apba_log_fifo) + msg->size - APBA_LOG_SIZE;
		if (of > 0) {
			/* pop out from older content if buffer is full */
			of = kfifo_out(&apba_log_fifo, fifo_overflow,
				       MIN(of, APBA_MSG_SIZE_MAX));
		}
		kfifo_in(&apba_log_fifo,
			 payload + sizeof(*msg), msg->size);
		mutex_unlock(&g_ctrl->log_mutex);
		break;
	case APBA_CTRL_LOG_REQUEST:
		complete(&g_ctrl->comp);
		break;
	case APBA_CTRL_BAUD_ACK:
		baud_ack = (struct apba_baud_ack *)payload;

		pr_debug("%s: got baud ack %d %d\n", __func__,
			 le32_to_cpu(baud_ack->baud), baud_ack->accepted);

		if (baud_ack->accepted) {
			ret = mods_uart_update_baud(g_ctrl->mods_uart,
						    le32_to_cpu(baud_ack->baud));
			if (ret)
				pr_err("%s: baud update failed: %d\n",
					__func__, ret);
		}
		complete(&g_ctrl->baud_comp);
		break;
	case APBA_CTRL_MODE_REQUEST:
		complete(&g_ctrl->mode_comp);
		break;
	default:
		pr_err("%s: Unknown message received.\n", __func__);
		break;
	}
}

/*
 * muc is informing us through this callback that it has a slave present,
 * likely APBE. If it is an APBE, we should enable the APBA so that the two
 * can communicate.
 */
static void apba_slave_notify(uint8_t master_intf, uint32_t slave_mask,
				uint32_t slave_state)
{
	if (!g_ctrl)
		return;

	pr_debug("%s: master_intf=%d, slave_mask=0x%x, slave_state=0x%x\n",
		__func__, master_intf, slave_mask, slave_state);

	if (slave_mask != MB_CONTROL_SLAVE_MASK_APBE) {
		pr_debug("%s: ignore\n", __func__);
		return;
	}

	g_ctrl->master_intf = master_intf;

	switch (slave_state) {
	case SLAVE_STATE_DISABLED:
		/* don't call apba_disable() here, causes greybus
		 * operation failures as we are not done handling
		 * slave state gb message yet.
		 */
		schedule_delayed_work(&apba_disable_work, HZ);
		break;
	case SLAVE_STATE_ENABLED:
		apba_enable();
		break;
	default:
		pr_err("%s: Invalid slave state=%d.\n", __func__, slave_state);
		break;
	}
}

static struct mods_slave_ctrl_driver apbe_ctrl_drv = {
	.slave_notify = apba_slave_notify,
};

int apba_enable(void)
{
	int ret;

	if (!g_ctrl)
		return -ENODEV;

	ret = clk_prepare_enable(g_ctrl->mclk);
	if (ret) {
		dev_err(g_ctrl->dev, "%s: failed to prepare clock.\n",
			__func__);
		return ret;
	}

	g_ctrl->desired_on = 1;

	apba_on(g_ctrl, true);

	return 0;
}

void apba_disable(void)
{
	if (!g_ctrl || !g_ctrl->desired_on)
		return;

	mods_slave_ctrl_power(g_ctrl->master_intf,
		MB_CONTROL_SLAVE_POWER_OFF, MB_CONTROL_SLAVE_MASK_APBE);
	g_ctrl->desired_on = 0;

	if (g_ctrl->mods_uart)
		mod_attach(g_ctrl->mods_uart, 0);

	clk_disable_unprepare(g_ctrl->mclk);
	apba_on(g_ctrl, false);
}

static int apba_ctrl_probe(struct platform_device *pdev)
{
	struct apba_ctrl *ctrl;
	int ret;

	/* we depend on the muc_core for transports and pinctrls */
	if (!muc_core_probed())
		return -EPROBE_DEFER;

	if (!pdev->dev.of_node) {
		/* Platform data not currently supported */
		dev_err(&pdev->dev, "%s: of devtree not found\n", __func__);
		return -EINVAL;
	}

	ctrl = devm_kzalloc(&pdev->dev, sizeof(*ctrl),
		GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;

	ctrl->dev = &pdev->dev;

	ctrl->mclk = devm_clk_get(&pdev->dev, "apba_mclk");
	if (IS_ERR(ctrl->mclk)) {
		dev_err(&pdev->dev, "%s: failed to get clock.\n", __func__);
		return PTR_ERR(ctrl->mclk);
	}

	ret = apba_gpio_setup(ctrl, &pdev->dev);
	if (ret) {
		dev_err(&pdev->dev, "failed to read gpios.\n");
		return ret;
	}

	ret = apba_int_setup(ctrl, &pdev->dev);
	if (ret)
		goto free_gpios;

	ctrl->enable_seq.len = ARRAY_SIZE(ctrl->enable_seq.val);
	ret = apba_parse_seq(&pdev->dev, "mmi,enable-seq",
		&ctrl->enable_seq);
	if (ret)
		goto disable_irq;

	ctrl->disable_seq.len = ARRAY_SIZE(ctrl->disable_seq.val);
	ret = apba_parse_seq(&pdev->dev, "mmi,disable-seq",
		&ctrl->disable_seq);
	if (ret)
		goto disable_irq;

	ctrl->wake_assert_seq.len = ARRAY_SIZE(ctrl->wake_assert_seq.val);
	ret = apba_parse_seq(&pdev->dev, "mmi,wake-assert-seq",
		&ctrl->wake_assert_seq);
	if (ret)
		goto disable_irq;

	ctrl->wake_deassert_seq.len = ARRAY_SIZE(ctrl->wake_deassert_seq.val);
	ret = apba_parse_seq(&pdev->dev, "mmi,wake-deassert-seq",
		&ctrl->wake_deassert_seq);
	if (ret)
		goto disable_irq;

	ctrl->flash_start_seq.len = ARRAY_SIZE(ctrl->flash_start_seq.val);
	ret = apba_parse_seq(&pdev->dev, "mmi,flash-start-seq",
		&ctrl->flash_start_seq);
	if (ret)
		goto disable_irq;

	ctrl->flash_end_seq.len = ARRAY_SIZE(ctrl->flash_end_seq.val);
	ret = apba_parse_seq(&pdev->dev, "mmi,flash-end-seq",
		&ctrl->flash_end_seq);
	if (ret)
		goto disable_irq;

	/* A default pinctrl state (at least) is expected */
	ctrl->pinctrl = devm_pinctrl_get(&pdev->dev);
	if (IS_ERR(ctrl->pinctrl)) {
		dev_err(&pdev->dev, "Pinctrl not defined\n");
		ret = PTR_ERR(ctrl->pinctrl);
		goto disable_irq;
	}

	ctrl->pinctrl_state_default = pinctrl_lookup_state(ctrl->pinctrl,
							   PINCTRL_STATE_DEFAULT);
	if (IS_ERR(ctrl->pinctrl_state_default)) {
		dev_err(&pdev->dev, "Pinctrl lookup failed for default\n");
		ret = PTR_ERR(ctrl->pinctrl_state_default);
		goto disable_irq;
	}

	/* The spi_active pinctrl state is optional */
	ctrl->pinctrl_state_active = pinctrl_lookup_state(ctrl->pinctrl,
							  "spi_active");
	if (IS_ERR(ctrl->pinctrl_state_active)) {
		dev_warn(&pdev->dev, "Pinctrl lookup failed for spi_active\n");
	}

	mutex_init(&ctrl->log_mutex);
	init_completion(&ctrl->comp);
	init_completion(&ctrl->baud_comp);
	init_completion(&ctrl->mode_comp);

	ret = sysfs_create_groups(&pdev->dev.kobj, apba_groups);
	if (ret) {
		dev_err(&pdev->dev, "Failed to create sysfs attr\n");
		goto disable_irq;
	}

	/* start with APBA turned OFF */
	apba_on(ctrl, false);

	g_ctrl = ctrl;

	platform_set_drvdata(pdev, ctrl);
	INIT_DELAYED_WORK(&apba_disable_work, apba_disable_work_func);

	ret = mods_register_slave_ctrl_driver(&apbe_ctrl_drv);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register slave driver\n");
		goto reset_global;
	}

	ret = request_firmware_nowait(THIS_MODULE, true, APBA_FIRMWARE_NAME,
				      g_ctrl->dev, GFP_KERNEL, g_ctrl,
				      apba_firmware_callback);
	if (ret) {
		dev_err(g_ctrl->dev, "failed to request firmware.\n");
		goto unregister_slave_ctrl;
	}

	kobject_uevent(&pdev->dev.kobj, KOBJ_ADD);

	return 0;

unregister_slave_ctrl:
	mods_unregister_slave_ctrl_driver(&apbe_ctrl_drv);
reset_global:
	sysfs_remove_groups(&pdev->dev.kobj, apba_groups);
	g_ctrl = NULL;
disable_irq:
	disable_irq_wake(ctrl->irq);
free_gpios:
	apba_gpio_free(ctrl, &pdev->dev);

	/* Let muc core finish probe even if we bombed out. */
	muc_enable_det();
	return ret;
}

static int apba_ctrl_remove(struct platform_device *pdev)
{
	struct apba_ctrl *ctrl = platform_get_drvdata(pdev);

	sysfs_remove_groups(&pdev->dev.kobj, apba_groups);

	mods_unregister_slave_ctrl_driver(&apbe_ctrl_drv);

	disable_irq_wake(ctrl->irq);
	apba_disable();
	apba_gpio_free(ctrl, &pdev->dev);

	g_ctrl = NULL;

	return 0;
}

static const struct of_device_id apba_ctrl_match[] = {
	{.compatible = "mmi,apba-ctrl",},
	{},
};

static const struct platform_device_id apba_ctrl_id_table[] = {
	{"apba_ctrl", 0},
	{},
};

static struct platform_driver apba_ctrl_driver = {
	.driver = {
		.name = "apba_ctrl",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(apba_ctrl_match),
	},
	.probe = apba_ctrl_probe,
	.remove = apba_ctrl_remove,
	.id_table = apba_ctrl_id_table,
};

int __init apba_ctrl_init(void)
{
	return platform_driver_register(&apba_ctrl_driver);
}

void __exit apba_ctrl_exit(void)
{
	platform_driver_unregister(&apba_ctrl_driver);
}
