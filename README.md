# Smooth Auto Brightness for GNOME

Smooth Auto Brightness for GNOME replaces only GNOME's ambient-light brightness
policy with low-overhead, gradual panel transitions. Manual brightness keys and
the GNOME slider remain immediate and continue to recalibrate the automatic
brightness baseline.

The daemon is native C and event-driven. At rest it performs no periodic work
unless an explicitly enabled hardware keepalive requires it. During a
transition it uses deadline-based, frame-limited interpolation; it does not run
a fixed-rate loop while idle.

## Status

This project is pre-release. Use `--check` and `--dry-run` before allowing it to
control a display.

## Runtime interfaces

- `net.hadess.SensorProxy` on the system D-Bus for ambient lux readings
- `org.gnome.SettingsDaemon.Power.Screen` on the session D-Bus for brightness
- `sd-event` one-shot timers for transitions and optional sensor keepalives

No GNOME Shell extension, direct backlight write permission, or root daemon is
required.

## Build

Ubuntu/Debian build dependencies:

```bash
sudo apt install build-essential cmake ninja-build pkg-config libsystemd-dev
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
this option.

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

- Ambient target filter time constant: 1.6 seconds
- Brightening: one percentage point every 40 ms
- Dimming: one percentage point every 60 ms
- Maximum transition duration: 250 ms
- Maximum active update rate: 60 Hz
- Automatic range: 2–100 percent
- Manual changes: immediate, cancel the current transition, and recalibrate

Short transitions retain the natural 40/60 ms-per-point timing. Longer
transitions are compressed to 250 ms and use no more than 60 updates per
second, avoiding high-rate D-Bus traffic while keeping large changes
responsive.

All values are configurable from the command line; see `--help`.

## License

GPL-3.0-or-later.
