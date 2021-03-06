/*
 * Wacom Penabled Driver for I2C
 *
 * Copyright (c) 2011 - 2013 Tatsunosuke Tobita, Wacom.
 * <tobita.tatsunosuke@wacom.co.jp>
 *
 * This program is free software; you can redistribute it
 * and/or modify it under the terms of the GNU General
 * Public License as published by the Free Software
 * Foundation; either version of 2 of the License,
 * or (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/input.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/delay.h>
#include <asm/unaligned.h>

#include <linux/notifier.h>
#include <linux/proc_fs.h>
#include <linux/proc_ratta.h>

int wacom_touch_action_down = 0;
static RAW_NOTIFIER_HEAD(wacom_touch_event_notifier);
int register_wacom_touch_event_notifier(struct notifier_block *nb) {
    return raw_notifier_chain_register(&wacom_touch_event_notifier, nb);
}
int unregister_wacom_touch_event_notifier(struct notifier_block *nb) {
    return raw_notifier_chain_unregister(&wacom_touch_event_notifier, nb);
}
int wacom_touch_event_notifier_call_chain(unsigned long val, void *v) {
    return raw_notifier_call_chain(&wacom_touch_event_notifier, val, v);
}
EXPORT_SYMBOL(register_wacom_touch_event_notifier);
EXPORT_SYMBOL(unregister_wacom_touch_event_notifier);

//#define	ORIGIN_COORD

#ifdef ORIGIN_COORD
static int exchange_x_y_flag 	= 0;//1
static int revert_x_flag 		= 0;//1
static int revert_y_flag 		= 0;
#else
static int exchange_x_y_flag 	= 0;
static int revert_x_flag 		= 0;
static int revert_y_flag 		= 0;

#endif
int screen_max_x = 20280;
int screen_max_y = 13942;

#define WACOM_CMD_QUERY0	0x04
#define WACOM_CMD_QUERY1	0x00
#define WACOM_CMD_QUERY2	0x33
#define WACOM_CMD_QUERY3	0x02
#define WACOM_CMD_THROW0	0x05
#define WACOM_CMD_THROW1	0x00
#define WACOM_QUERY_SIZE	19

struct wacom_features {
	int x_max;
	int y_max;
	int pressure_max;
	char fw_version;
};

/*HID specific register*/
#define HID_DESC_REGISTER       1
#define COMM_REG                0x04
#define DATA_REG                0x05

typedef struct hid_descriptor {
	u16 wHIDDescLength;
	u16 bcdVersion;
	u16 wReportDescLength;
	u16 wReportDescRegister;
	u16 wInputRegister;
	u16 wMaxInputLength;
	u16 wOutputRegister;
	u16 wMaxOutputLength;
	u16 wCommandRegister;
	u16 wDataRegister;
	u16 wVendorID;
	u16 wProductID;
	u16 wVersion;
	u16 RESERVED_HIGH;
	u16 RESERVED_LOW;
} HID_DESC;

struct wacom_i2c {
	struct wacom_features *features;
	struct i2c_client *client;
	struct input_dev *input;
	u8 data[WACOM_QUERY_SIZE];
	bool prox;
	int tool;
	int reset_gpio;
	int pwren_gpio;
};
int get_hid_desc(struct i2c_client *client,
			      struct hid_descriptor *hid_desc)
{
	int ret = -1;
	char cmd[] = {HID_DESC_REGISTER, 0x00};
	struct i2c_msg msgs[] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = sizeof(cmd),
			.buf = cmd,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = sizeof(HID_DESC),
			.buf = (char *)hid_desc,
		},
	};

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	printk("******************************\n");
	printk("wacom firmware vesrsion:0x%x\n",hid_desc->wVersion);
	printk("******************************\n");

	ret = 0;
//out:
	return ret;
}


static int wacom_query_device(struct i2c_client *client,
			      struct wacom_features *features)
{
	int ret;
	u8 cmd1[] = { WACOM_CMD_QUERY0, WACOM_CMD_QUERY1,
			WACOM_CMD_QUERY2, WACOM_CMD_QUERY3 };
	u8 cmd2[] = { WACOM_CMD_THROW0, WACOM_CMD_THROW1 };
	u8 data[WACOM_QUERY_SIZE];
	struct i2c_msg msgs[] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = sizeof(cmd1),
			.buf = cmd1,
		},
		{
			.addr = client->addr,
			.flags = 0,
			.len = sizeof(cmd2),
			.buf = cmd2,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = sizeof(data),
			.buf = data,
		},
	};

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	features->x_max = get_unaligned_le16(&data[3]);
	features->y_max = get_unaligned_le16(&data[5]);
	features->pressure_max = get_unaligned_le16(&data[11]);
	features->fw_version = get_unaligned_le16(&data[13]);
	printk("Wacom source screen x_max:%d, y_max:%d, pressure:%d, fw:%d\n",
		features->x_max, features->y_max,
		features->pressure_max, features->fw_version);

	if (1 == exchange_x_y_flag) {
		swap(features->x_max, features->y_max);
	}
	screen_max_x = features->x_max;
	screen_max_y = features->y_max;
	printk("Wacom desc screen x_max:%d, y_max:%d\n", features->x_max, features->y_max);

	return 0;
}

static irqreturn_t wacom_i2c_irq(int irq, void *dev_id)
{
	struct wacom_i2c *wac_i2c = dev_id;
	struct input_dev *input = wac_i2c->input;
	//struct wacom_features *features = wac_i2c->features;
	u8 *data = wac_i2c->data;
	unsigned int x, y, pressure;
	unsigned char tsw, f1, f2, ers;
	int error;

	error = i2c_master_recv(wac_i2c->client,
				wac_i2c->data, sizeof(wac_i2c->data));
	if (error < 0)
		goto out;

	tsw = data[3] & 0x01;
	ers = data[3] & 0x04;
	f1 = data[3] & 0x02;
	f2 = data[3] & 0x10;
	x = le16_to_cpup((__le16 *)&data[4]);
	y = le16_to_cpup((__le16 *)&data[6]);
	pressure = le16_to_cpup((__le16 *)&data[8]);

	if (!wac_i2c->prox)
		wac_i2c->tool = (data[3] & 0x0c) ?
			BTN_TOOL_RUBBER : BTN_TOOL_PEN;

	wac_i2c->prox = data[3] & 0x20;

	ratta_set_raw_pen_type(data[2]);
	if (data[2] == 2)
		/* G12 */
		ratta_set_pen_type(12);
	else
		/* G14 */
		ratta_set_pen_type(14);

	if (1 == exchange_x_y_flag) {
		swap(x, y);
	}
	if (1 == revert_x_flag) {
		x = screen_max_x - x - 1;
	}
	if (1 == revert_y_flag) {
		y = screen_max_y - y - 1;
	}
	if (pressure != 0 || (wac_i2c->tool == BTN_TOOL_PEN && wac_i2c->prox == 0x1)) {
		if (wacom_touch_action_down == 0) {
			wacom_touch_action_down = 1;
			if (wacom_touch_event_notifier_call_chain(1, NULL)) {
				printk("wacom_touch_event_notifier_call_chain error\n");
			}
		}
	} else {
		if (wacom_touch_action_down == 1) {
			wacom_touch_action_down = 0;
			if (wacom_touch_event_notifier_call_chain(0, NULL)) {
				printk("wacom_touch_event_notifier_call_chain error\n");
			}
		}
	}

	input_report_key(input, BTN_TOUCH, tsw || ers);
	input_report_key(input, wac_i2c->tool, wac_i2c->prox);
	input_report_key(input, BTN_STYLUS, f1);
	input_report_key(input, BTN_STYLUS2, f2);
	input_report_abs(input, ABS_X, x);
	input_report_abs(input, ABS_Y, y);
	input_report_abs(input, ABS_PRESSURE, pressure);
	input_sync(input);

out:
	return IRQ_HANDLED;
}

static int wacom_i2c_open(struct input_dev *dev)
{
	struct wacom_i2c *wac_i2c = input_get_drvdata(dev);
	struct i2c_client *client = wac_i2c->client;

	enable_irq(client->irq);

	return 0;
}

static void wacom_i2c_close(struct input_dev *dev)
{
	struct wacom_i2c *wac_i2c = input_get_drvdata(dev);
	struct i2c_client *client = wac_i2c->client;

	disable_irq(client->irq);
}

static ssize_t wacom_fw_version_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
        struct i2c_client *client = to_i2c_client(dev);
        struct wacom_i2c *wac_i2c = i2c_get_clientdata(client);

        return sprintf(buf,"%04X.%04X\n",
		       wac_i2c->input->id.product,
		       wac_i2c->features->fw_version);
}
static DEVICE_ATTR(fw_version, S_IRUGO, wacom_fw_version_show, NULL);

static int g_irq_gpio = -1;
static int wacom_i2c_probe(struct i2c_client *client,
				     const struct i2c_device_id *id)
{
	struct wacom_i2c *wac_i2c;
	struct input_dev *input;
	struct wacom_features features = { 0 };
	HID_DESC hid_desc = { 0 };
	struct device_node *wac_np;
	int reset_gpio, irq_gpio = -1, pen_detect_gpio, pwren_gpio;
	int error;
	char buf[256];

	printk("wacom_i2c_probe\n");
	wac_np = client->dev.of_node;
	if (!wac_np) {
		dev_err(&client->dev, "get device node error\n");
		return -ENODEV;
	}

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "i2c_check_functionality error\n");
		return -EIO;
	}

	reset_gpio = of_get_named_gpio(wac_np, "gpio_rst", 0);
	if (!gpio_is_valid(reset_gpio)) {
		dev_err(&client->dev, "no gpio_rst pin available\n");
		return -EINVAL;
	}

	error = devm_gpio_request_one(&client->dev, reset_gpio,
				      GPIOF_OUT_INIT_HIGH, "gpio-rst");
	if (error < 0) {
		dev_err(&client->dev, "request reset gpio failed,%d\n", error);
		return error;
	}

	pwren_gpio = of_get_named_gpio(wac_np, "gpio_pwren", 0);
	if (!gpio_is_valid(pwren_gpio)) {
		dev_err(&client->dev, "no gpio_pwren pin available\n");
		return -EINVAL;
	}

	error = devm_gpio_request_one(&client->dev, pwren_gpio,
				      GPIOF_OUT_INIT_HIGH, "gpio-pwren");
	if (error < 0) {
		dev_err(&client->dev, "request pwren gpio failed,%d\n", error);
		return error;
	}
	msleep(100);

	gpio_direction_output(reset_gpio, 0);
	msleep(150);
	gpio_direction_output(reset_gpio, 1);
	msleep(50);

	error = wacom_query_device(client, &features);
	if (error) {
		return error;
	}

	error = get_hid_desc(client, &hid_desc);
	if (error)
		return error;

	wac_i2c = kzalloc(sizeof(*wac_i2c), GFP_KERNEL);
	input = input_allocate_device();
	if (!wac_i2c || !input) {
		error = -ENOMEM;
		goto err_free_pwren_gpio;
	}


	pen_detect_gpio = of_get_named_gpio(wac_np, "gpio_detect", 0);
	if (!gpio_is_valid(pen_detect_gpio)) {
		dev_err(&client->dev, "no pen_detect_gpio pin available\n");
		goto err_free_mem;
	}
	error = devm_gpio_request_one(&client->dev, pen_detect_gpio, GPIOF_IN, "gpio_detect");
	if (error < 0) {
		dev_err(&client->dev, "request gpio detect failed,%d\n", error);
		goto err_free_mem;
	}

	irq_gpio = of_get_named_gpio(wac_np, "gpio_intr", 0);
	if (!gpio_is_valid(irq_gpio)) {
		dev_err(&client->dev, "no gpio_intr pin available\n");
		goto err_free_pen_detect_gpio;
	}
	g_irq_gpio = irq_gpio;
#if 1
	error = devm_gpio_request_one(&client->dev, irq_gpio, GPIOF_IN, "gpio_intr");
	if (error < 0) {
		dev_err(&client->dev, "request intr gpio failed,%d\n", error);
		goto err_free_pen_detect_gpio;
	}
#endif
	client->irq = gpio_to_irq(irq_gpio);
	//printk("wacom_i2c_probe irq=%d, irq_gpio=%d\n",client->irq, irq_gpio);
	if (client->irq < 0) {
		dev_err(&client->dev, "Unable to get irq number for GPIO %d, error %d\n", irq_gpio, client->irq);
		goto err_free_irq_gpio;
	}
	wac_i2c->pwren_gpio = pwren_gpio;
	wac_i2c->reset_gpio = reset_gpio;
	wac_i2c->features = &features;
	wac_i2c->client = client;
	wac_i2c->input = input;

	input->name = "Wacom I2C Digitizer";
	input->id.bustype = BUS_I2C;
	input->id.vendor = 0x56a;
	//input->id.version = features.fw_version;
	input->id.version = hid_desc.wVersion;
	input->hint_events_per_packet = 8192;

	input->dev.parent = &client->dev;
	input->open = wacom_i2c_open;
	input->close = wacom_i2c_close;

	input->evbit[0] |= BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);

	__set_bit(BTN_TOOL_PEN, input->keybit);
	__set_bit(BTN_TOOL_RUBBER, input->keybit);
	__set_bit(BTN_STYLUS, input->keybit);
	__set_bit(BTN_STYLUS2, input->keybit);
	__set_bit(BTN_TOUCH, input->keybit);
	__set_bit(INPUT_PROP_DIRECT, input->propbit);

	input_set_abs_params(input, ABS_X, 0, features.x_max, 0, 0);
	input_set_abs_params(input, ABS_Y, 0, features.y_max, 0, 0);
	input_set_abs_params(input, ABS_PRESSURE, 0, features.pressure_max, 0, 0);

	input_set_drvdata(input, wac_i2c);

	error = request_threaded_irq(client->irq, NULL, wacom_i2c_irq,
				     IRQF_TRIGGER_LOW | IRQF_ONESHOT,
				     "wacom", wac_i2c);
	if (error) {
		dev_err(&client->dev,
			"Failed to enable IRQ, error: %d\n", error);
		goto err_free_irq_gpio;
	}

	/* Disable the IRQ, we'll enable it in wac_i2c_open() */
	disable_irq(client->irq);

	error = input_register_device(wac_i2c->input);
	if (error) {
		dev_err(&client->dev,
			"Failed to register input device, error: %d\n", error);
		goto err_free_irq;
	}

	i2c_set_clientdata(client, wac_i2c);
	error = device_create_file(&client->dev, &dev_attr_fw_version);
	if (error) {
		dev_err(&client->dev, "create fw_version failed,%d\n", error);
		goto err_unregister_input;
	}

	memset(buf, 0, sizeof(buf));
	snprintf(buf, sizeof(buf) - 1,
		 "/sys/bus/i2c/devices/%s/fw_version",
		 kobject_name(&client->dev.kobj));
	proc_symlink("ratta/emr_fw_version", NULL, buf);

	return 0;

err_unregister_input:
	input_unregister_device(wac_i2c->input);
err_free_irq:
	free_irq(client->irq, wac_i2c);
err_free_irq_gpio:
	devm_gpio_free(&client->dev, irq_gpio);
err_free_pen_detect_gpio:
	devm_gpio_free(&client->dev, pen_detect_gpio);
err_free_mem:
	input_free_device(input);
	kfree(wac_i2c);
err_free_pwren_gpio:
	devm_gpio_free(&client->dev, pwren_gpio);
//err_free_reset_gpio:
	devm_gpio_free(&client->dev, reset_gpio);

	return error;
}

static int wacom_i2c_remove(struct i2c_client *client)
{
	struct wacom_i2c *wac_i2c = i2c_get_clientdata(client);

	device_remove_file(&client->dev, &dev_attr_fw_version);
	free_irq(client->irq, wac_i2c);
	input_unregister_device(wac_i2c->input);
	kfree(wac_i2c);

	return 0;
}

static int __maybe_unused wacom_i2c_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct wacom_i2c *wac_i2c = i2c_get_clientdata(client);

	disable_irq(client->irq);
	gpio_set_value(wac_i2c->reset_gpio, 0);
	gpio_set_value(wac_i2c->pwren_gpio, 0);
	return 0;
}

static int __maybe_unused wacom_i2c_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct wacom_i2c *wac_i2c = i2c_get_clientdata(client);

	gpio_set_value(wac_i2c->pwren_gpio, 1);
	msleep(100);
	gpio_set_value(wac_i2c->reset_gpio, 0);
	msleep(150);
	gpio_set_value(wac_i2c->reset_gpio, 1);
	msleep(50);
	enable_irq(client->irq);

	return 0;
}

static SIMPLE_DEV_PM_OPS(wacom_i2c_pm, wacom_i2c_suspend, wacom_i2c_resume);

static const struct i2c_device_id wacom_i2c_id[] = {
	{ "wacom", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, wacom_i2c_id);

static const struct of_device_id wacom_dt_ids[] = {
	{
		.compatible = "wacom,w9013",
		.data = (void *) &wacom_i2c_id[0],
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, wacom_dt_ids);

static struct i2c_driver wacom_i2c_driver = {
	.driver	= {
		.name	= "wacom",
		.owner	= THIS_MODULE,
		.of_match_table = wacom_dt_ids,
		.pm	= &wacom_i2c_pm,
	},

	.probe		= wacom_i2c_probe,
	.remove		= wacom_i2c_remove,
	.id_table	= wacom_i2c_id,
};

static int __init wacom_init(void)
{
	return i2c_add_driver(&wacom_i2c_driver);
}

static void __exit wacom_exit(void)
{
	i2c_del_driver(&wacom_i2c_driver);
}

/*
 * Module entry points
 */
subsys_initcall(wacom_init);
//late_initcall(wacom_init);
module_exit(wacom_exit);

//module_i2c_driver(wacom_i2c_driver);

MODULE_AUTHOR("Tatsunosuke Tobita <tobita.tatsunosuke@wacom.co.jp>");
MODULE_DESCRIPTION("WACOM EMR I2C Driver");
MODULE_LICENSE("GPL");
