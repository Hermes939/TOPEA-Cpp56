---
name: Dynamic basket target formula calibration
description: Why the $/lot base rate and bonus tiers in effectiveTarget() were lowered, and the untestable velocity coefficient
---

The dynamic target formula (`base = lots × rate × tickValue × bonus`) is
calibrated against realized $/lot profitability from historical trade data
(`top4ea_cpp/data/automl_training.csv`), not chosen arbitrarily.

**Why:** Multiplicative bonus tiers that scale up the target for larger
baskets can easily overshoot the real $/lot profitability of those baskets,
especially once recovery/martingale lots dilute average efficiency —
larger baskets (5+ lots) also tend to have high-variance, sometimes
negative median outcomes, so pushing their target higher makes it *less*
likely to close in profit, not more. Always backtest against realized
`pnl_result` per lot bucket before raising bonus tiers or base rate,
rather than assuming higher lots deserve proportionally higher targets.

**How to apply:** If `automl_training.csv` (or its successor) gains a
logged per-cycle P&L velocity ($/tick) series, the velocity coefficient
(currently 0.30, unverified) can finally be backtested too — it could not
be calibrated this round because the dataset only has per-cycle
lot/pnl/drawdown snapshots, not a velocity time series. When editing this
formula, also check the console dashboard print block in
`top4ea_engine.cpp` (`oss << "║  Target ..."`) — it independently
hardcodes the same base rate/bonus constants for display and will silently
drift out of sync with `effectiveTarget()` if only one is updated.
