# Development notes

## 2026-08-22 calibrated v0.5.0 baseline

The approved implementation is on branch `adaptive-motion`. The calibrated
transition work landed in `d8c6dc3`; `09f2cff` adds Python cache hygiene. The
branch was pushed to `origin/adaptive-motion` and had not yet been merged into
`main` at the end of this session.

### Installed state on `dipswitch`

- Release binary: `~/.local/bin/smooth-autobrightness-for-gnome`, version 0.5.0.
- Calibration utility: `~/.local/bin/smooth-autobrightness-calibrate`.
- User service: enabled and running with `--apple-als-keepalive` only; verbose
  logging is off.
- Guardrails: `CPUQuota=5%`, `CPUQuotaPeriodSec=100ms`, `MemoryMax=32M`, and
  `TasksMax=4`.
- GNOME's ambient policy is disabled. The old `apple-als-poller.service` is
  installed but disabled; do not re-enable it while the integrated keepalive is
  active.
- Machine-local profile:
  `~/.config/smooth-autobrightness-for-gnome/display-calibration.json`. It is
  intentionally not stored in Git.

The hardened unit uses `ProtectHome=yes` and exposes only the default profile
with `BindReadOnlyPaths`. If a non-default profile under the home directory is
selected, update the unit's bind path as well.

### Calibration and timing decisions

- The internal panel exposes 90 distinct firmware backlight levels through
  `acpi_video0` (raw 1–90 for GNOME commands 0–100).
- The Apple ALS exposes integer illuminance at 5 Hz; there is no finer raw
  channel. The accepted white-screen mirror sweep measured an approximately
  0–38 lux reflected range.
- Sensor quantization is smoothed with a local monotonic response fit. Do not
  invent intermediate brightness levels or use temporal dithering/software
  overlays.
- The response profile changes transition timing only. It preserves real target
  levels and accelerates boundaries that are perceptually conspicuous near
  black.
- Accepted curve: real low-end boundaries clamp at a 0.20 cadence factor,
  duplicate GNOME commands use 0.05, the median factor from 50–90% is about
  0.96, and the full curve spans about 59.7 effective coordinate points.
- The 0.20 real-step floor pairs with the scheduler's 0.20 maximum coordinate
  step per frame, preventing calibrated motion from deliberately skipping a
  physical panel level.
- Multiple gray-screen sweeps were considered and rejected: they reduce ALS
  signal and add panel-gamma/black-leakage uncertainty without increasing the
  usable hardware resolution.

### Verified baseline

All ten tests passed in Debug, Release, GCC sanitizer, and Clang sanitizer
builds. The live calibrated release was visually approved as the best result of
the session.

Thirty-second runtime samples:

| State | CPU (one core) | Context switches | RSS | Throttles |
|---|---:|---:|---:|---:|
| Stable lighting | 0.078% cgroup | 5.8/s | 3.8 MiB | 0 |
| Repeated light/dark transitions | 0.219% cgroup | 31.5/s | 3.8 MiB | 0 |

The service had zero restarts. Active use was roughly 1/23 of the conservative
5% CPU quota.

### Resume after burn-in

After a reboot and a useful run period, check:

```bash
systemctl --user status smooth-autobrightness-for-gnome.service
journalctl --user -u smooth-autobrightness-for-gnome.service -b --no-pager
systemctl --user show smooth-autobrightness-for-gnome.service \
  -p NRestarts -p MemoryCurrent -p MemoryPeak -p CPUUsageNSec
```

Confirm the journal reports both the Apple ALS keepalive path and the display
response profile, with no profile parse errors, throttling symptoms, or restart
loop. If burn-in remains clean, the next repository decision is whether to merge
`adaptive-motion` into `main` and tag v0.5.0. Touch Bar control remains deferred.
