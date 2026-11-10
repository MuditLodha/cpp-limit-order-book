"""Run the C++ market-making study and print a statistical summary."""

import argparse
import csv
import math
import os
from pathlib import Path
import random
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[1]
REGIMES = ("quiet", "normal", "volatile", "informed")
STRATEGIES = ("fixed", "inventory", "as")


def read_csv(path):
    with Path(path).open(newline="", encoding="utf-8-sig") as file:
        rows = list(csv.DictReader(file))
    for row in rows:
        for key, value in row.items():
            if key not in ("group", "regime", "strategy"):
                row[key] = float(value)
                if not math.isfinite(row[key]):
                    raise ValueError(f"Invalid {key} in {path}")
    return rows


def interval(values, seed=42):
    """Mean and percentile-bootstrap 95% confidence interval (3,000 resamples)."""
    if not values:
        raise ValueError("No observations")
    mean = statistics.mean(values)
    if len(values) == 1:
        return mean, mean, mean
    rng = random.Random(seed)
    sample_means = sorted(
        statistics.mean(rng.choices(values, k=len(values)))
        for _ in range(3000)
    )
    return mean, sample_means[74], sample_means[2924]


def audit(directory, rows):
    """Verify summary accounting and independently check example fill ledgers."""
    if not rows:
        raise ValueError("No simulation results")
    keys = [(r["group"], r["regime"], r["strategy"], r["seed"],
             r["half_spread"], r["quote_size"]) for r in rows]
    if len(keys) != len(set(keys)):
        raise ValueError("Duplicate experiment configurations")

    def close(a, b):
        return math.isclose(a, b, rel_tol=1e-8, abs_tol=1e-5)

    for r in rows:
        if r["final_inventory"] != 0 or r["max_abs_inventory"] > r["inventory_limit"]:
            raise ValueError("Inventory limit or liquidation failed")
        if not 0 <= r["fill_rate"] <= 1:
            raise ValueError("Invalid fill rate")
        if not close(r["pre_liquidation_pnl"] - r["liquidation_cost"], r["net_pnl"]):
            raise ValueError("PnL reconciliation failed")
        if r["markout_volume"] + r["censored_markout_volume"] != r["passive_volume"]:
            raise ValueError("Markout reconciliation failed")

    checked = 0
    for path in directory.glob("examples/*/*/trajectory.csv"):
        regime, strategy = path.parent.parent.name, path.parent.name
        candidates = [r for r in rows if r["group"] == "comparison"
                      and r["regime"] == regime and r["strategy"] == strategy]
        expected = min(candidates, key=lambda r: r["seed"])
        fills, points = read_csv(path.with_name("fills.csv")), read_csv(path)
        cash, inventory, fees, passive = 0.0, 0, 0.0, 0
        for f in fills:
            cash -= f["direction"] * f["quantity"] * f["price"] + f["fee"]
            inventory += int(f["direction"] * f["quantity"])
            fees += f["fee"]
            if not f["liquidation"]:
                passive += f["quantity"]
            if abs(inventory) > expected["inventory_limit"]:
                raise ValueError("Fill ledger exceeds inventory limit")
        if (inventory != 0 or not close(cash, expected["net_pnl"])
                or not close(fees, expected["fees"])
                or passive != expected["passive_volume"]):
            raise ValueError("Fill ledger does not match summary")
        for point in points:
            if not close(point["cash"] + point["inventory"] * point["reference"], point["equity"]):
                raise ValueError("Incorrect snapshot equity")
        if not close(points[-1]["equity"], cash):
            raise ValueError("Final equity does not match the fill ledger")
        checked += 1
    return checked


def print_results(rows, checked):
    print(f"\nMARKET-MAKING STUDY: {len(rows)} synthetic simulations")
    print(f"Audit passed: {len(rows)} summaries, {checked} example fill ledgers")
    print("PnL is in tick-lots; inventory variance is in lots squared.\n")
    print(f"{'Regime':<11} {'Strategy':<11} {'Seeds':>5} {'Mean PnL':>12} {'Inv. var.':>12} {'Max DD':>12}")
    for regime in REGIMES:
        for strategy in STRATEGIES:
            group = [r for r in rows if r["group"] == "comparison"
                     and r["regime"] == regime and r["strategy"] == strategy]
            if not group:
                raise ValueError(f"Missing {regime}/{strategy} results")
            print(f"{regime:<11} {strategy:<11} {len(group):>5} "
                  f"{statistics.mean(r['net_pnl'] for r in group):>12.2f} "
                  f"{statistics.mean(r['inventory_variance'] for r in group):>12.2f} "
                  f"{statistics.mean(r['max_drawdown'] for r in group):>12.2f}")

    print("\nPAIRED COMPARISONS AGAINST FIXED (95% bootstrap intervals)")
    for regime in REGIMES:
        baseline = {r["seed"]: r for r in rows if r["group"] == "comparison"
                    and r["regime"] == regime and r["strategy"] == "fixed"}
        for strategy in STRATEGIES[1:]:
            compared = [r for r in rows if r["group"] == "comparison"
                        and r["regime"] == regime and r["strategy"] == strategy]
            pnl = interval([r["net_pnl"] - baseline[r["seed"]]["net_pnl"] for r in compared])
            var = interval([r["inventory_variance"] - baseline[r["seed"]]["inventory_variance"]
                            for r in compared])
            print(f"{regime:<9} {strategy:<9} "
                  f"PnL {pnl[0]:+.2f} [{pnl[1]:+.2f}, {pnl[2]:+.2f}]; "
                  f"Inv.var {var[0]:+.2f} [{var[1]:+.2f}, {var[2]:+.2f}]")

    normal = {strategy: [r for r in rows if r["group"] == "comparison"
                         and r["regime"] == "normal" and r["strategy"] == strategy]
              for strategy in ("fixed", "inventory")}
    fixed_var = statistics.mean(r["inventory_variance"] for r in normal["fixed"])
    skew_var = statistics.mean(r["inventory_variance"] for r in normal["inventory"])
    fixed_pnl = statistics.mean(r["net_pnl"] for r in normal["fixed"])
    skew_pnl = statistics.mean(r["net_pnl"] for r in normal["inventory"])
    if fixed_var != 0 and fixed_pnl != 0:
        print(f"\nNORMAL MARKET: inventory variance change "
              f"{100 * (skew_var / fixed_var - 1):+.1f}%; "
              f"mean net PnL change {100 * (skew_pnl / fixed_pnl - 1):+.1f}%")
    print("\nThese are synthetic simulation results, not live trading performance.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "results" / "study")
    parser.add_argument("--seeds", type=int, default=12)
    parser.add_argument("--seconds", type=float, default=120)
    parser.add_argument("--dt", type=float, default=0.1)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--analyze-only", "--report-only", action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()

    if not args.analyze_only:
        binary = ROOT / "bin" / ("market_making.exe" if os.name == "nt" else "market_making")
        if not binary.exists():
            raise SystemExit("Build the C++ simulator first: .\\build.ps1 -Target sim-build")
        env = os.environ.copy()
        if os.name == "nt" and Path("C:/msys64/ucrt64/bin").exists():
            env["PATH"] = "C:/msys64/ucrt64/bin" + os.pathsep + env.get("PATH", "")
        subprocess.run([str(binary), "--study", "--seeds", str(args.seeds),
                        "--seconds", str(args.seconds), "--dt", str(args.dt),
                        "--seed", str(args.seed), "--output", str(output)],
                       check=True, env=env, cwd=ROOT)

    rows = read_csv(output / "runs.csv")
    checked = audit(output, rows)
    print_results(rows, checked)


if __name__ == "__main__":
    main()
