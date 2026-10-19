/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_KPROFILES_H
#define _LINUX_KPROFILES_H

#include <linux/notifier.h>

enum kp_mode {
	KP_MODE_DISABLED,
	KP_MODE_BATTERY,
	KP_MODE_BALANCED,
	KP_MODE_PERFORMANCE,
};

extern unsigned int KP_MODE_CHANGE;

/* Setters require sleepable context; applying the policy is asynchronous. */
void kp_set_mode(unsigned int level);
void kp_set_mode_rollback(unsigned int level, unsigned int duration_ms);
int kp_active_mode(void);
int kp_notifier_register_client(struct notifier_block *nb);
int kp_notifier_unregister_client(struct notifier_block *nb);

#endif
