# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
from importlib.machinery import SourceFileLoader
from pathlib import Path
import sys
from types import SimpleNamespace


script = Path(sys.argv[1])
spec = importlib.util.spec_from_loader(
    "sabg_calibration",
    SourceFileLoader("sabg_calibration", str(script)),
)
module = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(module)

assert module.isotonic_increasing([]) == []
assert module.isotonic_increasing([1.0, 2.0, 3.0]) == [1.0, 2.0, 3.0]
assert module.isotonic_increasing([1.0, 3.0, 2.0, 4.0]) == [1.0, 2.5, 2.5, 4.0]
assert module.isotonic_increasing([3.0, 2.0, 1.0]) == [2.0, 2.0, 2.0]
assert module.isotonic_increasing([0.0, 10.0, 0.0], [1, 1, 3]) == [0.0, 2.5, 2.5]

try:
    module.isotonic_increasing([1.0], [0])
except ValueError:
    pass
else:
    raise AssertionError("zero weight accepted")

records = [
    {"direction": "ascending", "command": 0, "lux": 1.0, "raw_by_device": {"panel": 0}},
    {"direction": "ascending", "command": 1, "lux": 1.1, "raw_by_device": {"panel": 0}},
    {"direction": "ascending", "command": 2, "lux": 5.0, "raw_by_device": {"panel": 1}},
    {"direction": "descending", "command": 2, "lux": 4.8, "raw_by_device": {"panel": 1}},
    {"direction": "descending", "command": 1, "lux": 1.2, "raw_by_device": {"panel": 0}},
]
profile = module.build_profile(
    records,
    {"panel": {"path": "/nonexistent", "type": "firmware", "maximum": 1}},
    SimpleNamespace(settle_ms=250, samples=3, sample_ms=150),
)
assert profile["selected_backlight"] == "panel"
assert profile["distinct_levels"] == 2
assert profile["entries"][0]["commands"] == [0, 1]
assert profile["entries"][1]["step_lux"] > 0.0
assert profile["entries"][1]["step_fraction_of_range"] == 1.0
coordinates = profile["perceptual_timing"]["command_coordinates"]
assert len(coordinates) == 101
assert abs(coordinates[1] - 0.05) < 1e-9
assert all(right > left for left, right in zip(coordinates, coordinates[1:]))

linear = module.locally_smoothed_response(list(range(9)), [0, 1, 2, 3, 5, 5, 6, 7, 8])
assert linear[0] == 0
assert linear[-1] == 8
assert all(right >= left for left, right in zip(linear, linear[1:]))
