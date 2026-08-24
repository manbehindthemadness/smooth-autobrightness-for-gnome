# Smooth Auto Brightness for GNOME

Smooth Auto Brightness for GNOME replaces GNOME's ambient-light brightness
policy with low-overhead, gradual transitions for the display and keyboard
backlight. Manual controls remain immediate and independently recalibrate each
backlight's automatic baseline.

The daemon is native C and event-driven. At rest it performs no periodic work
unless an explicitly enabled hardware keepalive requires it. During a
transition it uses a velocity-preserving, frame-limited trajectory and writes
only when the panel's integer brightness changes; it does not run a fixed-rate
loop while idle.

## Status

This project is pre-release. Use `--check` and `--dry-run` before allowing it to
control a display.

## Runtime interfaces

- `net.hadess.SensorProxy` on the system D-Bus for ambient lux readings
- `org.gnome.SettingsDaemon.Power.Screen` on the session D-Bus for brightness
- `org.gnome.SettingsDaemon.Power.Keyboard` for keyboard illumination when available
- `org.freedesktop.UPower.KbdBacklight` as the portable keyboard fallback
- `org.freedesktop.login1.Manager` for lid and suspend/resume state
- `sd-event` one-shot timers for transitions and optional sensor keepalives

The keyboard adapter discovers the desktop interface first and then UPower,
including UPower's newer per-device object enumeration. Native hardware ranges
are normalized to percentages; no vendor-specific LED name is assumed. No
GNOME Shell extension, direct sysfs write permission, or root daemon is required.

## Build

Ubuntu/Debian build dependencies:

```bash
sudo apt install \
  build-essential cmake gir1.2-gtk-4.0 libsystemd-dev ninja-build pkg-config python3-gi
```

Configure, build, and test:

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Install the binary and user service (the default prefix is `/usr/local`):

```bash
sudo cmake --install build/dev
systemctl --user daemon-reload
```

## Safe interface check

This claims the sensor long enough to verify both D-Bus interfaces but does not
change brightness:

```bash
./build/dev/smooth-autobrightness-for-gnome --check
```

To observe target calculation without changing brightness:

```bash
./build/dev/smooth-autobrightness-for-gnome --dry-run --verbose
```

## GNOME setup

The daemon and GNOME's built-in ambient controller must not run concurrently.
After dry-run testing, disable GNOME's controller and start this daemon:

```bash
gsettings set org.gnome.settings-daemon.plugins.power ambient-enabled false
systemctl --user enable --now smooth-autobrightness-for-gnome.service
```

Restore stock behavior at any time:

```bash
systemctl --user disable --now smooth-autobrightness-for-gnome.service
gsettings reset org.gnome.settings-daemon.plugins.power ambient-enabled
```

## Optional Apple ALS keepalive

Some Intel MacBook ambient-light drivers update only when their IIO sysfs value
is read. Enable the optional adapter with:

```bash
smooth-autobrightness-for-gnome --apple-als-keepalive
```

It discovers an IIO device named `als` and reads its illuminance attribute every
500 ms. Systems whose SensorProxy readings update normally should not enable
this option. Each refresh opens the sysfs attribute anew so an IIO reprobe or
suspend/resume cycle cannot leave the daemon using a stale descriptor. If the
device is temporarily unavailable, the adapter rediscovers it and retries;
repeated diagnostics are limited to one per minute until it recovers.
The timer allows a small wakeup-coalescing window and is disabled while the lid
is closed or the system is preparing to sleep.

The daemon also watches the SensorProxy D-Bus name. If SensorProxy exits during
an IIO reprobe, suspend, hibernation, hotplug, or an independent service restart,
the old light claim is discarded. When a replacement owner appears, the daemon
reclaims the sensor and explicitly refreshes its light level after the existing
settle window. Recovery uses bounded retries and does not depend on a particular
sleep implementation or system-level workaround.

Enable it for the installed user service with a drop-in:

```bash
systemctl --user edit smooth-autobrightness-for-gnome.service
```

Add:

```ini
[Service]
Environment="SABG_OPTIONS=--apple-als-keepalive"
```

Then apply the change:

```bash
systemctl --user daemon-reload
systemctl --user restart smooth-autobrightness-for-gnome.service
```

## Default policy

- Brightening envelope time constant: 1.6 seconds
- Dimming envelope time constant: approximately 0.53 seconds
- Final 4-point dimming tail: handed to the bounded trajectory controller
- Gradual brightening and dimming: one percentage point every 40 ms
- Changes of 12 percentage points or more: accelerated catch-up response
- Maximum transition duration: 250 ms
- Adaptive active update rate: 2–60 Hz based on envelope velocity and deadline
- Target hysteresis: 2 percentage points, with reachable 0%/100% endpoints
- Automatic range: 2–100 percent
- Display response: brighter environment means a brighter display
- Keyboard response: darker environment means brighter key illumination
- Display and keyboard baselines: independent
- Manual changes: immediate, cancel only that device's transition, and recalibrate
- Manual keyboard off: suspend keyboard automation until manually raised above zero
- Automatic keyboard zero: remain active and brighten again when the room darkens
- Display power-down: turn keyboard illumination off, then resume its ambient target on wake
- Lid/suspend guard: restore the last manually selected display and keyboard levels, reject
  covered-sensor readings, and wait two seconds before accepting a fresh ambient sample

Short trajectory corrections retain the natural 40 ms-per-point timing.
Larger corrections are compressed to 250 ms. Retargeting preserves velocity,
and output is quantized to actual integer brightness changes. The scheduler
runs near-stationary, multi-minute drift at 2 Hz, scales through intermediate
rates for gradual motion, and reaches 60 Hz only for rapid changes or deadline
pressure.

Closing a laptop lid makes its ambient sensor report darkness before suspend.
The daemon pauses ambient control as soon as logind reports the lid closed (and
also on non-lid suspend), restores the last manually selected display and
keyboard levels on wake, and reapplies them once after GNOME's own wake handling.
It then gives the sensor two seconds to settle. Resuming resets filter timing
but does not change the user's calibrated ambient-response curve. Manual choices
are persisted only when they change, under the service's private XDG state
directory, so daemon restarts do not turn an automatic level into the new resume
baseline.

The transition cap, natural step rates, ambient filter, hysteresis, and
automatic range are configurable from the command line; see `--help`. Set
`--hysteresis 0` to disable output hysteresis. The 2–60 Hz adaptive range is an
internal safety limit. `--legacy-transitions` temporarily retains the pre-0.4
restarted-fade controller for compatibility testing.

Keyboard control is enabled when a supported interface is present. Disable it
without affecting display automation with `--no-keyboard-backlight`. Systems
without a keyboard backlight continue with display-only operation.

## Display response calibration

The optional calibration utility measures the display's real per-step response
with mirror-reflected ambient-light readings. The profile is a step-salience
map: coarse hardware jumps can be traversed quickly to mask stair-stepping,
while fine steps retain the normal aesthetic cadence. It is not intended to
linearize real-world luminance. The runtime continues using real low backlight
levels rather than temporal dithering or a power-wasting software dimming
overlay. The utility requires Python GObject and GTK 4, available on
Ubuntu/Debian as `python3-gi` and `gir1.2-gtk-4.0`.

Verify the interfaces without changing brightness:

```bash
smooth-autobrightness-calibrate --check
```

For a sweep, place the computer in a completely dark room facing a mirror and
run:

```bash
smooth-autobrightness-calibrate
```

The utility presents instructions before entering fullscreen white, temporarily
stops this daemon and GNOME's ambient policy, measures ascending and descending
passes, restores the original brightness and services, and writes a monotonic
profile under `~/.config/smooth-autobrightness-for-gnome/`. Escape cancels and
restores state. A forced kill or power loss cannot perform restoration.

The daemon automatically loads `display-calibration.json` from that directory
at startup. It smooths sensor quantization during calibration and uses the
resulting perceptual coordinate only for transition timing: conspicuous steps
are crossed sooner, targets still settle on real hardware brightness levels,
and no level is synthesized. Use `--no-calibration-profile` for an uncalibrated
comparison or `--calibration-profile PATH` to select another profile. Timing
metadata in an older completed profile can be regenerated without another
sweep:

```bash
smooth-autobrightness-calibrate --refresh-profile
```

## License

GPL-3.0-or-later.
