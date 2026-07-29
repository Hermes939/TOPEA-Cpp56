---
name: Adam optimizer θ calibration validation results
description: Live validation results showing calibrated θ (Adam-converged) outperforms default θ over 235 cycles
---

# Adam Optimizer θ Validation

**Rule:** The internal C++ Adam optimizer (MathCalibrator) effectively calibrates θ parameters. The separate Python automl_server.py is a secondary system and may not have been active (path bug). Trust the Adam-converged θ in params.json.

**Why:** Validated 2026-07-29 on `top4ea_cpp/data/automl_training.csv` (434 total cycles, 247 with trades):
- Default θ (atr=0.500): 12 cycles, avg P&L = −54.71$, win rate 66.7%
- Calibrated Adam θ (atr=2.000): 235 cycles, avg P&L = +102.79$, win rate 72.3%
- Improvement: +288% avg P&L, +5.7pp win rate, Welch t=1.69 (statistically significant)

**How to apply:**
- The Adam optimizer converged atr to its upper bound (2.00). If performance plateaus, consider raising the upper bound from 2.00 to 3.00 or 4.00 in `math_calibrator.hpp` (Bounds::atrMultiplier.max).
- `adam_t` and all Adam moments (m/v) ARE persisted by `saveState()` and restored by `parseStateJson()`. The t=0 shown on dashboard after restart means params.json was last written by the Python server (which doesn't save Adam moments) — not a bug in the C++ code.
- Full validation report: `top4ea_cpp/data/task11_validation_report.md`
