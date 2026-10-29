"""Run the C++ experiment grid, audit its CSVs, and produce a readable report.
Python standard library handles experiments/statistics; matplotlib is needed for charts.
"""
from __future__ import annotations
import argparse
import base64
import csv
import hashlib
import html
import json
import math
import os
from pathlib import Path
import random
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
STRATEGIES = ("fixed", "inventory", "as")
REGIMES = ("quiet", "normal", "volatile", "informed")
LABELS = {"fixed": "Fixed spread", "inventory": "Inventory skew", "as": "A-S inspired"}
COLORS = {"fixed": "#64748b", "inventory": "#0891b2", "as": "#d97706"}


def read_csv(path):
    with Path(path).open(newline="", encoding="utf-8-sig") as f:
        rows = list(csv.DictReader(f))
    for row in rows:
        for key, value in row.items():
            if key not in ("group", "regime", "strategy"):
                row[key] = float(value)
                if not math.isfinite(row[key]):
                    raise ValueError(f"Nonfinite {key} in {path}")
    return rows


def interval(values, seed=42):
    """Percentile bootstrap CI of the mean over independent simulation seeds."""
    if not values:
        raise ValueError("Empty sample")
    mean = statistics.mean(values)
    if len(values) == 1:
        return mean, mean, mean
    rng = random.Random(seed)
    means = sorted(statistics.mean(rng.choices(values, k=len(values))) for _ in range(3000))
    return mean, means[74], means[2924]


def close(a, b):
    return math.isclose(a, b, rel_tol=1e-8, abs_tol=1e-5)


def audit(directory, rows):
    """Independently reconstruct the example ledgers; fail instead of hiding errors."""
    if not rows:
        raise ValueError("No runs")
    keys = [(r["group"], r["regime"], r["strategy"], r["seed"],
             r["half_spread"], r["quote_size"]) for r in rows]
    if len(keys) != len(set(keys)):
        raise ValueError("Duplicate experiment keys")
    for r in rows:
        if r["final_inventory"] != 0 or r["max_abs_inventory"] > r["inventory_limit"]:
            raise ValueError("Inventory invariant failed")
        if not 0 <= r["fill_rate"] <= 1:
            raise ValueError("Invalid fill fraction")
        if not close(r["pre_liquidation_pnl"] - r["liquidation_cost"], r["net_pnl"]):
            raise ValueError("Liquidation accounting failed")
        if r["markout_volume"] + r["censored_markout_volume"] != r["passive_volume"]:
            raise ValueError("Markout coverage failed")
    checked = 0
    for trajectory in directory.glob("examples/*/*/trajectory.csv"):
        regime, strategy = trajectory.parent.parent.name, trajectory.parent.name
        candidates = [r for r in rows if r["group"] == "comparison"
                      and r["regime"] == regime and r["strategy"] == strategy]
        expected = min(candidates, key=lambda r: r["seed"])
        fills = read_csv(trajectory.with_name("fills.csv"))
        points = read_csv(trajectory)
        cash, inventory, fees, passive = 0.0, 0, 0.0, 0
        for f in fills:
            cash -= f["direction"] * f["quantity"] * f["price"] + f["fee"]
            inventory += int(f["direction"] * f["quantity"])
            fees += f["fee"]
            if not f["liquidation"]:
                passive += f["quantity"]
            if abs(inventory) > expected["inventory_limit"]:
                raise ValueError("Ledger inventory limit breach")
        if inventory or not close(cash, expected["net_pnl"]) or not close(fees, expected["fees"]):
            raise ValueError("CSV fill-ledger reconciliation failed")
        if passive != expected["passive_volume"]:
            raise ValueError("CSV volume reconciliation failed")
        for p in points:
            if not close(p["cash"] + p["inventory"] * p["reference"], p["equity"]):
                raise ValueError("Snapshot equity reconciliation failed")
        if not close(points[-1]["equity"], cash):
            raise ValueError("Final snapshot differs from ledger")
        checked += 1
    return checked


def paired_comparisons(rows):
    output = []
    metrics = ("net_pnl", "inventory_variance", "mean_abs_inventory",
               "max_drawdown", "fill_rate", "adverse_selection")
    for regime in REGIMES:
        baseline = {r["seed"]: r for r in rows if r["group"] == "comparison"
                    and r["regime"] == regime and r["strategy"] == "fixed"}
        for strategy in STRATEGIES[1:]:
            challengers = [r for r in rows if r["group"] == "comparison"
                           and r["regime"] == regime and r["strategy"] == strategy]
            for metric in metrics:
                differences = [r[metric] - baseline[r["seed"]][metric] for r in challengers]
                mean, lower, upper = interval(differences)
                output.append(dict(regime=regime, strategy=strategy, metric=metric,
                                   seeds=len(differences), mean_difference=mean,
                                   ci95_low=lower, ci95_high=upper))
    return output


def write_csv(path, rows):
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def charts(directory, rows):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError as exc:
        raise SystemExit("Install plotting dependency: python -m pip install -r simulation/requirements.txt") from exc
    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 10,
                         "axes.spines.top": False, "axes.spines.right": False})
    output = []
    fig, axes = plt.subplots(2, 2, figsize=(12, 8), layout="constrained")
    metrics = [("net_pnl", "Net PnL after liquidation (tick-lots)"),
               ("inventory_variance", "Sampled inventory variance (lots squared)"),
               ("max_drawdown", "Sampled max drawdown (tick-lots)"),
               ("adverse_selection", "1-second adverse price movement (ticks/lot)")]
    for ax, (metric, title) in zip(axes.flat, metrics):
        for j, strategy in enumerate(STRATEGIES):
            values = []
            for regime in REGIMES:
                group = [r[metric] for r in rows if r["group"] == "comparison"
                         and r["regime"] == regime and r["strategy"] == strategy]
                values.append(interval(group))
            x = [i + (j - 1) * 0.22 for i in range(4)]
            means = [v[0] for v in values]
            ax.errorbar(x, means, yerr=[[max(0, v[0]-v[1]) for v in values],
                                     [max(0, v[2]-v[0]) for v in values]],
                        fmt="o", capsize=3, color=COLORS[strategy], label=LABELS[strategy])
        ax.set_xticks(range(4), REGIMES)
        ax.set_title(title)
        ax.axhline(0, color="#cbd5e1", linewidth=0.7)
        ax.grid(axis="y", alpha=0.18)
    axes[0, 0].legend(fontsize=8)
    fig.suptitle("Synthetic market comparison | means and bootstrap 95% intervals", fontsize=14)
    path = directory / "comparison.png"
    fig.savefig(path, dpi=150); plt.close(fig); output.append(path)

    fig, axes = plt.subplots(2, 2, figsize=(12, 7), layout="constrained")
    for column, regime in enumerate(("volatile", "informed")):
        for strategy in STRATEGIES:
            points = read_csv(directory / "examples" / regime / strategy / "trajectory.csv")
            times = [p["time"] for p in points]
            for row, metric in enumerate(("equity", "inventory")):
                axes[row, column].plot(times, [p[metric] for p in points],
                                      label=LABELS[strategy], color=COLORS[strategy], linewidth=1)
                axes[row, column].grid(alpha=0.18)
        axes[0, column].set_title(regime.capitalize())
        axes[1, column].set_xlabel("Simulated time (seconds)")
    axes[0, 0].set_ylabel("Equity (tick-lots)")
    axes[1, 0].set_ylabel("Inventory (lots)")
    axes[0, 0].legend(fontsize=8)
    fig.suptitle("One example seed | illustration, not the statistical conclusion", fontsize=14)
    path = directory / "example_paths.png"
    fig.savefig(path, dpi=150); plt.close(fig); output.append(path)

    for regime in ("volatile", "informed"):
        fig, axes = plt.subplots(2, 3, figsize=(12, 7), layout="constrained")
        for row, strategy in enumerate(("fixed", "inventory")):
            for column, (metric, label) in enumerate(
                (("net_pnl", "Net PnL (tick-lots)"), ("fill_rate", "Filled / posted volume"),
                 ("inventory_variance", "Inventory variance"))):
                grid = [[statistics.mean(r[metric] for r in rows if r["group"] == "sweep"
                          and r["regime"] == regime and r["strategy"] == strategy
                          and r["quote_size"] == size and r["half_spread"] == width)
                         for width in (1, 2, 4)] for size in (2, 5, 10)]
                ax = axes[row, column]
                scale_values = [r[metric] for r in rows if r["group"] == "sweep" and r["regime"] == regime]
                # One scale across both strategy rows for each metric.
                ax.imshow(grid, cmap="YlGnBu", aspect="auto",
                          vmin=min(scale_values), vmax=max(scale_values))
                for y in range(3):
                    for x in range(3):
                        value = f"{100*grid[y][x]:.1f}%" if metric == "fill_rate" else f"{grid[y][x]:.1f}"
                        ax.text(x, y, value, ha="center", va="center", fontsize=10,
                                bbox=dict(facecolor="white", alpha=0.8, edgecolor="none", pad=2))
                ax.set_xticks(range(3), (1, 2, 4)); ax.set_yticks(range(3), (2, 5, 10))
                ax.set_xlabel("Half-spread (ticks)"); ax.set_ylabel("Quote size (lots)")
                ax.set_title(f"{LABELS[strategy]} | {label}", fontsize=10)
        fig.suptitle(f"{regime.capitalize()} | width and size sweep (seed averages)", fontsize=14)
        path = directory / f"sweep_{regime}.png"
        fig.savefig(path, dpi=150); plt.close(fig); output.append(path)
    return output


def generate_report(directory):
    rows = read_csv(directory / "runs.csv")
    if len({r["seed"] for r in rows if r["group"] == "comparison"}) < 2:
        raise ValueError("A statistical report requires at least two simulation seeds")
    if not any(r["group"] == "comparison" for r in rows):
        raise ValueError("Report expects --study output")
    checked = audit(directory, rows)
    paired = paired_comparisons(rows)
    write_csv(directory / "paired_comparisons.csv", paired)
    metadata = json.loads((directory / "metadata.json").read_text())
    table = []
    for regime in REGIMES:
        for strategy in STRATEGIES:
            group = [r for r in rows if r["group"] == "comparison"
                     and r["regime"] == regime and r["strategy"] == strategy]
            mean, lo, hi = interval([r["net_pnl"] for r in group])
            table.append(dict(regime=regime, strategy=strategy, seeds=len(group), mean_pnl=mean,
                              pnl_ci95_low=lo, pnl_ci95_high=hi,
                              mean_inventory_variance=statistics.mean(r["inventory_variance"] for r in group),
                              mean_drawdown=statistics.mean(r["max_drawdown"] for r in group),
                              mean_fill_rate=statistics.mean(r["fill_rate"] for r in group),
                              mean_adverse_selection=statistics.mean(r["adverse_selection"] for r in group)))
    write_csv(directory / "aggregate.csv", table)
    images = charts(directory, rows)
    findings = []
    for regime in REGIMES:
        differences = {r["metric"]: r for r in paired
                       if r["regime"] == regime and r["strategy"] == "inventory"}
        p, v = differences["net_pnl"], differences["inventory_variance"]
        findings.append(f"{regime.capitalize()}: inventory skew minus fixed spread changed mean net PnL by "
                        f"{p['mean_difference']:+.1f} tick-lots (paired 95% interval "
                        f"{p['ci95_low']:+.1f} to {p['ci95_high']:+.1f}) and inventory variance by "
                        f"{v['mean_difference']:+.1f} lots squared.")
    md = ["# Market-making experiment results", "",
          f"{len(rows)} synthetic runs; {metadata['seeds']} seeds per configuration; "
          f"{metadata['seconds']:g} seconds per run; dt={metadata['dt']:g} seconds.",
          "", "## Measured findings", ""] + ["- " + s for s in findings]
    md += ["", "## Interpretation", "",
           "These are outcomes in a deliberately simplified synthetic model, not a real-market backtest.",
           "Higher profit is not guaranteed for inventory-aware strategies. Report both benefit and cost.",
           "Intervals measure Monte Carlo uncertainty across seeds, not uncertainty about market realism.",
           "Parameter sweeps are exploratory. Validate a chosen configuration on fresh seeds before making a selection claim.",
           "The A-S-inspired policy uses the formula but not the original paper's fill process.",
           "", "## Validation", "",
           f"All {len(rows)} run summaries passed inventory, liquidation and markout-coverage checks.",
           f"Independently reconstructed cash, fees and inventory from {checked} example fill ledgers.",
           "", "See paired_comparisons.csv for paired effects; aggregate.csv for configuration means.",
           "See docs/STUDY_GUIDE.md for the event timeline, formulae, assumptions and exercises."]
    (directory / "RESULTS.md").write_text("\n".join(md) + "\n", encoding="utf-8")
    table_html = "<table><thead><tr><th>Regime / strategy</th><th>Mean net PnL<br>95% interval</th>" \
                 "<th>Inventory variance</th><th>Max drawdown</th><th>Fill fraction</th></tr></thead><tbody>"
    for r in table:
        table_html += (f"<tr><td>{r['regime']} / {LABELS[r['strategy']]}</td>"
                       f"<td>{r['mean_pnl']:.1f}<small>{r['pnl_ci95_low']:.1f} to {r['pnl_ci95_high']:.1f}</small></td>"
                       f"<td>{r['mean_inventory_variance']:.1f}</td><td>{r['mean_drawdown']:.1f}</td>"
                       f"<td>{100*r['mean_fill_rate']:.2f}%</td></tr>")
    table_html += "</tbody></table>"
    image_html = "".join(f'<figure><img alt="{html.escape(p.stem)}" src="data:image/png;base64,'
                         f'{base64.b64encode(p.read_bytes()).decode()}"></figure>' for p in images)
    doc = f"""<!doctype html><html lang="en"><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>Market-making research report</title>
<style>
body{{font:16px/1.65 system-ui,sans-serif;color:#183044;background:#f4f7fa;margin:0}}
main{{max-width:1080px;margin:auto;padding:36px 24px}} h1{{font-size:36px;line-height:1.2}}
h2{{margin-top:38px}} .eyebrow{{color:#087f8c;font-weight:700;letter-spacing:.1em;font-size:12px}}
.card,figure{{background:white;border:1px solid #dbe4eb;border-radius:12px;padding:20px;margin:20px 0}}
.note{{border-left:4px solid #d97706;padding:12px 20px;background:#fff6e5}}
table{{border-collapse:collapse;width:100%;font-size:14px}} td,th{{padding:12px 8px;text-align:left;border-bottom:1px solid #e2e8f0}}
small{{display:block;color:#64748b}} img{{width:100%;height:auto}} code{{background:#e6edf3;padding:2px 5px}}
@media(max-width:650px){{main{{padding:20px 12px}} table{{font-size:11px}}td,th{{padding:7px 3px}}h1{{font-size:28px}}}}
</style><main><div class="eyebrow">MATCHING ENGINE / QUANTITATIVE RESEARCH</div>
<h1>When does inventory-aware quoting help?</h1>
<p>{len(rows)} reproducible synthetic runs &middot; {metadata['seeds']} seeds per configuration
&middot; {metadata['seconds']:g} simulated seconds per run</p>
<div class="note"><strong>Simulation study.</strong> These results demonstrate implementation and research methods.
They are not evidence of live-market profitability. PnL is measured in tick-lots, not dollars.</div>
<h2>What was compared</h2><p>Fixed symmetric quotes, linear inventory skew, and an Avellaneda&ndash;Stoikov-inspired
policy. Each strategy sees identical external price shocks and incoming order requests for a given seed.
Every fill is produced by the original C++ matching engine with FIFO priority.</p>
<div class="card">{table_html}</div>
<h2>Measured trade-offs</h2><ul>{''.join('<li>'+html.escape(s)+'</li>' for s in findings)}</ul>
<p>Paired intervals compare strategies on the same seed. They describe simulation sampling uncertainty;
they do not account for an incorrect market model. The sweeps are exploratory, not out-of-sample optimization.</p>
{image_html}
<h2>How to read the results</h2>
<ul><li><strong>Net PnL:</strong> cash after fees and forced end-of-run liquidation through background depth.</li>
<li><strong>Inventory variance:</strong> variance of inventory sampled on the decision grid; lower need not mean more profit.</li>
<li><strong>Fill fraction:</strong> filled strategy volume divided by all submitted quote volume.
Canceled/reposted lots count again; this is not the probability that a market order executes.</li>
<li><strong>Adverse price movement:</strong> negative of the signed reference-price change one second after a passive fill.
Positive is unfavorable. It is separate from spread capture and is not causal attribution.</li>
<li><strong>Sharpe:</strong> runs.csv includes an unannualized mean/std ratio of fixed-capital grid returns,
including liquidation. Do not interpret it as an annual or live trading Sharpe.</li></ul>
<h2>What this model assumes</h2>
<p>Poisson order counts on a fixed time grid; geometric order sizes; exponentially distributed price tolerance;
arithmetic Brownian external reference; a background liquidity ladder rebuilt each step. Competitor orders
enter before our quotes. Prices and inventory are synthetic. The informed-flow scenario correlates incoming
order direction with the next price innovation, which is hidden from our quoting policy.</p>
<p>The reference price is observable and exogenous. There is no market impact, realistic latency, exchange calibration,
historical data or adaptive competitor. Quote replacement loses priority. The A-S formula is a heuristic in this
different fill environment. End liquidation uses refreshed modeled depth and incurs fees and spread/depth costs.</p>
<h2>Validation</h2><p>{len(rows)} summaries passed invariant checks. {checked} example fill ledgers were independently
reconciled to final cash, fees and inventory. C++ tests cover FIFO, partial fills, IOC expiry, inventory caps,
quote direction, deterministic replay, liquidation and signed markouts.</p>
<h2>Study next</h2><p>Read <code>docs/STUDY_GUIDE.md</code>, then follow one buy and one sell through
<code>fills.csv</code>. Change one assumption at a time. Explain an observed cost of inventory control before
putting performance claims on your resume.</p></main></html>"""
    (directory / "report.html").write_text(doc, encoding="utf-8")
    print("\n".join(findings))
    print(f"Report: {directory / 'report.html'}")
    return rows


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output", type=Path, default=ROOT / "results" / "study")
    p.add_argument("--seeds", type=int, default=12)
    p.add_argument("--seconds", type=float, default=120)
    p.add_argument("--dt", type=float, default=0.1)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--report-only", action="store_true")
    args = p.parse_args()
    output = args.output.resolve()
    if not args.report_only:
        binary = ROOT / "bin" / ("market_making.exe" if os.name == "nt" else "market_making")
        if not binary.exists():
            raise SystemExit("Build first: .\\build.ps1 -Target sim-build")
        env = os.environ.copy()
        if os.name == "nt" and Path("C:/msys64/ucrt64/bin").exists():
            env["PATH"] = "C:/msys64/ucrt64/bin" + os.pathsep + env.get("PATH", "")
        command = [str(binary), "--study", "--seeds", str(args.seeds), "--seconds", str(args.seconds),
                   "--dt", str(args.dt), "--seed", str(args.seed), "--output", str(output)]
        subprocess.run(command, check=True, env=env, cwd=ROOT)
        metadata_path = output / "metadata.json"
        data = json.loads(metadata_path.read_text())
        data["command"] = command
        data["binary_sha256"] = hashlib.sha256(binary.read_bytes()).hexdigest()
        data["source_sha256"] = {
            str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
            for folder, pattern in (("src", "*.cpp"), ("include", "*.hpp"), ("simulation", "*.cpp"))
            for path in (ROOT / folder).rglob(pattern)}
        metadata_path.write_text(json.dumps(data, indent=2) + "\n")
    generate_report(output)


if __name__ == "__main__":
    main()
