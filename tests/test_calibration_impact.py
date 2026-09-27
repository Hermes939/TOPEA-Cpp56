#!/usr/bin/env python3
"""Forward validation of the calibrated theta vector.

The runtime error log records the realized P&L and the theta values that were
available when a cycle ran.  For each counterfactual parameter vector this
check keeps the realized P&L and applies a deterministic target-sized penalty
for distance from the parameters used by that cycle.  This is intentionally a
small, dependency-free forward model rather than a claim that historical P&L
can be replayed tick-for-tick from the error log.

At the moment error_log.csv records atr_mult_used.  Other theta dimensions are
loaded and validated from params.json, and will automatically participate when
their corresponding *_used columns are added to the runtime log.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import sys
from pathlib import Path
from typing import Dict, Iterable, Mapping


THETA_KEYS = (
    "atr_mult",
    "kelly_frac",
    "gap_expansion",
    "trail_asym",
    "vel_trigger",
    "cppi_mult",
    "vabt_coeff",
    "ou_theta",
)

DEFAULT_THETA = {
    "atr_mult": 0.50,
    "kelly_frac": 0.25,
    "gap_expansion": 0.05,
    "trail_asym": 0.15,
    "vel_trigger": 0.85,
    "cppi_mult": 3.00,
    "vabt_coeff": 0.15,
    "ou_theta": 0.05,
}

# The same hard bounds enforced by ParamBounds in math_calibrator.hpp.
THETA_BOUNDS = {
    "atr_mult": (0.20, 2.00),
    "kelly_frac": (0.05, 0.50),
    "gap_expansion": (0.01, 0.30),
    "trail_asym": (0.05, 0.40),
    "vel_trigger": (0.60, 0.98),
    "cppi_mult": (1.50, 6.00),
    "vabt_coeff": (0.05, 0.50),
    "ou_theta": (0.01, 0.30),
}

# Runtime CSV column -> theta key. Missing columns are skipped deliberately;
# error_log.csv currently persists only atr_mult_used.
OBSERVED_COLUMNS = {
    "atr_mult_used": "atr_mult",
    "kelly_frac_used": "kelly_frac",
    "gap_exp_used": "gap_expansion",
    "trail_asym_used": "trail_asym",
    "vel_trigger_used": "vel_trigger",
    "cppi_mult_used": "cppi_mult",
    "vabt_coeff_used": "vabt_coeff",
    "ou_theta_used": "ou_theta",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    root = Path(__file__).resolve().parents[1]
    parser.add_argument(
        "--csv",
        type=Path,
        default=root / "top4ea_cpp" / "data" / "error_log.csv",
        help="runtime error log CSV",
    )
    parser.add_argument(
        "--params",
        type=Path,
        default=root / "top4ea_cpp" / "automl" / "output" / "params.json",
        help="calibrated theta JSON",
    )
    parser.add_argument("--cycles", type=int, default=20)
    parser.add_argument("--min-win-rate", type=float, default=0.70)
    return parser.parse_args()


def load_calibrated_theta(path: Path) -> Dict[str, float]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise AssertionError(f"cannot load calibrated theta from {path}: {exc}") from exc

    missing = [key for key in THETA_KEYS if key not in payload]
    if missing:
        raise AssertionError(f"{path} is missing theta keys: {', '.join(missing)}")

    theta = {key: float(payload[key]) for key in THETA_KEYS}
    validate_theta(theta, "calibrated")
    return theta


def validate_theta(theta: Mapping[str, float], label: str) -> None:
    for key in THETA_KEYS:
        value = theta[key]
        if not math.isfinite(value):
            raise AssertionError(f"{label} theta {key} is not finite: {value}")
        lower, upper = THETA_BOUNDS[key]
        if not lower <= value <= upper:
            raise AssertionError(
                f"{label} theta {key}={value} is outside [{lower}, {upper}]"
            )


def load_rows(path: Path, cycles: int) -> list[dict[str, str]]:
    try:
        with path.open(newline="", encoding="utf-8") as handle:
            rows = list(csv.DictReader(handle))
    except OSError as exc:
        raise AssertionError(f"cannot load runtime error log {path}: {exc}") from exc

    required = {"pnl_result", "target_expected"}
    if not rows:
        raise AssertionError(f"{path} contains no cycle rows")
    missing = required.difference(rows[0])
    if missing:
        raise AssertionError(f"{path} is missing required columns: {sorted(missing)}")

    observed = [column for column in OBSERVED_COLUMNS if column in rows[0]]
    if not observed:
        raise AssertionError(
            f"{path} contains no recorded theta columns; expected one of "
            f"{sorted(OBSERVED_COLUMNS)}"
        )
    if len(rows) < cycles:
        raise AssertionError(f"{path} has {len(rows)} cycles; need at least {cycles}")
    return rows[-cycles:]


def fit_score(row: Mapping[str, str], theta: Mapping[str, float]) -> float:
    """Return [0, 1], where 1 means an exact match to recorded theta."""
    distances = []
    for column, key in OBSERVED_COLUMNS.items():
        if column not in row or row[column] == "":
            continue
        observed = float(row[column])
        lower, upper = THETA_BOUNDS[key]
        distances.append(min(1.0, abs(theta[key] - observed) / (upper - lower)))
    if not distances:
        raise AssertionError("cycle has no usable recorded theta value")
    return 1.0 - sum(distances) / len(distances)


def expected_pnl(row: Mapping[str, str], theta: Mapping[str, float]) -> float:
    """Forward estimate for one cycle under a candidate theta vector.

    We preserve the observed cycle result and charge one target-sized unit for
    normalized theta mismatch.  This makes the counterfactual conservative:
    it does not multiply profitable or losing P&L by an arbitrary leverage
    factor, and it treats a mismatch as a missed opportunity/risk cost.
    """
    realized_pnl = float(row["pnl_result"])
    target_unit = max(abs(float(row["target_expected"])), 1.0)
    return realized_pnl + target_unit * (fit_score(row, theta) - 1.0)


def simulate(rows: Iterable[Mapping[str, str]], theta: Mapping[str, float]) -> list[float]:
    return [expected_pnl(row, theta) for row in rows]


def main() -> int:
    args = parse_args()
    if args.cycles < 1:
        raise AssertionError("--cycles must be positive")
    if not 0.0 <= args.min_win_rate <= 1.0:
        raise AssertionError("--min-win-rate must be between 0 and 1")

    calibrated = load_calibrated_theta(args.params)
    validate_theta(DEFAULT_THETA, "default")
    rows = load_rows(args.csv, args.cycles)

    default_pnl = simulate(rows, DEFAULT_THETA)
    calibrated_pnl = simulate(rows, calibrated)
    calibrated_not_worse = sum(
        candidate >= baseline
        for candidate, baseline in zip(calibrated_pnl, default_pnl)
    )
    win_rate = calibrated_not_worse / len(rows)
    default_total = sum(default_pnl)
    calibrated_total = sum(calibrated_pnl)

    print(f"Calibration impact: {len(rows)} latest cycles")
    print(f"  default expected P&L   : {default_total:.4f}")
    print(f"  calibrated expected P&L: {calibrated_total:.4f}")
    print(
        f"  calibrated >= default  : {calibrated_not_worse}/{len(rows)} "
        f"({win_rate:.1%})"
    )
    print(
        "  recorded theta columns : "
        + ", ".join(column for column in OBSERVED_COLUMNS if column in rows[0])
    )

    if calibrated_total < default_total:
        raise AssertionError(
            "calibrated expected P&L is lower than default expected P&L"
        )
    if win_rate < args.min_win_rate:
        raise AssertionError(
            f"calibrated theta is not better on {args.min_win_rate:.0%} "
            f"of cycles (observed {win_rate:.1%})"
        )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, ValueError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)