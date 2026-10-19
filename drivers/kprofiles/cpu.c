// SPDX-License-Identifier: GPL-2.0
/* Independent CPU frequency votes; never overwrite userspace or thermal QoS. */
#define pr_fmt(fmt) "kprofiles: " fmt

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#include "policy.h"

struct kp_cpu_policy {
	struct list_head link;
	struct cpufreq_policy *policy;
	struct freq_qos_request min_req;
	struct freq_qos_request max_req;
};

static LIST_HEAD(kp_policies);
static DEFINE_MUTEX(kp_cpu_lock);
static unsigned int kp_cpu_mode = CONFIG_KP_DEFAULT_MODE;
static bool kp_cpu_interactive = true;
static bool kp_cpu_stopping;

static void kp_cpu_apply(struct kp_cpu_policy *entry)
{
	unsigned int min_freq, max_freq;
	int ret;

	kp_cpu_limits(kp_cpu_mode, kp_cpu_interactive,
		entry->policy->cpuinfo.min_freq, entry->policy->cpuinfo.max_freq,
		&min_freq, &max_freq);
	/* Release a performance floor before installing a battery ceiling. */
	ret = freq_qos_update_request(&entry->min_req, min_freq);
	if (ret < 0)
		pr_warn_ratelimited("CPU%u: min vote failed: %d\n",
				    entry->policy->cpu, ret);
	ret = freq_qos_update_request(&entry->max_req, max_freq);
	if (ret < 0)
		pr_warn_ratelimited("CPU%u: max vote failed: %d\n",
				    entry->policy->cpu, ret);
}

void kp_cpu_update(unsigned int mode, bool interactive)
{
	struct kp_cpu_policy *entry;

	mutex_lock(&kp_cpu_lock);
	kp_cpu_mode = mode;
	kp_cpu_interactive = interactive;
	list_for_each_entry(entry, &kp_policies, link)
		kp_cpu_apply(entry);
	mutex_unlock(&kp_cpu_lock);
}

static int kp_cpu_add(struct cpufreq_policy *policy)
{
	struct kp_cpu_policy *entry;
	int ret = 0;

	mutex_lock(&kp_cpu_lock);
	if (kp_cpu_stopping)
		goto unlock;
	list_for_each_entry(entry, &kp_policies, link) {
		if (entry->policy == policy)
			goto unlock;
	}
	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) {
		ret = -ENOMEM;
		goto unlock;
	}
	entry->policy = policy;
	ret = freq_qos_add_request(&policy->constraints, &entry->min_req,
				  FREQ_QOS_MIN, FREQ_QOS_MIN_DEFAULT_VALUE);
	if (ret < 0)
		goto free_entry;
	ret = freq_qos_add_request(&policy->constraints, &entry->max_req,
				  FREQ_QOS_MAX, FREQ_QOS_MAX_DEFAULT_VALUE);
	if (ret < 0) {
		freq_qos_remove_request(&entry->min_req);
		goto free_entry;
	}
	list_add_tail(&entry->link, &kp_policies);
	kp_cpu_apply(entry);
	ret = 0;
	goto unlock;

free_entry:
	kfree(entry);
unlock:
	mutex_unlock(&kp_cpu_lock);
	return ret;
}

static void kp_cpu_remove(struct kp_cpu_policy *entry)
{
	freq_qos_remove_request(&entry->min_req);
	freq_qos_remove_request(&entry->max_req);
	list_del(&entry->link);
	kfree(entry);
}

static int kp_cpufreq_event(struct notifier_block *nb, unsigned long event,
			    void *data)
{
	struct cpufreq_policy *policy = data;
	struct kp_cpu_policy *entry, *next;
	int ret;

	if (event == CPUFREQ_CREATE_POLICY) {
		ret = kp_cpu_add(policy);
		if (ret)
			pr_err("CPU%u: cannot add profile votes: %d\n", policy->cpu, ret);
		return NOTIFY_OK;
	}
	if (event != CPUFREQ_REMOVE_POLICY)
		return NOTIFY_DONE;
	mutex_lock(&kp_cpu_lock);
	list_for_each_entry_safe(entry, next, &kp_policies, link) {
		if (entry->policy == policy) {
			kp_cpu_remove(entry);
			break;
		}
	}
	mutex_unlock(&kp_cpu_lock);
	return NOTIFY_OK;
}

static struct notifier_block kp_cpufreq_nb = {
	.notifier_call = kp_cpufreq_event,
};

void kp_cpu_exit(void)
{
	struct kp_cpu_policy *entry, *next;

	/* Keep REMOVE notifications registered until all pointers are retired. */
	mutex_lock(&kp_cpu_lock);
	kp_cpu_stopping = true;
	list_for_each_entry_safe(entry, next, &kp_policies, link)
		kp_cpu_remove(entry);
	mutex_unlock(&kp_cpu_lock);
	cpufreq_unregister_notifier(&kp_cpufreq_nb, CPUFREQ_POLICY_NOTIFIER);
}

int kp_cpu_init(void)
{
	struct cpufreq_policy *policy;
	unsigned int cpu;
	int ret;

	ret = cpufreq_register_notifier(&kp_cpufreq_nb, CPUFREQ_POLICY_NOTIFIER);
	if (ret)
		return ret;
	/* Cover policies that exist when kprofiles is loaded as a module. */
	for_each_possible_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;
		down_read(&policy->rwsem);
		ret = cpumask_empty(policy->cpus) ? 0 : kp_cpu_add(policy);
		up_read(&policy->rwsem);
		cpufreq_cpu_put(policy);
		if (ret) {
			kp_cpu_exit();
			return ret;
		}
	}
	return 0;
}
