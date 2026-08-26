# Changelog

## 0.5.0 - 2026-08-25

Initial stable release.

- Smooth, velocity-preserving display and keyboard-backlight transitions.
- Adaptive 2–60 Hz scheduling with no fixed-rate idle loop.
- Ten-second time-weighted ambient-light averaging to prevent low-light
  feedback oscillation.
- Independent manual display and keyboard calibration with persisted resume
  baselines.
- Lid, suspend, hibernation, SensorProxy restart, and Apple ALS reprobe
  recovery.
- Optional perceptual display-response calibration without software dimming or
  invented panel levels.
- Hardened user service and low-resource runtime guardrails.
- Fourteen automated regression tests covering the controller, scheduler,
  calibration, suspend behavior, and Apple ALS keepalive.
