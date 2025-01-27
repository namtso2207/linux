// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Rockchip RK805 PMIC Power Key driver
 *
 * Copyright (c) 2017, Fuzhou Rockchip Electronics Co., Ltd
 *
 * Author: Joseph Chen <chenjh@rock-chips.com>
 */

#include <linux/errno.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

static struct input_dev *sinput_dev;
void rk_send_power_key(int state)
{
	if (!sinput_dev)
		return;
	if (state) {
		input_report_key(sinput_dev, KEY_POWER, 1);
		input_sync(sinput_dev);
	} else {
		input_report_key(sinput_dev, KEY_POWER, 0);
		input_sync(sinput_dev);
	}
}
EXPORT_SYMBOL(rk_send_power_key);

void rk_send_wakeup_key(void)
{
	if (!sinput_dev)
		return;

	input_report_key(sinput_dev, KEY_WAKEUP, 1);
	input_sync(sinput_dev);
	input_report_key(sinput_dev, KEY_WAKEUP, 0);
	input_sync(sinput_dev);
}
EXPORT_SYMBOL(rk_send_wakeup_key);

extern int key_test_flag;
static irqreturn_t pwrkey_fall_irq(int irq, void *_pwr)
{
	struct input_dev *pwr = _pwr;
	if(key_test_flag)
		input_report_key(pwr, KEY_VOLUMEUP, 1);
	else
		input_report_key(pwr, KEY_POWER, 1);
	input_sync(pwr);

	return IRQ_HANDLED;
}

static irqreturn_t pwrkey_rise_irq(int irq, void *_pwr)
{
	struct input_dev *pwr = _pwr;

	if(key_test_flag)
		input_report_key(pwr, KEY_VOLUMEUP, 0);
	else
		input_report_key(pwr, KEY_POWER, 0);
	input_sync(pwr);

	return IRQ_HANDLED;
}

static int rk805_pwrkey_probe(struct platform_device *pdev)
{
	struct input_dev *pwr;
	int fall_irq, rise_irq;
	struct device_node *np;
	int err;

	np = of_get_child_by_name(pdev->dev.parent->of_node, "pwrkey");
	if (np && !of_device_is_available(np)) {
		dev_info(&pdev->dev, "device is disabled\n");
		return -EINVAL;
	}

	pwr = devm_input_allocate_device(&pdev->dev);
	if (!pwr) {
		dev_err(&pdev->dev, "Can't allocate power button\n");
		return -ENOMEM;
	}

	pwr->name = "rk805 pwrkey";
	pwr->phys = "rk805_pwrkey/input0";
	pwr->id.bustype = BUS_HOST;
	input_set_capability(pwr, EV_KEY, KEY_POWER);
	input_set_capability(pwr, EV_KEY, KEY_VOLUMEUP);

	fall_irq = platform_get_irq(pdev, 0);
	if (fall_irq < 0)
		return fall_irq;

	rise_irq = platform_get_irq(pdev, 1);
	if (rise_irq < 0)
		return rise_irq;

	err = devm_request_any_context_irq(&pwr->dev, fall_irq,
					   pwrkey_fall_irq,
					   IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					   "rk805_pwrkey_fall", pwr);
	if (err < 0) {
		dev_err(&pdev->dev, "Can't register fall irq: %d\n", err);
		return err;
	}

	err = devm_request_any_context_irq(&pwr->dev, rise_irq,
					   pwrkey_rise_irq,
					   IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					   "rk805_pwrkey_rise", pwr);
	if (err < 0) {
		dev_err(&pdev->dev, "Can't register rise irq: %d\n", err);
		return err;
	}

	err = input_register_device(pwr);
	if (err) {
		dev_err(&pdev->dev, "Can't register power button: %d\n", err);
		return err;
	}

	sinput_dev = pwr;
	platform_set_drvdata(pdev, pwr);
	device_init_wakeup(&pdev->dev, true);

	return 0;
}

static struct platform_driver rk805_pwrkey_driver = {
	.probe	= rk805_pwrkey_probe,
	.driver	= {
		.name = "rk805-pwrkey",
	},
};
module_platform_driver(rk805_pwrkey_driver);

MODULE_ALIAS("platform:rk805-pwrkey");
MODULE_AUTHOR("Joseph Chen <chenjh@rock-chips.com>");
MODULE_DESCRIPTION("RK805 PMIC Power Key driver");
MODULE_LICENSE("GPL");
