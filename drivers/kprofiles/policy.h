/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _KPROFILES_POLICY_H
#define _KPROFILES_POLICY_H

#include <linux/kprofiles.h>
#include <linux/minmax.h>
#include <linux/pm_qos.h>

/* Disabled always wins, including over automatic management. */
static inline unsigned int kp_resolve_mode(unsigned int selected, bool automatic,
		bool interactive, bool override, unsigned int temporary)
{
	if (selected == KP_MODE_DISABLED)
		return KP_MODE_DISABLED;
	if (automatic && !interactive)
		return KP_MODE_BATTERY;
	return override ? temporary : selected;
}

enum kp_cpu_class {
	KP_CPU_LITTLE,
	KP_CPU_BIG,
	KP_CPU_PRIME,
};

#define KP_WAKE_GRACE_MS	500

static inline void kp_cpu_limits(unsigned int state, enum kp_cpu_class cpu_class,
		unsigned int hw_min, unsigned int hw_max,
		unsigned int *min_freq, unsigned int *max_freq)
{
	unsigned int mode = state & KP_STATE_MODE_MASK;
	unsigned int percent = 80;
	bool interactive = state & KP_STATE_INTERACTIVE;

	*min_freq = FREQ_QOS_MIN_DEFAULT_VALUE;
	*max_freq = FREQ_QOS_MAX_DEFAULT_VALUE;
	if (kp_state_battery(state)) {
		if (!interactive) {
			if (cpu_class == KP_CPU_LITTLE)
				percent = 60;
			else if (cpu_class == KP_CPU_BIG)
				percent = 50;
			else
				percent = 45;
		}
		*max_freq = max(hw_min, hw_max / 100 * percent);
	} else if (mode == KP_MODE_PERFORMANCE && interactive) {
		*min_freq = max(hw_min, hw_max / 5 * 2);
	}
}

int kp_cpu_init(void);
void kp_cpu_exit(void);
void kp_cpu_update(unsigned int state);
void kp_cpu_sync(void);

#endif
