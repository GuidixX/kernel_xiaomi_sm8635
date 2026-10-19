// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2021-2022, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "%s:%s " fmt, KBUILD_MODNAME, __func__

#include <linux/devfreq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/pm_qos.h>
#include <linux/slab.h>
#include <linux/thermal.h>
#include <linux/workqueue.h>

#if IS_REACHABLE(CONFIG_KPROFILES)
#include <linux/kprofiles.h>
#include <linux/mutex.h>
#endif

#define MAX_RETRY_CNT 20
#define RETRY_DELAY msecs_to_jiffies(1000)
#define DEVFREQ_CDEV_NAME "gpu"
#define DEVFREQ_CDEV "qcom-devfreq-cdev"

struct devfreq_cdev_device {
	struct device_node *np;
	struct devfreq *devfreq;
	struct device *dev;
	unsigned long *freq_table;
	int cur_state;
	int max_state;
	int retry_cnt;
	struct dev_pm_qos_request qos_max_freq_req;
	struct delayed_work register_work;
	struct thermal_cooling_device *cdev;
#if IS_REACHABLE(CONFIG_KPROFILES)
	struct dev_pm_qos_request kp_max_freq_req;
	struct mutex kp_lock;
	struct notifier_block kp_nb;
#endif
};

static struct devfreq_cdev_device *devfreq_cdev;

#if IS_REACHABLE(CONFIG_KPROFILES)
/* Profile and thermal limits are independent votes in the devfreq QoS list. */
static int devfreq_cdev_kp_notifier_cb(struct notifier_block *nb,
				     unsigned long action, void *data)
{
	struct devfreq_cdev_device *cdev_data =
		container_of(nb, struct devfreq_cdev_device, kp_nb);
	unsigned long limit = PM_QOS_MAX_FREQUENCY_DEFAULT_VALUE;
	unsigned long target;
	int i, ret;

	if (action != KP_MODE_CHANGE)
		return NOTIFY_DONE;

	mutex_lock(&cdev_data->kp_lock);
	/* Read inside the lock so initial application cannot race a notification. */
	if (kp_active_mode() == KP_MODE_BATTERY) {
		target = cdev_data->freq_table[0] / 5 * 4;
		for (i = 0; i < cdev_data->max_state; i++)
			if (cdev_data->freq_table[i] <= target)
				break;
		limit = cdev_data->freq_table[i];
	}
	ret = dev_pm_qos_update_request(&cdev_data->kp_max_freq_req, limit);
	mutex_unlock(&cdev_data->kp_lock);
	if (ret < 0)
		dev_warn_ratelimited(cdev_data->dev, "GPU profile vote failed: %d\n", ret);
	return NOTIFY_OK;
}

static void devfreq_cdev_kp_init(struct devfreq_cdev_device *cdev_data)
{
	int ret;

	mutex_init(&cdev_data->kp_lock);
	ret = dev_pm_qos_add_request(cdev_data->dev, &cdev_data->kp_max_freq_req,
				    DEV_PM_QOS_MAX_FREQUENCY,
				    PM_QOS_MAX_FREQUENCY_DEFAULT_VALUE);
	if (ret < 0)
		goto error;
	cdev_data->kp_nb.notifier_call = devfreq_cdev_kp_notifier_cb;
	ret = kp_notifier_register_client(&cdev_data->kp_nb);
	if (ret) {
		dev_pm_qos_remove_request(&cdev_data->kp_max_freq_req);
		goto error;
	}
	devfreq_cdev_kp_notifier_cb(&cdev_data->kp_nb, KP_MODE_CHANGE, NULL);
	return;
error:
	dev_warn(cdev_data->dev, "Cannot add kprofiles GPU vote: %d\n", ret);
}
#endif

static int devfreq_cdev_set_state(struct thermal_cooling_device *cdev,
					unsigned long state)
{
	struct devfreq_cdev_device *cdev_data = cdev->devdata;
	int ret = 0;
	unsigned long freq;

	if (state > cdev_data->max_state)
		return -EINVAL;
	if (state == cdev_data->cur_state)
		return 0;
	freq = cdev_data->freq_table[state];
	pr_debug("cdev:%s state:%lu limit:%lu\n", cdev->type, state, freq);
	ret = dev_pm_qos_update_request(&cdev_data->qos_max_freq_req, freq);
	if (ret < 0) {
		pr_err("Error placing qos request:%lu. cdev:%s err:%d\n",
				freq, cdev->type, ret);
		return ret;
	}
	cdev_data->cur_state = state;

	return 0;
}

static int devfreq_cdev_get_state(struct thermal_cooling_device *cdev,
					unsigned long *state)
{
	struct devfreq_cdev_device *cdev_data = cdev->devdata;

	*state = cdev_data->cur_state;
	return 0;
}

static int devfreq_cdev_get_max_state(struct thermal_cooling_device *cdev,
					unsigned long *state)
{
	struct devfreq_cdev_device *cdev_data = cdev->devdata;

	*state = cdev_data->max_state;
	return 0;
}

static struct thermal_cooling_device_ops devfreq_cdev_ops = {
	.set_cur_state = devfreq_cdev_set_state,
	.get_cur_state = devfreq_cdev_get_state,
	.get_max_state = devfreq_cdev_get_max_state,
};

static void devfreq_cdev_work(struct work_struct *work)
{
	struct devfreq *df = NULL;
	unsigned long freq = ULONG_MAX;
	unsigned long *freq_table;
	struct dev_pm_opp *opp;
	int ret = 0, freq_ct, i;
	struct devfreq_cdev_device *cdev_data = container_of(work,
						struct devfreq_cdev_device,
						register_work.work);

	df = devfreq_get_devfreq_by_node(cdev_data->np);
	if (IS_ERR(df)) {
		ret = PTR_ERR(df);
		pr_debug("Devfreq not available:%d\n", ret);
		if (--cdev_data->retry_cnt)
			queue_delayed_work(system_highpri_wq,
					&cdev_data->register_work,
					RETRY_DELAY);
		return;
	}

	cdev_data->dev = df->dev.parent;
	cdev_data->devfreq = df;
	freq_ct = dev_pm_opp_get_opp_count(cdev_data->dev);
	if (freq_ct <= 0)
		return;
	freq_table = kcalloc(freq_ct, sizeof(*freq_table), GFP_KERNEL);
	if (!freq_table) {
		ret = -ENOMEM;
		return;
	}

	cdev_data->freq_table = freq_table;
	for (i = 0, freq = ULONG_MAX; i < freq_ct; i++, freq--) {

		opp = dev_pm_opp_find_freq_floor(cdev_data->dev, &freq);
		if (IS_ERR(opp)) {
			ret = PTR_ERR(opp);
			goto qos_exit;
		}
		dev_pm_opp_put(opp);

		freq_table[i] = DIV_ROUND_UP(freq, 1000); //hz to khz
		pr_debug("%d. freq table:%lu\n", i, freq_table[i]);
	}
	cdev_data->max_state = freq_ct - 1;
	ret = dev_pm_qos_add_request(cdev_data->dev,
					&cdev_data->qos_max_freq_req,
					DEV_PM_QOS_MAX_FREQUENCY,
					PM_QOS_MAX_FREQUENCY_DEFAULT_VALUE);
	if (ret < 0)
		goto qos_exit;
	cdev_data->cdev = thermal_cooling_device_register(DEVFREQ_CDEV_NAME,
						cdev_data, &devfreq_cdev_ops);
	if (IS_ERR(cdev_data->cdev)) {
		pr_err("Cdev register failed for gpu, ret:%ld\n",
			PTR_ERR(cdev_data->cdev));
		cdev_data->cdev = NULL;
		goto qos_exit;
	}

#if IS_REACHABLE(CONFIG_KPROFILES)
	devfreq_cdev_kp_init(cdev_data);
#endif

	return;

qos_exit:
	kfree(cdev_data->freq_table);
	cdev_data->freq_table = NULL;
	if (dev_pm_qos_request_active(&cdev_data->qos_max_freq_req))
		dev_pm_qos_remove_request(&cdev_data->qos_max_freq_req);
}

static int devfreq_cdev_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	devfreq_cdev = devm_kzalloc(dev, sizeof(*devfreq_cdev), GFP_KERNEL);
	if (!devfreq_cdev)
		return -ENOMEM;

	devfreq_cdev->np = of_parse_phandle(pdev->dev.of_node,
				"qcom,devfreq", 0);
	if (!devfreq_cdev->np)
		return -EINVAL;
	devfreq_cdev->retry_cnt = MAX_RETRY_CNT;

	INIT_DEFERRABLE_WORK(&devfreq_cdev->register_work, devfreq_cdev_work);
	queue_delayed_work(system_highpri_wq, &devfreq_cdev->register_work, 0);

	return 0;
}

static int devfreq_cdev_remove(struct platform_device *pdev)
{
	cancel_delayed_work_sync(&devfreq_cdev->register_work);
	if (devfreq_cdev->cdev) {
#if IS_REACHABLE(CONFIG_KPROFILES)
		if (dev_pm_qos_request_active(&devfreq_cdev->kp_max_freq_req)) {
			kp_notifier_unregister_client(&devfreq_cdev->kp_nb);
			dev_pm_qos_remove_request(&devfreq_cdev->kp_max_freq_req);
		}
#endif
		thermal_cooling_device_unregister(devfreq_cdev->cdev);
		dev_pm_qos_remove_request(&devfreq_cdev->qos_max_freq_req);
		kfree(devfreq_cdev->freq_table);
		devfreq_cdev->cdev = NULL;
	}
	of_node_put(devfreq_cdev->np);

	return 0;
}

static const struct of_device_id devfreq_cdev_match[] = {
	{.compatible = "qcom,devfreq-cdev"},
	{}
};

static struct platform_driver devfreq_cdev_driver = {
	.probe          = devfreq_cdev_probe,
	.remove         = devfreq_cdev_remove,
	.driver         = {
		.name   = DEVFREQ_CDEV,
		.of_match_table = devfreq_cdev_match,
	},
};

static int __init devfreq_cdev_init(void)
{
	return platform_driver_register(&devfreq_cdev_driver);
}
module_init(devfreq_cdev_init);

static void __exit devfreq_cdev_exit(void)
{
	platform_driver_unregister(&devfreq_cdev_driver);
}
module_exit(devfreq_cdev_exit);

MODULE_DESCRIPTION("Qualcomm Technologies, Inc. devfreq cooling device driver");
MODULE_LICENSE("GPL");
