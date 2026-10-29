# Kprofiles for peridot

Kprofiles starts in Balanced mode and applies its initial CPU policy during
driver initialization. The peridot defconfig already enables the driver and
the QTI devfreq integration. No additional config fragment, profile file,
boot script or manual sysfs initialization is required.

XiaomiParts continues to select the global and per-app profiles through the
existing kp_mode interface. Its saved choices are restored by the app.

| Mode | CPU policy | GPU policy |
| --- | --- | --- |
| 0 Disabled | No kprofiles votes | No kprofiles votes |
| 1 Battery, interactive | Ceiling at 80% of each policy's hardware maximum | Highest OPP at or below 736 MHz |
| 1 Battery, display inactive | Little/big/prime ceilings at 60%/50%/45% of hardware maximum | Highest OPP at or below 353 MHz |
| 2 Balanced | No additional ceiling or floor | Demand driven |
| 3 Performance | Floor at 40% of maximum while interactive | Demand driven |

CPU limits are resolved to supported frequencies by cpufreq. The driver owns
a separate pair of QoS requests for each CPU policy, including policies created
after initialization. Switching profiles changes only those requests.
Userspace, input boost and thermal constraints retain their own lifetimes.
CPU roles are derived from architectural capacity, not fixed CPU numbers. The
lowest-capacity policies are little; the highest are prime; intermediate ones
are big. On a symmetric CPU layout all policies use the little percentage.
These are ceilings, not fixed operating frequencies: governors may go lower.

The QTI devfreq driver supplies a separate GPU Battery vote for peridot's
KGSL GPU. Cooling state N maps directly to frequency table entry N in every
profile. Performance never suppresses thermal constraints or forces the GPU
to its maximum frequency.

## WALT Battery policy

Only Battery changes WALT's effective tuning; saved userspace tunables remain
untouched. Prime hispeed is capped at 2188800 kHz and its load threshold is at
least 85%. Lower frequency or higher threshold settings from userspace win.
cpufreq resolves requests to the device's supported frequency table.

Ordinary load below target_load_thresh uses 15% frequency headroom instead of
25%. Explicit WALT boost reasons, positive governor boost, RT work and uclamp
minimums keep the original mapping. Hispeed/RTG therefore retain the mapping
used by target_util(), and their frequency floors are not scaled down as a
side effect. Mode changes refresh the cached hispeed utilization on the next
governor callback. Existing input boost and predictive-load settings are kept.

## Fingerprint wake grace

The mi_fp threaded IRQ calls kp_wake_hint() before notifying fingerprint HAL.
When the display is inactive, this releases Battery CPU/GPU ceilings and the
WALT Battery adjustments for 500 ms. CPU QoS votes are flushed and active CPU
policies refreshed before the call returns. The governor and existing HAL
boosts choose frequencies; no new frequency floor is imposed. Thermal and
other clients' limits still apply.

The hint does not change the selected mode, forge screen_on=1, or take a new
wakelock. A new fingerprint IRQ while inactive extends the grace. It survives
the subsequent display-on transition to cover unlock, then expires against
the current profile. A genuine new screen-off transition, suspend preparation,
or selecting Disabled cancels it. Duplicate screen_on=0 writes from multiple
userspace clients do not cancel an in-progress fingerprint wake. Background
resume events do not receive grace. There is no periodic polling.

Build and deploy the matching mi_fp and graphics-kernel modules with this
kernel. KGSL must recognize the default maximum-frequency QoS value when the
last ceiling is removed; otherwise its previous Battery cap can remain latched.

Deep suspend already stops ordinary CPU execution. These limits target work
while the display is inactive, including background wakeups; reducing a CPU
ceiling does not by itself reduce deep-suspend residency power. Fingerprint
latency and battery savings require measurements on hardware.

## Userspace ABI

- /sys/kernel/kprofiles/kp_mode: selected mode, 0 through 3.
- /sys/module/kprofiles/parameters/auto_kp: automatic Battery mode while the
  display is inactive or the system is suspended; controlled by XiaomiParts.
- /sys/kernel/kprofiles/active_mode: effective mode, read only.
- /sys/kernel/kprofiles/screen_on: display state, supplied by the existing
  Power HAL DISPLAY_INACTIVE hint and XiaomiParts KProfiles service. The app
  synchronizes PowerManager.isInteractive() on startup and screen events,
  including when no per-app profiles exist.
- /sys/kernel/kprofiles/wake_active: read-only indication of fingerprint grace.

The display starts active. The suspend notifier also releases CPU floors.
With auto_kp enabled, display inactivity or suspend temporarily selects
Battery; waking restores the selected profile. Display inactivity and suspend
release the Performance CPU floor even when automatic mode selection is off.
Writing kp_mode or auto_kp waits for the corresponding workqueue update.

## Kernel API

Include linux/kprofiles.h. kp_active_mode() is a side-effect-free READ_ONCE
getter safe for scheduler paths. Setters require sleepable context.

kp_set_mode(level) changes the selected mode and cancels a temporary override.
kp_set_mode_rollback(level, duration_ms) applies a temporary override without
sleeping for its duration. The latest request replaces the previous one;
zero duration cancels it. Expiration resolves against the selected profile.
Screen-off and suspend cancel temporary overrides. Disabled takes precedence
over both automatic mode selection and temporary overrides.

Blocking notifications use KP_MODE_CHANGE and carry the integer mode in data.
They run on a workqueue without the state mutex. Callbacks may read or change
mode, but must not unregister themselves from the same blocking chain.
Registration does not replay a notification: initialize clients from
kp_active_mode(), serializing that read with callbacks when needed.
Repeated effective modes do not generate duplicate notifications.

KP_STATE_CHANGE also notifies clients about changes to interactive/wake state
without a mode change. Its data is the packed unsigned state. kp_get_state()
returns a coherent, lock-free snapshot: KP_STATE_MODE_MASK selects the mode,
KP_STATE_INTERACTIVE indicates an active display outside suspend, and
KP_STATE_WAKE indicates wake grace. GPU clients use this event and initialize
from kp_get_state() under their own callback lock.

kp_wake_hint() requires sleepable context and must not be called from the
kprofiles blocking notifier chain. Its caller waits for profile application
and CPU policy refresh, so it is intended for the fingerprint threaded IRQ,
not a hard IRQ or scheduler callback. Disabled ignores wake hints.

## Validation

Host regression tests compile the actual main.c handlers and extracted WALT,
QTI GPU and KGSL functions against small host fakes:

```sh
python3 tools/testing/selftests/kprofiles/run_host_tests.py \
    --modules /path/to/sm8635-modules --cc cc
```

They cover screen-only notifications, CPU ceilings, repeated screen-off writes,
fingerprint grace/extension/expiration, suspend cancellation, background resume,
Disabled precedence, jiffies wraparound, OPP selection, WALT boost preservation,
and releasing the final KGSL ceiling. The fake workqueue is single-threaded;
these tests do not replace a kernel/modules build or concurrency testing.

On device, compare screen-off CPU limits and wake_active during repeated
fingerprint unlocks (including failed attempts), AOD, rapid lock/unlock and
background resume. Record frame deadlines and power for identical workloads,
brightness, refresh rate and temperature. Check suspend residency as well as
average current, and verify thermal caps remain effective during wake grace.
