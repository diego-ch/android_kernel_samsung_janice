#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/input-polldev.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/earlysuspend.h>
#include "alps.h"

#define ALPSIO	0xAF
#define ALPSIO_SET_MAGACTIVATE   _IOW(ALPSIO, 0, int)
#define ALPSIO_SET_ACCACTIVATE   _IOW(ALPSIO, 1, int)
#define ALPSIO_SET_DELAY         _IOW(ALPSIO, 2, int)

#define EVENT_TYPE_ACCEL_X          ABS_X
#define EVENT_TYPE_ACCEL_Y          ABS_Y
#define EVENT_TYPE_ACCEL_Z          ABS_Z

#define EVENT_TYPE_MAGV_X           ABS_HAT0X
#define EVENT_TYPE_MAGV_Y           ABS_HAT0Y
#define EVENT_TYPE_MAGV_Z           ABS_BRAKE

#define ALPS_POLL_INTERVAL			100	/* msecs */
#define ALPS_INPUT_FUZZ				0	/* event threshold */
#define ALPS_INPUT_FLAT				0

static void alps_early_suspend(struct early_suspend *handler);
static void alps_early_resume(struct early_suspend *handler);

struct alps_data {
	struct platform_device *pdev;
	struct input_polled_dev *alps_idev;
	struct mutex alps_lock;
	struct miscdevice alps_device;
#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend alps_early_suspend;
#endif
	int suspend_flag;
	int delay;
	int flgM;
	int flgA;
};

/* for I/O Control */
static long alps_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int ret = -1, tmpval;
	void __user *argp = (void __user *)arg;
	struct alps_data *data = container_of(filp->private_data,
					     struct alps_data, alps_device);

	switch (cmd) {
	case ALPSIO_SET_MAGACTIVATE:
			ret = copy_from_user(&tmpval, argp, sizeof(tmpval));
			if (ret) {
				pr_info("error : ioctl(ALPSIO_SET_MAGACTIVATE)\n");
				return -EFAULT;
			}
			mutex_lock(&data->alps_lock);
			data->flgM = tmpval;
			hscd_activate(1, tmpval, data->delay);
			mutex_unlock(&data->alps_lock);
			break;
	case ALPSIO_SET_ACCACTIVATE:
			ret = copy_from_user(&tmpval, argp, sizeof(tmpval));
			if (ret) {
				pr_info("error : ioctl(cmd = ALPSIO_SET_ACCACTIVATE)\n");
				return -EFAULT;
			}
			mutex_lock(&data->alps_lock);
			data->flgA = tmpval;
			accsns_activate(1, data->flgA, data->delay);
			mutex_unlock(&data->alps_lock);
			break;

	case ALPSIO_SET_DELAY:
			ret = copy_from_user(&tmpval, argp, sizeof(tmpval));
			if (ret) {
				pr_info("error : ioctl(ALPSIO_SET_DELAY)\n");
				return -EFAULT;
			}
			if (tmpval <=  10)
				tmpval =  10;
			else if (tmpval <=  20)
				tmpval =  20;
			else if (tmpval <=  60)
				tmpval =  60;
			else
				tmpval = 100;
			mutex_lock(&data->alps_lock);
			data->delay = tmpval;

			if (data->flgM)
				hscd_activate(1, data->flgM, data->delay);
			if (data->flgA)
				accsns_activate(1, data->flgA, data->delay);
			mutex_unlock(&data->alps_lock);
			break;
	default:
			return -ENOTTY;
	}
	return 0;
}

static int alps_io_open(struct inode *inode, struct file *filp)
{
	pr_info("alps_io_open\n");
	return 0;
}

static int alps_io_release(struct inode *inode, struct file *filp)
{
	pr_info("alps_io_release\n");
	return 0;
}

static const struct file_operations alps_fops = {
	.owner   = THIS_MODULE,
	.open    = alps_io_open,
	.release = alps_io_release,
	.unlocked_ioctl = alps_ioctl,
};

static void accsns_poll(struct input_dev *idev)
{
	int xyz[3];

	if (accsns_get_acceleration_data(xyz) == 0) {
		input_report_abs(idev, EVENT_TYPE_ACCEL_X, xyz[0]);
		input_report_abs(idev, EVENT_TYPE_ACCEL_Y, xyz[1]);
		input_report_abs(idev, EVENT_TYPE_ACCEL_Z, xyz[2]);
		idev->sync = 0;
		input_event(idev, EV_SYN, SYN_REPORT, 1);
	}
}

static void hscd_poll(struct input_dev *idev)
{
	int xyz[3];

	if (hscd_get_magnetic_field_data(xyz) == 0) {
		input_report_abs(idev, EVENT_TYPE_MAGV_X, xyz[0]);
		input_report_abs(idev, EVENT_TYPE_MAGV_Y, xyz[1]);
		input_report_abs(idev, EVENT_TYPE_MAGV_Z, xyz[2]);
		idev->sync = 0;
		input_event(idev, EV_SYN, SYN_REPORT, 2);
	}
}

static void alps_poll(struct input_polled_dev *dev)
{
	struct alps_data *data = dev->private;

	if (!data->suspend_flag) {
		mutex_lock(&data->alps_lock);
		data->alps_idev->poll_interval = data->delay;

		if (data->flgM)
			hscd_poll(data->alps_idev->input);
		if (data->flgA)
			accsns_poll(data->alps_idev->input);
		mutex_unlock(&data->alps_lock);
	}
}

static int alps_probe(struct platform_device *dev)
{
	int ret;
	struct alps_data *data;
	struct input_dev *idev;

	pr_info("alps: alps_probe\n");

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (data == NULL) {
		pr_err("%s, failed to alloc memory for module data\n",
			__func__);
		ret = -ENOMEM;
		goto exit;
	}
	mutex_init(&data->alps_lock);

	data->pdev = platform_device_register_simple("alps", -1, NULL, 0);
	if (IS_ERR(data->pdev)) {
		ret = PTR_ERR(data->pdev);
		goto out_driver;
	}

	data->alps_idev = input_allocate_polled_device();
	if (data->alps_idev == NULL) {
		ret = -ENOMEM;
		goto out_device;
	}

	data->alps_idev->poll = alps_poll;
	data->alps_idev->poll_interval = ALPS_POLL_INTERVAL;
	data->alps_idev->private = data;

	/* initialize the input class */
	idev = data->alps_idev->input;
	idev->name = "alps";
	idev->phys = "alps/input0";
	idev->id.bustype = BUS_HOST;
	idev->dev.parent = &data->pdev->dev;
	idev->evbit[0] = BIT_MASK(EV_ABS);

	input_set_abs_params(idev, EVENT_TYPE_ACCEL_X,
			-4096, 4096, ALPS_INPUT_FUZZ, ALPS_INPUT_FLAT);
	input_set_abs_params(idev, EVENT_TYPE_ACCEL_Y,
			-4096, 4096, ALPS_INPUT_FUZZ, ALPS_INPUT_FLAT);
	input_set_abs_params(idev, EVENT_TYPE_ACCEL_Z,
			-4096, 4096, ALPS_INPUT_FUZZ, ALPS_INPUT_FLAT);

	input_set_abs_params(idev, EVENT_TYPE_MAGV_X,
			-4096, 4096, ALPS_INPUT_FUZZ, ALPS_INPUT_FLAT);
	input_set_abs_params(idev, EVENT_TYPE_MAGV_Y,
			-4096, 4096, ALPS_INPUT_FUZZ, ALPS_INPUT_FLAT);
	input_set_abs_params(idev, EVENT_TYPE_MAGV_Z,
			-4096, 4096, ALPS_INPUT_FUZZ, ALPS_INPUT_FLAT);

	ret = input_register_polled_device(data->alps_idev);
	if (ret)
		goto out_idev;

	data->alps_device.minor = MISC_DYNAMIC_MINOR;
	data->alps_device.name = "alps_io";
	data->alps_device.fops = &alps_fops;
	ret = misc_register(&data->alps_device);
	if (ret)
		goto exit_misc_device_register_failed;

#ifdef CONFIG_HAS_EARLYSUSPEND
	data->alps_early_suspend.suspend = alps_early_suspend;
	data->alps_early_suspend.resume = alps_early_resume;
	register_early_suspend(&data->alps_early_suspend);
#endif

	data->flgA = 0;
	data->flgM = 0;
	data->delay = 200;
	data->suspend_flag = OFF;

	pr_info("%s: success.\n", __func__);
	return 0;

exit_misc_device_register_failed:
	input_unregister_polled_device(data->alps_idev);
out_idev:
	input_free_polled_device(data->alps_idev);
out_device:
	platform_device_unregister(data->pdev);
out_driver:
	mutex_destroy(&data->alps_lock);
	kfree(data);
exit:
	pr_err("%s: failed!\n", __func__);
	return ret;
}

static int alps_remove(struct platform_device *dev)
{
	struct alps_data *data = platform_get_drvdata(dev);

	misc_deregister(&data->alps_device);
	input_unregister_polled_device(data->alps_idev);
	input_free_polled_device(data->alps_idev);
	platform_device_unregister(data->pdev);
	mutex_destroy(&data->alps_lock);

	kfree(data);

	return 0;
}

static int alps_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct alps_data *data = platform_get_drvdata(pdev);

	data->suspend_flag = ON;

	return 0;
}

static int alps_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct alps_data *data = platform_get_drvdata(pdev);

	data->suspend_flag = OFF;

	return 0;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void alps_early_suspend(struct early_suspend *handler)
{
	struct alps_data *data;
	data = container_of(handler, struct alps_data, alps_early_suspend);

	data->suspend_flag = ON;
}

static void alps_early_resume(struct early_suspend *handler)
{
	struct alps_data *data;
	data = container_of(handler, struct alps_data, alps_early_suspend);

	data->suspend_flag = OFF;
}
#endif

static const struct dev_pm_ops alps_pm_ops = {
	.suspend = alps_suspend,
	.resume = alps_resume
};

static struct platform_driver alps_driver = {
	.driver	= {
#ifndef CONFIG_HAS_EARLYSUSPEND
		.pm = &alps_pm_ops,
#endif
		.name = "alps-input",
		.owner = THIS_MODULE,
	},
	.probe = alps_probe,
	.remove = alps_remove,
};

static int __init alps_init(void)
{
	return platform_driver_register(&alps_driver);
}

static void __exit alps_exit(void)
{
	platform_driver_unregister(&alps_driver);
}

module_init(alps_init);
module_exit(alps_exit);

MODULE_DESCRIPTION("Alps Input Device");
MODULE_AUTHOR("ALPS");
MODULE_LICENSE("GPL v2");
