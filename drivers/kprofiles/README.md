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
| 1 Battery | Ceiling at 80% of each policy's hardware maximum | Highest OPP at or below 80% of maximum |
| 2 Balanced | No additional ceiling or floor | Demand driven |
| 3 Performance | Floor at 40% of maximum while interactive | Demand driven |

CPU limits are resolved to supported frequencies by cpufreq. The driver owns
a separate pair of QoS requests for each CPU policy, including policies created
after initialization. Switching profiles changes only those requests.
Userspace, input boost and thermal constraints retain their own lifetimes.

The QTI devfreq driver supplies a separate GPU Battery vote for peridot's
KGSL GPU. Cooling state N maps directly to frequency table entry N in every
profile. Performance never suppresses thermal constraints or forces the GPU
to its maximum frequency.

## Userspace ABI

- /sys/kernel/kprofiles/kp_mode: selected mode, 0 through 3.
- /sys/module/kprofiles/parameters/auto_kp: automatic Battery mode while the
  display is inactive or the system is suspended; controlled by XiaomiParts.
- /sys/kernel/kprofiles/active_mode: effective mode, read only.
- /sys/kernel/kprofiles/screen_on: display state, supplied by the existing
  Power HAL DISPLAY_INACTIVE hint and its device init/SELinux integration.

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
