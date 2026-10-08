// SPDX-License-Identifier: GPL-2.0
/* GPIO/PWM motor and optional LED Bar consumer, with bounded ON leases. */
#include <linux/compat.h>
#include <linux/gpio/consumer.h>
#include <linux/jiffies.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/pwm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include "smartfan_uapi.h"

struct smartfan
{
	struct miscdevice misc;
	struct mutex lock;
	struct kref refs;
	struct delayed_work expiry_work;
	struct gpio_desc *enable;
	struct gpio_desc *in1;
	struct gpio_desc *in2;
	struct pwm_device *pwm;
	struct gpio_descs *leds;
	u32 led_count;
	u32 led_mode;
	u32 leds_configured;
	unsigned long lease_deadline;
	unsigned long on_deadline;
	unsigned long boost_deadline;
	u32 lease_ms;
	u32 max_on_ms;
	u32 period_ns;
	u32 boost_ms;
	u32 level;
	u32 last_nonzero_level;
	u32 duty_percent;
	u32 stop_reason;
	bool boosting;
	bool running;
	bool owner;
	bool removed;
	bool suspended;
};

static void smartfan_free(struct kref *refs)
{
	kfree(container_of(refs, struct smartfan, refs));
}

static u32 smartfan_remaining_ms(unsigned long deadline, unsigned long now)
{
	return time_after(deadline, now) ? jiffies_to_msecs(deadline - now) : 0;
}

static int smartfan_pwm_apply(struct smartfan *fan, u32 duty_percent, bool enabled)
{
	struct pwm_state state = {
		.period = fan->period_ns,
		.duty_cycle = (u64)fan->period_ns * duty_percent / 100,
		.polarity = PWM_POLARITY_NORMAL,
		.enabled = enabled,
	};

	return pwm_apply_state(fan->pwm, &state);
}

/* Caller holds lock (or probe has not published the device yet). */
static void smartfan_leds_locked(struct smartfan *fan, u32 count)
{
	u32 i;

	if (!fan->leds)
		return;
	for (i = 0; i < fan->leds_configured; ++i)
		gpiod_set_value_cansleep(fan->leds->desc[i], i < count);
	fan->led_count = count;
}

static void smartfan_leds_auto_locked(struct smartfan *fan)
{
	fan->led_mode = SMARTFAN_LED_AUTO;
	smartfan_leds_locked(fan, fan->running ? smartfan_level_leds(fan->level) : 0);
}

/* Caller holds lock; resources must still belong to this platform device. */
static int smartfan_stop_locked(struct smartfan *fan, u32 reason)
{
	int ret = 0;

	/* Gate off first; disabled PWM alone does not guarantee its output level. */
	gpiod_set_value_cansleep(fan->enable, 0);
	if (fan->in1)
		gpiod_set_value_cansleep(fan->in1, 0);
	gpiod_set_value_cansleep(fan->in2, 0);
	fan->running = false;
	fan->boosting = false;
	fan->duty_percent = 0;
	fan->stop_reason = reason;
	smartfan_leds_auto_locked(fan);
	if (fan->pwm)
	{
		ret = smartfan_pwm_apply(fan, 0, false);
		if (ret)
		{
			fan->stop_reason = SMARTFAN_STOP_PWM_ERROR;
			dev_err(fan->misc.parent, "PWM disable failed: %d; EN is OFF\n", ret);
		}
	}
	return ret;
}

static int smartfan_set_duty_locked(struct smartfan *fan, u32 duty_percent)
{
	int ret;

	ret = smartfan_pwm_apply(fan, duty_percent, true);
	if (ret)
	{
		dev_err(fan->misc.parent, "PWM apply failed: %d; stopping\n", ret);
		smartfan_stop_locked(fan, SMARTFAN_STOP_PWM_ERROR);
		return ret;
	}
	fan->duty_percent = duty_percent;
	return 0;
}

static int smartfan_expire_locked(struct smartfan *fan, unsigned long now)
{
	int ret;

	if (!fan->running)
		return 0;
	if (time_after_eq(now, fan->on_deadline))
		return smartfan_stop_locked(fan, SMARTFAN_STOP_MAX_ON);
	else if (time_after_eq(now, fan->lease_deadline))
		return smartfan_stop_locked(fan, SMARTFAN_STOP_LEASE);
	else if (fan->boosting && time_after_eq(now, fan->boost_deadline))
	{
		fan->boosting = false;
		ret = smartfan_set_duty_locked(fan, smartfan_level_duty(fan->level));
		if (ret)
			return ret;
		/* PWM apply may sleep: do not renew a lease that expired meanwhile. */
		now = jiffies;
		if (time_after_eq(now, fan->on_deadline))
			return smartfan_stop_locked(fan, SMARTFAN_STOP_MAX_ON);
		if (time_after_eq(now, fan->lease_deadline))
			return smartfan_stop_locked(fan, SMARTFAN_STOP_LEASE);
	}
	return 0;
}

static void smartfan_schedule_locked(struct smartfan *fan)
{
	unsigned long deadline;
	unsigned long now = jiffies;

	deadline = time_before(fan->lease_deadline, fan->on_deadline) ? fan->lease_deadline : fan->on_deadline;
	if (fan->boosting && time_before(fan->boost_deadline, deadline))
		deadline = fan->boost_deadline;
	mod_delayed_work(system_wq, &fan->expiry_work,
					 time_after(deadline, now) ? deadline - now : 0);
}

static void smartfan_expiry_work(struct work_struct *work)
{
	struct smartfan *fan = container_of(to_delayed_work(work),
										struct smartfan, expiry_work);
	unsigned long now;

	mutex_lock(&fan->lock);
	if (!fan->removed && !fan->suspended && fan->running)
	{
		now = jiffies;
		smartfan_expire_locked(fan, now);
		if (fan->running)
			smartfan_schedule_locked(fan);
	}
	mutex_unlock(&fan->lock);
}

static int smartfan_open(struct inode *inode, struct file *file)
{
	struct smartfan *fan = container_of(file->private_data,
										struct smartfan, misc);
	int ret = 0;

	mutex_lock(&fan->lock);
	if (fan->removed)
		ret = -ENODEV;
	else if (fan->suspended)
		ret = -EHOSTDOWN;
	else if (fan->owner)
		ret = -EBUSY;
	else
	{
		kref_get(&fan->refs);
		fan->owner = true;
		file->private_data = fan;
	}
	mutex_unlock(&fan->lock);
	return ret;
}

static int smartfan_release(struct inode *inode, struct file *file)
{
	struct smartfan *fan = file->private_data;

	mutex_lock(&fan->lock);
	if (!fan->removed && !fan->suspended)
		smartfan_stop_locked(fan, SMARTFAN_STOP_CLOSE);
	fan->owner = false;
	mutex_unlock(&fan->lock);
	/* A new owner may already have armed work: do not cancel it here. */
	kref_put(&fan->refs, smartfan_free);
	return 0;
}

static long smartfan_ioctl(struct file *file, unsigned int cmd,
						   unsigned long arg)
{
	struct smartfan *fan = file->private_data;
	struct smartfan_request request;
	struct smartfan_speed_request speed_request;
	struct smartfan_status status = {0};
	struct smartfan_speed_status speed_status = {0};
	struct smartfan_led_request led_request;
	struct smartfan_led_status led_status = {0};
	unsigned long now;
	long ret = 0;

	switch (cmd)
	{
	case SMARTFAN_IOC_SET:
		if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
			return -EFAULT;
		if (request.abi_version != SMARTFAN_ABI_VERSION ||
			request.enabled > 1 || request.reserved[0] || request.reserved[1])
			return -EINVAL;
		break;
	case SMARTFAN_IOC_HEARTBEAT:
		if (arg)
			return -EINVAL;
		break;
	case SMARTFAN_IOC_SET_SPEED:
		if (copy_from_user(&speed_request, (void __user *)arg, sizeof(speed_request)))
			return -EFAULT;
		if (speed_request.abi_version != SMARTFAN_ABI_VERSION ||
			speed_request.level > SMARTFAN_MAX_SPEED_LEVEL ||
			speed_request.reserved[0] || speed_request.reserved[1])
			return -EINVAL;
		break;
	case SMARTFAN_IOC_GET:
	case SMARTFAN_IOC_GET_SPEED:
	case SMARTFAN_IOC_GET_LEDS:
		break;
	case SMARTFAN_IOC_SET_LEDS:
		if (copy_from_user(&led_request, (void __user *)arg, sizeof(led_request)))
			return -EFAULT;
		if (led_request.abi_version != SMARTFAN_ABI_VERSION ||
		    led_request.mode > SMARTFAN_LED_TEST ||
		    led_request.count > SMARTFAN_LED_SEGMENTS || led_request.reserved ||
		    (led_request.mode == SMARTFAN_LED_AUTO && led_request.count))
			return -EINVAL;
		break;
	default:
		return -ENOTTY;
	}

	mutex_lock(&fan->lock);
	if (fan->removed)
	{
		ret = -ENODEV;
		goto out;
	}
	if (fan->suspended)
	{
		ret = -EHOSTDOWN;
		goto out;
	}
	/* GPIO fallback cannot represent intermediate speeds; reject before writes. */
	if (cmd == SMARTFAN_IOC_SET_SPEED && !fan->pwm &&
		speed_request.level && speed_request.level != SMARTFAN_MAX_SPEED_LEVEL)
	{
		ret = -EOPNOTSUPP;
		goto out;
	}
	now = jiffies;
	/* Check expiry here as well: delayed work scheduling is not a deadline. */
	ret = smartfan_expire_locked(fan, now);
	if (ret && cmd != SMARTFAN_IOC_GET && cmd != SMARTFAN_IOC_GET_SPEED &&
	    cmd != SMARTFAN_IOC_GET_LEDS)
		goto out;
	/* GET must remain available to report a stopped PWM error state. */
	ret = 0;
	now = jiffies;
	switch (cmd)
	{
	case SMARTFAN_IOC_SET:
		if (!request.enabled)
		{
			ret = smartfan_stop_locked(fan, SMARTFAN_STOP_USER);
			break;
		}
		if (!fan->running)
		{
			/* Only an explicit ON command may start a new run. */
			if (!fan->level)
				fan->level = fan->last_nonzero_level;
			gpiod_set_value_cansleep(fan->in2, 0);
			fan->on_deadline = now + msecs_to_jiffies(fan->max_on_ms);
			fan->lease_deadline = now + msecs_to_jiffies(fan->lease_ms);
			if (fan->pwm)
			{
				fan->boosting = fan->boost_ms &&
								fan->level < SMARTFAN_MAX_SPEED_LEVEL;
				if (fan->boosting)
					fan->boost_deadline = now + msecs_to_jiffies(fan->boost_ms);
				ret = smartfan_set_duty_locked(fan, fan->boosting ? 100 : smartfan_level_duty(fan->level));
				if (ret)
					break;
			}
			else
			{
				gpiod_set_value_cansleep(fan->in1, 1);
				fan->duty_percent = 100;
			}
			/* Sleeping configuration must not enable an already expired run. */
			if (time_after_eq(jiffies, fan->on_deadline) ||
			    time_after_eq(jiffies, fan->lease_deadline)) {
				u32 reason = time_after_eq(jiffies, fan->on_deadline) ?
					SMARTFAN_STOP_MAX_ON : SMARTFAN_STOP_LEASE;

				ret = smartfan_stop_locked(fan, reason);
				if (!ret)
					ret = -ETIMEDOUT;
				break;
			}
			gpiod_set_value_cansleep(fan->enable, 1);
			fan->running = true;
		}
		/* Repeat ON renews the lease, but never extends on_deadline. */
		fan->lease_deadline = now + msecs_to_jiffies(fan->lease_ms);
		smartfan_leds_auto_locked(fan);
		smartfan_schedule_locked(fan);
		break;
	case SMARTFAN_IOC_HEARTBEAT:
		if (!fan->running)
		{
			if (fan->stop_reason == SMARTFAN_STOP_PWM_ERROR)
			{
				ret = -EIO;
				break;
			}
			ret = fan->stop_reason == SMARTFAN_STOP_LEASE ||
						  fan->stop_reason == SMARTFAN_STOP_MAX_ON
					  ? -ETIMEDOUT
					  : -EPIPE;
			break;
		}
		fan->lease_deadline = now + msecs_to_jiffies(fan->lease_ms);
		smartfan_schedule_locked(fan);
		break;
	case SMARTFAN_IOC_SET_SPEED:
		fan->level = speed_request.level;
		if (!fan->level)
		{
			ret = smartfan_stop_locked(fan, SMARTFAN_STOP_USER);
			break;
		}
		fan->last_nonzero_level = fan->level;
		/* Selecting speed after OFF/timeout does not start or renew a run. */
		if (!fan->running)
		{
			smartfan_leds_auto_locked(fan);
			break;
		}
		if (fan->pwm && !fan->boosting)
		{
			ret = smartfan_set_duty_locked(fan, smartfan_level_duty(fan->level));
			if (ret)
				break;
		}
		ret = smartfan_expire_locked(fan, jiffies);
		if (ret || !fan->running) {
			if (!ret)
				ret = -ETIMEDOUT;
			break;
		}
		fan->lease_deadline = now + msecs_to_jiffies(fan->lease_ms);
		smartfan_leds_auto_locked(fan);
		smartfan_schedule_locked(fan);
		break;
	case SMARTFAN_IOC_GET:
		status.abi_version = SMARTFAN_ABI_VERSION;
		status.running = fan->running;
		status.stop_reason = fan->stop_reason;
		status.lease_ms = fan->lease_ms;
		status.max_on_ms = fan->max_on_ms;
		if (fan->running)
		{
			status.lease_remaining_ms =
				smartfan_remaining_ms(fan->lease_deadline, now);
			status.on_remaining_ms =
				smartfan_remaining_ms(fan->on_deadline, now);
		}
		break;
	case SMARTFAN_IOC_GET_SPEED:
		speed_status.abi_version = SMARTFAN_ABI_VERSION;
		speed_status.level = fan->level;
		speed_status.max_level = SMARTFAN_MAX_SPEED_LEVEL;
		speed_status.duty_percent = fan->duty_percent;
		speed_status.capabilities = (fan->pwm ? SMARTFAN_CAP_PWM : 0) |
			(fan->leds ? SMARTFAN_CAP_LED_BAR : 0);
		speed_status.period_ns = fan->period_ns;
		if (fan->boosting)
			speed_status.boost_remaining_ms =
				smartfan_remaining_ms(fan->boost_deadline, now);
		break;
	case SMARTFAN_IOC_SET_LEDS:
		if (!fan->leds)
		{
			ret = -EOPNOTSUPP;
			break;
		}
		if (led_request.mode == SMARTFAN_LED_TEST && fan->running)
		{
			ret = -EBUSY;
			break;
		}
		if (led_request.mode == SMARTFAN_LED_AUTO)
			smartfan_leds_auto_locked(fan);
		else
		{
			fan->led_mode = SMARTFAN_LED_TEST;
			smartfan_leds_locked(fan, led_request.count);
		}
		break;
	case SMARTFAN_IOC_GET_LEDS:
		led_status.abi_version = SMARTFAN_ABI_VERSION;
		led_status.mode = fan->led_mode;
		led_status.count = fan->led_count;
		led_status.available = !!fan->leds;
		break;
	}
out:
	mutex_unlock(&fan->lock);
	if (!ret && cmd == SMARTFAN_IOC_GET &&
		copy_to_user((void __user *)arg, &status, sizeof(status)))
		ret = -EFAULT;
	if (!ret && cmd == SMARTFAN_IOC_GET_SPEED &&
		copy_to_user((void __user *)arg, &speed_status, sizeof(speed_status)))
		ret = -EFAULT;
	if (!ret && cmd == SMARTFAN_IOC_GET_LEDS &&
		copy_to_user((void __user *)arg, &led_status, sizeof(led_status)))
		ret = -EFAULT;
	return ret;
}

static const struct file_operations smartfan_fops = {
	.owner = THIS_MODULE,
	.open = smartfan_open,
	.release = smartfan_release,
	.unlocked_ioctl = smartfan_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = compat_ptr_ioctl,
#endif
	.llseek = no_llseek,
};

static int smartfan_get_output(struct device *dev, const char *name,
							   struct gpio_desc **desc)
{
	struct gpio_desc *gpio;
	int ret;

	/* Validate polarity before changing any physical output level. */
	gpio = devm_gpiod_get(dev, name, GPIOD_ASIS);
	if (IS_ERR(gpio))
		return dev_err_probe(dev, PTR_ERR(gpio), "%s GPIO unavailable\n", name);
	if (gpiod_is_active_low(gpio))
		return dev_err_probe(dev, -EINVAL, "%s GPIO must be active high\n", name);
	ret = gpiod_direction_output(gpio, 0);
	if (ret)
		return dev_err_probe(dev, ret, "%s GPIO output setup failed\n", name);
	*desc = gpio;
	return 0;
}

static int smartfan_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct smartfan *fan;
	struct pwm_args pwm_args;
	bool pwm_mode = device_property_present(dev, "pwms");
	int ret;

	BUILD_BUG_ON(sizeof(struct smartfan_request) != 16);
	BUILD_BUG_ON(sizeof(struct smartfan_status) != 32);
	BUILD_BUG_ON(sizeof(struct smartfan_speed_request) != 16);
	BUILD_BUG_ON(sizeof(struct smartfan_speed_status) != 32);
	BUILD_BUG_ON(sizeof(struct smartfan_led_request) != 16);
	BUILD_BUG_ON(sizeof(struct smartfan_led_status) != 16);
	fan = kzalloc(sizeof(*fan), GFP_KERNEL);
	if (!fan)
		return -ENOMEM;
	kref_init(&fan->refs);
	mutex_init(&fan->lock);
	INIT_DELAYED_WORK(&fan->expiry_work, smartfan_expiry_work);
	fan->lease_ms = SMARTFAN_DEFAULT_LEASE_MS;
	fan->max_on_ms = SMARTFAN_DEFAULT_MAX_ON_MS;
	fan->level = SMARTFAN_MAX_SPEED_LEVEL;
	fan->last_nonzero_level = SMARTFAN_MAX_SPEED_LEVEL;
	fan->misc.parent = dev;
	fan->stop_reason = SMARTFAN_STOP_INITIAL;
	if (pwm_mode)
	{
		fan->boost_ms = SMARTFAN_DEFAULT_BOOST_MS;
		if (device_property_present(dev, "startup-boost-ms"))
		{
			ret = device_property_read_u32(dev, "startup-boost-ms", &fan->boost_ms);
			if (ret)
				goto fail;
		}
		if (fan->boost_ms > 1000)
		{
			ret = dev_err_probe(dev, -EINVAL, "startup boost must be 0..1000 ms\n");
			goto fail;
		}
	}
	if (device_property_present(dev, "lease-timeout-ms"))
	{
		ret = device_property_read_u32(dev, "lease-timeout-ms", &fan->lease_ms);
		if (ret)
			goto fail;
	}
	if (device_property_present(dev, "max-on-ms"))
	{
		ret = device_property_read_u32(dev, "max-on-ms", &fan->max_on_ms);
		if (ret)
			goto fail;
	}
	if (fan->lease_ms < 500 || fan->lease_ms > 10000 ||
		fan->max_on_ms < 1000 || fan->max_on_ms > 120000 ||
		fan->lease_ms > fan->max_on_ms)
	{
		ret = dev_err_probe(dev, -EINVAL, "invalid lease/max-on timing\n");
		goto fail;
	}
	/* Acquire and deassert EN before either direction input is configured. */
	ret = smartfan_get_output(dev, "enable", &fan->enable);
	if (ret)
		goto fail;
	/* IN1 is owned solely by the PWM provider when pwms is present. */
	if (!pwm_mode)
	{
		ret = smartfan_get_output(dev, "in1", &fan->in1);
		if (ret)
			goto fail;
	}
	ret = smartfan_get_output(dev, "in2", &fan->in2);
	if (ret)
		goto fail;
	if (pwm_mode)
	{
		fan->pwm = devm_pwm_get(dev, NULL);
		if (IS_ERR(fan->pwm))
		{
			ret = dev_err_probe(dev, PTR_ERR(fan->pwm), "PWM unavailable\n");
			fan->pwm = NULL;
			goto fail;
		}
		pwm_get_args(fan->pwm, &pwm_args);
		/* Bound period before u64->u32 conversion/provider int duty arithmetic. */
		if ((pwm_args.period && (pwm_args.period < 1000000 ||
								 pwm_args.period > 4000000)) ||
			pwm_args.polarity != PWM_POLARITY_NORMAL)
		{
			ret = dev_err_probe(dev, -EINVAL,
								"PWM requires normal polarity and period 1000000..4000000 ns\n");
			/* Do not apply a state using invalid arguments during cleanup. */
			fan->pwm = NULL;
			goto fail;
		}
		fan->period_ns = pwm_args.period ? pwm_args.period : SMARTFAN_DEFAULT_PWM_PERIOD_NS;
		ret = smartfan_pwm_apply(fan, 0, false);
		if (ret)
		{
			dev_err_probe(dev, ret, "initial PWM disable failed; EN is OFF\n");
			goto fail;
		}
	}
	fan->leds = devm_gpiod_get_array_optional(dev, "led", GPIOD_ASIS);
	if (IS_ERR(fan->leds))
	{
		ret = dev_err_probe(dev, PTR_ERR(fan->leds), "LED GPIOs unavailable\n");
		fan->leds = NULL;
		goto fail;
	}
	if (fan->leds)
	{
		u32 i;

		if (fan->leds->ndescs != SMARTFAN_LED_SEGMENTS)
		{
			ret = dev_err_probe(dev, -EINVAL, "LED Bar requires exactly 8 GPIOs\n");
			goto fail;
		}
		for (i = 0; i < fan->leds->ndescs; ++i)
		{
			if (gpiod_is_active_low(fan->leds->desc[i]))
			{
				ret = dev_err_probe(dev, -EINVAL, "LED GPIOs must be active high\n");
				goto fail;
			}
		}
		for (i = 0; i < fan->leds->ndescs; ++i)
		{
			ret = gpiod_direction_output(fan->leds->desc[i], 0);
			if (ret)
				goto fail;
			++fan->leds_configured;
		}
	}
	fan->misc.minor = MISC_DYNAMIC_MINOR;
	fan->misc.name = "smartfan";
	fan->misc.fops = &smartfan_fops;
	fan->misc.parent = dev;
	fan->misc.mode = 0600;
	platform_set_drvdata(pdev, fan);
	ret = misc_register(&fan->misc);
	if (ret)
		goto fail;
	dev_info(dev, "OFF; %s mode, lease=%u ms, max-on=%u ms, PWM period=%u ns\n",
			 pwm_mode ? "PWM" : "GPIO", fan->lease_ms, fan->max_on_ms,
			 fan->period_ns);
	return 0;
fail:
	smartfan_leds_locked(fan, 0);
	/* Only successfully configured outputs are safe to access. */
	if (fan->enable)
		gpiod_set_value_cansleep(fan->enable, 0);
	if (fan->pwm)
	{
		int stop_ret = smartfan_pwm_apply(fan, 0, false);

		if (stop_ret)
			dev_err(dev, "probe cleanup PWM disable failed: %d; EN is OFF\n", stop_ret);
	}
	if (fan->in1)
		gpiod_set_value_cansleep(fan->in1, 0);
	if (fan->in2)
		gpiod_set_value_cansleep(fan->in2, 0);
	platform_set_drvdata(pdev, NULL);
	kref_put(&fan->refs, smartfan_free);
	return ret;
}

static void smartfan_quiesce(struct smartfan *fan)
{
	mutex_lock(&fan->lock);
	/* No fd or worker may use devm GPIOs after this point. */
	if (!fan->removed)
	{
		fan->removed = true;
		smartfan_stop_locked(fan, SMARTFAN_STOP_REMOVE);
	}
	mutex_unlock(&fan->lock);
}

static void smartfan_remove(struct platform_device *pdev)
{
	struct smartfan *fan = platform_get_drvdata(pdev);

	smartfan_quiesce(fan);
	/* misc_open takes misc_mtx before our mutex: keep deregister unlocked. */
	misc_deregister(&fan->misc);
	cancel_delayed_work_sync(&fan->expiry_work);
	platform_set_drvdata(pdev, NULL);
	/* Existing fds retain the state, but can no longer touch GPIO resources. */
	kref_put(&fan->refs, smartfan_free);
}

static void smartfan_shutdown(struct platform_device *pdev)
{
	struct smartfan *fan = platform_get_drvdata(pdev);

	smartfan_quiesce(fan);
	cancel_delayed_work_sync(&fan->expiry_work);
}

static int __maybe_unused smartfan_suspend(struct device *dev)
{
	struct smartfan *fan = dev_get_drvdata(dev);

	mutex_lock(&fan->lock);
	fan->suspended = true;
	if (!fan->removed)
		smartfan_stop_locked(fan, SMARTFAN_STOP_USER);
	mutex_unlock(&fan->lock);
	cancel_delayed_work_sync(&fan->expiry_work);
	return 0;
}

static int __maybe_unused smartfan_resume(struct device *dev)
{
	struct smartfan *fan = dev_get_drvdata(dev);

	mutex_lock(&fan->lock);
	fan->suspended = false;
	/* Resume leaves the motor OFF; only explicit SET ON may restart it. */
	mutex_unlock(&fan->lock);
	return 0;
}

static SIMPLE_DEV_PM_OPS(smartfan_pm_ops, smartfan_suspend, smartfan_resume);

static const struct of_device_id smartfan_of_match[] = {
	{.compatible = "edu,jetson-smartfan"},
	{}};
MODULE_DEVICE_TABLE(of, smartfan_of_match);

static struct platform_driver smartfan_driver = {
	.probe = smartfan_probe,
	.remove_new = smartfan_remove,
	.shutdown = smartfan_shutdown,
	.driver = {
		.name = "smartfan",
		.of_match_table = smartfan_of_match,
		.pm = &smartfan_pm_ops,
	},
};
module_platform_driver(smartfan_driver);

MODULE_AUTHOR("jetson-smart-fan-controller contributors");
MODULE_DESCRIPTION("Bounded-lease GPIO/PWM smart fan motor and LED Bar control");
MODULE_LICENSE("GPL");
