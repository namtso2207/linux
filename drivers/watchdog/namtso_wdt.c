//#define DEBUG

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/of_platform.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/module.h>
#include <linux/device.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <linux/fs.h>
#include <linux/timer.h>
#include <linux/jiffies.h>

static struct timer_list mytimer;
static unsigned int hw_margin = 3;
static int namtso_input_pin;
static unsigned int namtso_enble = 1;

static void time_pre(struct timer_list *timer)
{
	static unsigned int flag=0;
    flag = !flag;
	gpio_direction_output(namtso_input_pin, flag);

    //printk("%s\n", __func__);
    mytimer.expires = jiffies + hw_margin * HZ/1000;  // 500ms 运行一次
	if(namtso_enble)
		mod_timer(&mytimer, mytimer.expires);
}

/*
static void wdt_exit(void)
{
    if(timer_pending(&mytimer))
    {
        del_timer(&mytimer);
    }
    printk("exit Success \n");
}*/

static ssize_t show_enble(struct class *cls,
				struct class_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", namtso_enble);
}

static ssize_t store_enble(struct class *cls, struct class_attribute *attr,
		        const char *buf, size_t count)
{
	int enable;

	if (kstrtoint(buf, 0, &enable)){
		printk("namtso_enble error\n");
		return -EINVAL;
	}
	printk("namtso_enble=%d\n",enable);
	namtso_enble = enable;
	if(namtso_enble){
		mytimer.expires = jiffies + hw_margin * HZ/1000;  // 500ms 运行一次
		mod_timer(&mytimer, mytimer.expires);
	}
	return count;
}

static ssize_t store_pin_out(struct class *cls, struct class_attribute *attr,
		        const char *buf, size_t count)
{
	int enable;

	if (kstrtoint(buf, 0, &enable)){
		printk("namtso_pin_out error\n");
		return -EINVAL;
	}
	printk("namtso_pin_out=%d\n",enable);
	gpio_direction_output(namtso_input_pin, enable);
	return count;
}

static struct class_attribute namtso_attrs[] = {
	__ATTR(enble, 0644, show_enble, store_enble),
	__ATTR(pin_out, 0644, NULL, store_pin_out),
};

static void create_namtso_attrs(void)
{
	int i;
	struct class *namtso_class;
	printk("%s\n",__func__);
	namtso_class = class_create(THIS_MODULE, "namtso");
	if (IS_ERR(namtso_class)) {
		pr_err("create namtso_class debug class fail\n");
		return;
	}
	for (i = 0; i < ARRAY_SIZE(namtso_attrs); i++) {
		if (class_create_file(namtso_class, &namtso_attrs[i]))
			pr_err("create namtso attribute %s fail\n", namtso_attrs[i].attr.name);
	}
}

static int wdt_probe(struct platform_device *pdev)
{
	const char *value;
	int ret;
	printk("hw_wdt enter probe\n");

	ret = of_property_read_u32(pdev->dev.of_node,"hw_margin_ms", &hw_margin);
	if (ret)
		return ret;

	ret = of_property_read_string(pdev->dev.of_node,
					  "hw-gpios", &value);
	if (ret) {
		printk("no hw-gpios");
		return -1;
	} else {
		namtso_input_pin = of_get_named_gpio_flags
						(pdev->dev.of_node,
						"hw-gpios",
						0, NULL);
		printk("hlm hw-gpios: %d.\n", namtso_input_pin);
		ret = gpio_request(namtso_input_pin, "namtso");
	}

    timer_setup(&mytimer, time_pre, 0);
    mytimer.expires = jiffies + hw_margin * HZ/1000; //// 500ms 运行一次
    add_timer(&mytimer);
	create_namtso_attrs();
	return 0;
}

static const struct of_device_id hw_namtso_wdt_dt_ids[] = {
	{ .compatible = "linux,wdt-namtso", },
	{ }
};
MODULE_DEVICE_TABLE(of, hw_namtso_wdt_dt_ids);

static struct platform_driver namtso_wdt_driver = {
	.driver	= {
		.name		= "hw_namtso_wdt",
		.of_match_table	= hw_namtso_wdt_dt_ids,
	},
	.probe	= wdt_probe,
};

static int __init wdt_drv_init(void)
{
	return platform_driver_register(&namtso_wdt_driver);
}
arch_initcall(wdt_drv_init);

MODULE_LICENSE("GPL");
