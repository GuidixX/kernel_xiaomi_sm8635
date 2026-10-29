/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_KPROFILES_H
#define _LINUX_KPROFILES_H

#include <linux/kconfig.h>
#include <linux/notifier.h>

enum kp_mode {
	KP_MODE_DISABLED,
	KP_MODE_BATTERY,
	KP_MODE_BALANCED,
	KP_MODE_PERFORMANCE,
};

extern unsigned int KP_MODE_CHANGE;

/* A single snapshot keeps mode, display state and wake grace coherent. */
#define KP_STATE_MODE_MASK	0x3U
#define KP_STATE_INTERACTIVE	0x4U
#define KP_STATE_WAKE		0x8U
#define KP_STATE_CHANGE		0x80000001UL

static inline bool kp_state_battery(unsigned int state)
{
	return (state & KP_STATE_MODE_MASK) == KP_MODE_BATTERY &&
		!(state & KP_STATE_WAKE);
}

#if IS_REACHABLE(CONFIG_KPROFILES)
unsigned int kp_get_state(void);
/* Sleepable context only, never a kprofiles notifier callback. */
void kp_wake_hint(void);
#else
static inline unsigned int kp_get_state(void)
{
	return KP_MODE_DISABLED | KP_STATE_INTERACTIVE;
}

static inline void kp_wake_hint(void) { }
#endif

/* Setters require sleepable context; applying the policy is asynchronous. */
void kp_set_mode(unsigned int level);
void kp_set_mode_rollback(unsigned int level, unsigned int duration_ms);
int kp_active_mode(void);
int kp_notifier_register_client(struct notifier_block *nb);
int kp_notifier_unregister_client(struct notifier_block *nb);

#endif
