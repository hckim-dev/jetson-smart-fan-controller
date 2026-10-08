// SPDX-License-Identifier: GPL-2.0
/* GPIO consumer for one fixed-direction motor, with bounded ON leases. */
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
	unsigned long lease_deadline;
	unsigned long on_deadline;
	u32 lease_ms;
	u32 max_on_ms;
	u32 stop_reason;
	bool running;
	bool owner;
	bool removed;
	bool suspended;
};

static void smartfan_free(struct kref *refs)
{
	kfree(container_of(refs, struct smartfan, refs));
}

/* Caller holds lock; resources must still belong to this platform device. */
static void smartfan_stop_locked(struct smartfan *fan, u32 reason)
{
	gpiod_set_value_cansleep(fan->enable, 0);
	gpiod_set_value_cansleep(fan->in1, 0);
	gpiod_set_value_cansleep(fan->in2, 0);
	fan->running = false;
	fan->stop_reason = reason;
}

static void smartfan_expire_locked(struct smartfan *fan, unsigned long now)
{
	if (!fan->running)
		return;
	if (time_after_eq(now, fan->on_deadline))
		smartfan_stop_locked(fan, SMARTFAN_STOP_MAX_ON);
	else if (time_after_eq(now, fan->lease_deadline))
		smartfan_stop_locked(fan, SMARTFAN_STOP_LEASE);
}

static void smartfan_schedule_locked(struct smartfan *fan)
{
	unsigned long deadline;
	unsigned long now = jiffies;

	deadline = time_before(fan->lease_deadline, fan->on_deadline) ? fan->lease_deadline : fan->on_deadline;
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
	struct smartfan_status status = {0};
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
	case SMARTFAN_IOC_GET:
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
	now = jiffies;
	/* Check expiry here as well: delayed work scheduling is not a deadline. */
	smartfan_expire_locked(fan, now);
	switch (cmd)
	{
	case SMARTFAN_IOC_SET:
		if (!request.enabled)
		{
			smartfan_stop_locked(fan, SMARTFAN_STOP_USER);
			break;
		}
		if (!fan->running)
		{
			/* Only an explicit ON command may start a new run. */
			gpiod_set_value_cansleep(fan->in2, 0);
			gpiod_set_value_cansleep(fan->in1, 1);
			fan->on_deadline = now + msecs_to_jiffies(fan->max_on_ms);
			gpiod_set_value_cansleep(fan->enable, 1);
			fan->running = true;
		}
		/* Repeat ON renews the lease, but never extends on_deadline. */
		fan->lease_deadline = now + msecs_to_jiffies(fan->lease_ms);
		smartfan_schedule_locked(fan);
		break;
	case SMARTFAN_IOC_HEARTBEAT:
		if (!fan->running)
		{
			ret = fan->stop_reason == SMARTFAN_STOP_LEASE ||
						  fan->stop_reason == SMARTFAN_STOP_MAX_ON
					  ? -ETIMEDOUT
					  : -EPIPE;
			break;
		}
		fan->lease_deadline = now + msecs_to_jiffies(fan->lease_ms);
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
				jiffies_to_msecs(fan->lease_deadline - now);
			status.on_remaining_ms =
				jiffies_to_msecs(fan->on_deadline - now);
		}
		break;
	}
out:
	mutex_unlock(&fan->lock);
	if (!ret && cmd == SMARTFAN_IOC_GET &&
		copy_to_user((void __user *)arg, &status, sizeof(status)))
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
	int ret;

	BUILD_BUG_ON(sizeof(struct smartfan_request) != 16);
	BUILD_BUG_ON(sizeof(struct smartfan_status) != 32);
	fan = kzalloc(sizeof(*fan), GFP_KERNEL);
	if (!fan)
		return -ENOMEM;
	kref_init(&fan->refs);
	mutex_init(&fan->lock);
	INIT_DELAYED_WORK(&fan->expiry_work, smartfan_expiry_work);
	fan->lease_ms = SMARTFAN_DEFAULT_LEASE_MS;
	fan->max_on_ms = SMARTFAN_DEFAULT_MAX_ON_MS;
	fan->stop_reason = SMARTFAN_STOP_INITIAL;
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
	ret = smartfan_get_output(dev, "in1", &fan->in1);
	if (ret)
		goto fail;
	ret = smartfan_get_output(dev, "in2", &fan->in2);
	if (ret)
		goto fail;
	fan->misc.minor = MISC_DYNAMIC_MINOR;
	fan->misc.name = "smartfan";
	fan->misc.fops = &smartfan_fops;
	fan->misc.parent = dev;
	fan->misc.mode = 0600;
	platform_set_drvdata(pdev, fan);
	ret = misc_register(&fan->misc);
	if (ret)
		goto fail;
	dev_info(dev, "OFF; lease=%u ms, max-on=%u ms\n", fan->lease_ms,
			 fan->max_on_ms);
	return 0;
fail:
	/* Only successfully configured outputs are safe to access. */
	if (fan->enable)
		gpiod_set_value_cansleep(fan->enable, 0);
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
MODULE_DESCRIPTION("Bounded-lease GPIO motor control for a verified smart fan circuit");
MODULE_LICENSE("GPL");
