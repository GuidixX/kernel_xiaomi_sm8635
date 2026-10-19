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

static inline void kp_cpu_limits(unsigned int mode, bool interactive,
		unsigned int hw_min, unsigned int hw_max,
		unsigned int *min_freq, unsigned int *max_freq)
{
	*min_freq = FREQ_QOS_MIN_DEFAULT_VALUE;
	*max_freq = FREQ_QOS_MAX_DEFAULT_VALUE;
	if (mode == KP_MODE_BATTERY)
		*max_freq = max(hw_min, hw_max / 5 * 4);
	else if (mode == KP_MODE_PERFORMANCE && interactive)
		*min_freq = max(hw_min, hw_max / 5 * 2);
}

int kp_cpu_init(void);
void kp_cpu_exit(void);
void kp_cpu_update(unsigned int mode, bool interactive);

#endif
