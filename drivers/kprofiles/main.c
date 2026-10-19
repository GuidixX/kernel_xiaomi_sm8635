// SPDX-License-Identifier: GPL-2.0
/*
 * KernelSpace Profiles
 * Copyright (C) 2021-2025 Dakkshesh <dakkshesh5@gmail.com>
 * Copyright (C) 2026 GuidixX <guidixm@gmail.com>
 *
 * Selected mode, temporary override and display/suspend state are independent.
 * QoS updates and blocking notifications run in workqueue context.
 */
#define pr_fmt(fmt) "kprofiles: " fmt

#include <linux/jiffies.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/suspend.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#include "policy.h"
#include "version.h"

static BLOCKING_NOTIFIER_HEAD(kp_mode_notifier);
unsigned int KP_MODE_CHANGE = 0x80000000;
EXPORT_SYMBOL_GPL(KP_MODE_CHANGE);

static DEFINE_MUTEX(kp_lock);
static unsigned int kp_mode = CONFIG_KP_DEFAULT_MODE;
static unsigned int kp_effective_mode = CONFIG_KP_DEFAULT_MODE;
static unsigned int kp_override_mode;
static unsigned long kp_override_until;
static bool kp_override;
static bool auto_kp;
static bool screen_on = true;
static bool suspended;
static bool kp_ready;
static struct kobject *kp_kobj;

static void kp_apply_workfn(struct work_struct *work);
static void kp_expire_workfn(struct work_struct *work);
static DECLARE_WORK(kp_apply_work, kp_apply_workfn);
static DECLARE_DELAYED_WORK(kp_expire_work, kp_expire_workfn);

/* Caller holds kp_lock. The hot-path getter never sleeps or notifies. */
static void kp_refresh_locked(void)
{
	unsigned int mode = kp_resolve_mode(kp_mode, auto_kp,
			screen_on && !suspended, kp_override, kp_override_mode);

	WRITE_ONCE(kp_effective_mode, mode);
	if (kp_ready)
		schedule_work(&kp_apply_work);
}

static void kp_apply_workfn(struct work_struct *work)
{
	static unsigned int notified_mode = UINT_MAX;
	unsigned int mode;
	bool interactive;

	mutex_lock(&kp_lock);
	mode = kp_effective_mode;
	interactive = screen_on && !suspended;
	mutex_unlock(&kp_lock);

	kp_cpu_update(mode, interactive);
	if (mode == notified_mode)
		return;
	notified_mode = mode;
	/* Callbacks may read or change the profile; do not hold kp_lock. */
	blocking_notifier_call_chain(&kp_mode_notifier, KP_MODE_CHANGE,
				     (void *)(uintptr_t)mode);
}

static void kp_expire_workfn(struct work_struct *work)
{
	unsigned long now;

	mutex_lock(&kp_lock);
	now = jiffies;
	if (kp_override) {
		if (time_before(now, kp_override_until)) {
			mod_delayed_work(system_wq, &kp_expire_work,
					 kp_override_until - now);
		} else {
			kp_override = false;
			kp_refresh_locked();
		}
	}
	mutex_unlock(&kp_lock);
}

/*
 * Latest temporary request wins. It expires against the current selected
 * profile, never a saved profile that could overwrite a newer user choice.
 * A zero duration cancels an override. Unlike v6 this never sleeps for duration.
 */
void kp_set_mode_rollback(unsigned int level, unsigned int duration_ms)
{
	if (level > KP_MODE_PERFORMANCE)
		return;
	mutex_lock(&kp_lock);
	kp_override = duration_ms != 0;
	kp_override_mode = level;
	if (kp_override) {
		kp_override_until = jiffies + msecs_to_jiffies(duration_ms);
		mod_delayed_work(system_wq, &kp_expire_work,
				 msecs_to_jiffies(duration_ms));
	} else {
		cancel_delayed_work(&kp_expire_work);
	}
	kp_refresh_locked();
	mutex_unlock(&kp_lock);
}
EXPORT_SYMBOL_GPL(kp_set_mode_rollback);

void kp_set_mode(unsigned int level)
{
	if (level > KP_MODE_PERFORMANCE)
		return;
	mutex_lock(&kp_lock);
	WRITE_ONCE(kp_mode, level);
	kp_override = false;
	cancel_delayed_work(&kp_expire_work);
	kp_refresh_locked();
	mutex_unlock(&kp_lock);
}
EXPORT_SYMBOL_GPL(kp_set_mode);

int kp_active_mode(void)
{
	return READ_ONCE(kp_effective_mode);
}
EXPORT_SYMBOL_GPL(kp_active_mode);

int kp_notifier_register_client(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&kp_mode_notifier, nb);
}
EXPORT_SYMBOL_GPL(kp_notifier_register_client);

int kp_notifier_unregister_client(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&kp_mode_notifier, nb);
}
EXPORT_SYMBOL_GPL(kp_notifier_unregister_client);

static int kp_auto_set(const char *val, const struct kernel_param *param)
{
	bool enable, ready;
	int ret = kstrtobool(val, &enable);

	if (ret)
		return ret;
	mutex_lock(&kp_lock);
	auto_kp = enable;
	ready = kp_ready;
	kp_refresh_locked();
	mutex_unlock(&kp_lock);
	if (ready)
		flush_work(&kp_apply_work);
	return 0;
}

static int kp_auto_get(char *buf, const struct kernel_param *param)
{
	int ret;

	mutex_lock(&kp_lock);
	ret = param_get_bool(buf, param);
	mutex_unlock(&kp_lock);
	return ret;
}

static const struct kernel_param_ops kp_auto_ops = {
	.set = kp_auto_set,
	.get = kp_auto_get,
};
module_param_cb(auto_kp, &kp_auto_ops, &auto_kp, 0664);
MODULE_PARM_DESC(auto_kp, "Use Battery while display is inactive or suspended");

static ssize_t kp_mode_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(kp_mode));
}

static ssize_t kp_mode_store(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	unsigned int mode;
	int ret = kstrtouint(buf, 10, &mode);

	if (ret)
		return ret;
	if (mode > KP_MODE_PERFORMANCE)
		return -EINVAL;
	kp_set_mode(mode);
	flush_work(&kp_apply_work);
	return count;
}

static ssize_t active_mode_show(struct kobject *kobj, struct kobj_attribute *attr,
				char *buf)
{
	return sysfs_emit(buf, "%d\n", kp_active_mode());
}

static ssize_t screen_on_show(struct kobject *kobj, struct kobj_attribute *attr,
			      char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(screen_on));
}

static ssize_t screen_on_store(struct kobject *kobj, struct kobj_attribute *attr,
			       const char *buf, size_t count)
{
	bool on;
	int ret = kstrtobool(buf, &on);

	if (ret)
		return ret;
	mutex_lock(&kp_lock);
	WRITE_ONCE(screen_on, on);
	/* A transient boost must not reappear after a screen-off interval. */
	if (!on) {
		kp_override = false;
		cancel_delayed_work(&kp_expire_work);
	}
	kp_refresh_locked();
	mutex_unlock(&kp_lock);
	flush_work(&kp_apply_work);
	return count;
}

static struct kobj_attribute kp_mode_attribute =
	__ATTR(kp_mode, 0664, kp_mode_show, kp_mode_store);
static struct kobj_attribute active_mode_attribute = __ATTR_RO(active_mode);
static struct kobj_attribute screen_on_attribute =
	__ATTR(screen_on, 0664, screen_on_show, screen_on_store);

static struct attribute *kp_attrs[] = {
	&kp_mode_attribute.attr,
	&active_mode_attribute.attr,
	&screen_on_attribute.attr,
	NULL,
};
static const struct attribute_group kp_attr_group = {
	.attrs = kp_attrs,
};

static int kp_pm_event(struct notifier_block *nb, unsigned long event, void *data)
{
	bool sleep;

	switch (event) {
	case PM_SUSPEND_PREPARE:
	case PM_HIBERNATION_PREPARE:
		sleep = true;
		break;
	case PM_POST_SUSPEND:
	case PM_POST_HIBERNATION:
	case PM_POST_RESTORE:
		sleep = false;
		break;
	default:
		return NOTIFY_DONE;
	}
	mutex_lock(&kp_lock);
	suspended = sleep;
	if (sleep) {
		kp_override = false;
		cancel_delayed_work(&kp_expire_work);
	}
	kp_refresh_locked();
	mutex_unlock(&kp_lock);
	flush_work(&kp_apply_work);
	return NOTIFY_OK;
}

static struct notifier_block kp_pm_nb = {
	.notifier_call = kp_pm_event,
};

static int __init kp_init(void)
{
	int ret;

	ret = kp_cpu_init();
	if (ret)
		return ret;
	ret = register_pm_notifier(&kp_pm_nb);
	if (ret)
		goto cpu_exit;
	kp_kobj = kobject_create_and_add("kprofiles", kernel_kobj);
	if (!kp_kobj) {
		ret = -ENOMEM;
		goto pm_exit;
	}
	mutex_lock(&kp_lock);
	kp_ready = true;
	kp_refresh_locked();
	mutex_unlock(&kp_lock);
	ret = sysfs_create_group(kp_kobj, &kp_attr_group);
	if (ret)
		goto work_exit;
	/* Complete the initial votes before reporting the driver ready. */
	flush_work(&kp_apply_work);
	pr_info("Kprofiles " KPROFILES_VERSION " initialized\n");
	return 0;

work_exit:
	mutex_lock(&kp_lock);
	kp_ready = false;
	mutex_unlock(&kp_lock);
	cancel_delayed_work_sync(&kp_expire_work);
	cancel_work_sync(&kp_apply_work);
	kobject_put(kp_kobj);
pm_exit:
	unregister_pm_notifier(&kp_pm_nb);
cpu_exit:
	kp_cpu_exit();
	return ret;
}
module_init(kp_init);

static void __exit kp_exit(void)
{
	sysfs_remove_group(kp_kobj, &kp_attr_group);
	unregister_pm_notifier(&kp_pm_nb);
	mutex_lock(&kp_lock);
	kp_ready = false;
	kp_override = false;
	mutex_unlock(&kp_lock);
	cancel_delayed_work_sync(&kp_expire_work);
	cancel_work_sync(&kp_apply_work);
	kp_cpu_exit();
	kobject_put(kp_kobj);
}
module_exit(kp_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("KernelSpace Profiles with independent frequency QoS");
MODULE_AUTHOR("Dakkshesh <dakkshesh5@gmail.com>");
MODULE_VERSION(KPROFILES_VERSION);
