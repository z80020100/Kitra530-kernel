/*
 * LEDs driver for GPIOs
 *
 * Copyright (C) 2007 8D Technologies inc.
 * Raphael Assenat <raph@8d.com>
 * Copyright (C) 2008 Freescale Semiconductor, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * NOTE: This driver was modified by RushUp to handle led dimming with internal
 * timer.
 */
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/hrtimer.h>

struct gpio_led_data {
	struct led_classdev cdev;
	struct gpio_desc *gpiod;
	struct work_struct work;
	u8 new_level;
	u8 can_sleep;
	u8 blinking;
	struct hrtimer timer;
	bool timer_start;
	bool pin_on;
	int (*platform_gpio_blink_set)(struct gpio_desc *desc, int state,
			unsigned long *delay_on, unsigned long *delay_off);
};

static void gpio_led_work(struct work_struct *work)
{
	struct gpio_led_data *led_dat =
		container_of(work, struct gpio_led_data, work);

	if (led_dat->blinking) {
		led_dat->platform_gpio_blink_set(led_dat->gpiod,
					led_dat->new_level, NULL, NULL);
		led_dat->blinking = 0;
	} else
		gpiod_set_value_cansleep(led_dat->gpiod, led_dat->new_level);
}

enum hrtimer_restart timer_callback( struct hrtimer *timer )
{

	u32 off_time;
	u32 on_time;
	struct gpio_led_data *gpio_data = container_of(timer,
						       struct gpio_led_data,
						       timer);

	u32 tmp_newlevel;

	tmp_newlevel = gpio_data->new_level;
	if(tmp_newlevel <= 248) //prevent flicker
		tmp_newlevel += 7;

	on_time = (1000000 * tmp_newlevel) / 255;
	off_time = 1000000 - on_time;

	if (!gpio_data->pin_on) {
		hrtimer_forward_now(&gpio_data->timer,
				    ns_to_ktime(on_time));
		gpiod_set_value(gpio_data->gpiod, 1);
		gpio_data->pin_on = true;
	} else {
		hrtimer_forward_now(&gpio_data->timer,
				    ns_to_ktime(off_time));
		gpiod_set_value(gpio_data->gpiod, 0);
		gpio_data->pin_on = false;
	}

	return HRTIMER_RESTART;
}


static void gpio_led_set(struct led_classdev *led_cdev,
	enum led_brightness value)
{
	struct gpio_led_data *led_dat =
		container_of(led_cdev, struct gpio_led_data, cdev);
	int level;

	level = value;
	led_dat->new_level = level;

	/* Setting GPIOs with I2C/etc requires a task context, and we don't
	 * seem to have a reliable way to know if we're already in one; so
	 * let's just assume the worst.
	 */
	if (led_dat->can_sleep) {
		led_dat->new_level = level;
		schedule_work(&led_dat->work);
	} else {
		if (led_dat->blinking) {
			led_dat->platform_gpio_blink_set(led_dat->gpiod, level,
							 NULL, NULL);
			led_dat->blinking = 0;
		} else {
			if((level == 255 || level == 0) && led_dat->timer_start) {
				hrtimer_cancel(&led_dat->timer);
				led_dat->timer_start = false;
			}
			if(level == 255)
				gpiod_set_value(led_dat->gpiod, 1);
			else if(level == 0)
				gpiod_set_value(led_dat->gpiod, 0);
			else if(!led_dat->timer_start) {
				hrtimer_start(&led_dat->timer, ktime_set(0,0),
					      HRTIMER_MODE_REL);
				led_dat->timer_start = true;
			}
		}
	}
}

static int gpio_blink_set(struct led_classdev *led_cdev,
	unsigned long *delay_on, unsigned long *delay_off)
{
	struct gpio_led_data *led_dat =
		container_of(led_cdev, struct gpio_led_data, cdev);

	led_dat->blinking = 1;
	return led_dat->platform_gpio_blink_set(led_dat->gpiod, GPIO_LED_BLINK,
						delay_on, delay_off);
}

static int create_gpio_led(const struct gpio_led *template,
	struct gpio_led_data *led_dat, struct device *parent,
	int (*blink_set)(struct gpio_desc *, int, unsigned long *,
			 unsigned long *))
{
	int ret, state;

	led_dat->gpiod = template->gpiod;
	if (!led_dat->gpiod) {
		/*
		 * This is the legacy code path for platform code that
		 * still uses GPIO numbers. Ultimately we would like to get
		 * rid of this block completely.
		 */
		unsigned long flags = 0;

		/* skip leds that aren't available */
		if (!gpio_is_valid(template->gpio)) {
			dev_info(parent, "Skipping unavailable LED gpio %d (%s)\n",
					template->gpio, template->name);
			return 0;
		}

		if (template->active_low)
			flags |= GPIOF_ACTIVE_LOW;

		ret = devm_gpio_request_one(parent, template->gpio, flags,
					    template->name);
		if (ret < 0)
			return ret;

		led_dat->gpiod = gpio_to_desc(template->gpio);
		if (IS_ERR(led_dat->gpiod))
			return PTR_ERR(led_dat->gpiod);
	}

	led_dat->cdev.name = template->name;
	led_dat->cdev.default_trigger = template->default_trigger;
	led_dat->can_sleep = gpiod_cansleep(led_dat->gpiod);

	led_dat->blinking = 0;
	if (blink_set) {
		led_dat->platform_gpio_blink_set = blink_set;
		led_dat->cdev.blink_set = gpio_blink_set;
	}
	led_dat->cdev.brightness_set = gpio_led_set;
	if (template->default_state == LEDS_GPIO_DEFSTATE_KEEP)
		state = !!gpiod_get_value_cansleep(led_dat->gpiod);
	else
		state = (template->default_state == LEDS_GPIO_DEFSTATE_ON);
	led_dat->cdev.brightness = state ? LED_FULL : LED_OFF;
	if (!template->retain_state_suspended)
		led_dat->cdev.flags |= LED_CORE_SUSPENDRESUME;

	hrtimer_init(&led_dat->timer,
		     CLOCK_MONOTONIC,
		     HRTIMER_MODE_REL
		     );
	led_dat->timer.function = timer_callback;
	led_dat->timer_start = false;

	ret = gpiod_direction_output(led_dat->gpiod, state);
	if (ret < 0)
		return ret;

	INIT_WORK(&led_dat->work, gpio_led_work);

	return led_classdev_register(parent, &led_dat->cdev);
}

static void delete_gpio_led(struct gpio_led_data *led)
{
	led_classdev_unregister(&led->cdev);
	cancel_work_sync(&led->work);
}

struct gpio_leds_priv {
	int num_leds;
	struct gpio_led_data leds[];
};

static inline int sizeof_gpio_leds_priv(int num_leds)
{
	return sizeof(struct gpio_leds_priv) +
		(sizeof(struct gpio_led_data) * num_leds);
}

static struct gpio_leds_priv *gpio_leds_create(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct fwnode_handle *child;
	struct gpio_leds_priv *priv;
	int count, ret;
	struct device_node *np;

	count = device_get_child_node_count(dev);
	if (!count)
		return ERR_PTR(-ENODEV);

	priv = devm_kzalloc(dev, sizeof_gpio_leds_priv(count), GFP_KERNEL);
	if (!priv)
		return ERR_PTR(-ENOMEM);

	device_for_each_child_node(dev, child) {
		struct gpio_led led = {};
		const char *state = NULL;

		led.gpiod = devm_get_gpiod_from_child(dev, NULL, child);
		if (IS_ERR(led.gpiod)) {
			fwnode_handle_put(child);
			ret = PTR_ERR(led.gpiod);
			goto err;
		}

		np = to_of_node(child);

		if (fwnode_property_present(child, "label")) {
			fwnode_property_read_string(child, "label", &led.name);
		} else {
			if (IS_ENABLED(CONFIG_OF) && !led.name && np)
				led.name = np->name;
			if (!led.name) {
				ret = -EINVAL;
				goto err;
			}
		}
		fwnode_property_read_string(child, "linux,default-trigger",
					    &led.default_trigger);

		if (!fwnode_property_read_string(child, "default-state",
						 &state)) {
			if (!strcmp(state, "keep"))
				led.default_state = LEDS_GPIO_DEFSTATE_KEEP;
			else if (!strcmp(state, "on"))
				led.default_state = LEDS_GPIO_DEFSTATE_ON;
			else
				led.default_state = LEDS_GPIO_DEFSTATE_OFF;
		}

		if (fwnode_property_present(child, "retain-state-suspended"))
			led.retain_state_suspended = 1;

		ret = create_gpio_led(&led, &priv->leds[priv->num_leds],
				      dev, NULL);
		if (ret < 0) {
			fwnode_handle_put(child);
			goto err;
		}
		priv->num_leds++;
	}

	return priv;

err:
	for (count = priv->num_leds - 1; count >= 0; count--)
		delete_gpio_led(&priv->leds[count]);
	return ERR_PTR(ret);
}

static const struct of_device_id of_gpio_leds_match[] = {
	{ .compatible = "gpio-leds", },
	{},
};

MODULE_DEVICE_TABLE(of, of_gpio_leds_match);

static int gpio_led_probe(struct platform_device *pdev)
{
	struct gpio_led_platform_data *pdata = dev_get_platdata(&pdev->dev);
	struct gpio_leds_priv *priv;
	int i, ret = 0;

	if (pdata && pdata->num_leds) {
		priv = devm_kzalloc(&pdev->dev,
				sizeof_gpio_leds_priv(pdata->num_leds),
					GFP_KERNEL);
		if (!priv)
			return -ENOMEM;
		priv->num_leds = pdata->num_leds;
		for (i = 0; i < priv->num_leds; i++) {
			ret = create_gpio_led(&pdata->leds[i],
					      &priv->leds[i],
					      &pdev->dev, pdata->gpio_blink_set);
			if (ret < 0) {
				/* On failure: unwind the led creations */
				for (i = i - 1; i >= 0; i--)
					delete_gpio_led(&priv->leds[i]);
				return ret;
			}
		}
	} else {
		priv = gpio_leds_create(pdev);
		if (IS_ERR(priv))
			return PTR_ERR(priv);
	}

	platform_set_drvdata(pdev, priv);

	return 0;
}

static int gpio_led_remove(struct platform_device *pdev)
{
	struct gpio_leds_priv *priv = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < priv->num_leds; i++)
		delete_gpio_led(&priv->leds[i]);

	return 0;
}

static void gpio_led_shutdown(struct platform_device *pdev)
{
	struct gpio_leds_priv *priv = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < priv->num_leds; i++) {
		struct gpio_led_data *led = &priv->leds[i];

		gpio_led_set(&led->cdev, LED_OFF);
	}
}

static struct platform_driver gpio_led_driver = {
	.probe		= gpio_led_probe,
	.remove		= gpio_led_remove,
	.shutdown	= gpio_led_shutdown,
	.driver		= {
		.name	= "leds-gpio",
		.of_match_table = of_gpio_leds_match,
	},
};

module_platform_driver(gpio_led_driver);

MODULE_AUTHOR("Raphael Assenat <raph@8d.com>,"
	      "Trent Piepho <tpiepho@freescale.com>,"
	      "Matteo Fumagalli <m.fumagalli@rushup.tech>");
MODULE_DESCRIPTION("GPIO LED driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:leds-gpio");
