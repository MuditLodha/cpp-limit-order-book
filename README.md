# Limit Order Book & Market-Making Simulator

**C++20 matching engine | Synthetic market-making research | Python statistical analysis**

A single-threaded, price-time-priority limit order book (LOB) in C++20, extended with a market-making simulator that submits orders to the *actual matching engine*. The project studies two connected problems: **efficient and correct order matching**, and **how inventory-aware quoting affects risk and profit** in a controlled synthetic market.

> **Research scope:** This is an educational synthetic simulation, **not** historical-data backtesting or live trading. PnL values use **tick-lots**, not dollars or rupees. Local performance measurements are workload- and hardware-dependent.

## Highlights

- **Matching engine:** limit/market orders, cancel/modify, partial fills, multi-level sweeps, FIFO within each price level, and integer-tick prices.
- **Data structures:** direct array-indexed price levels, intrusive doubly-linked FIFO queues, pooled orders, average O(1) order-ID hash lookup, and occupancy-bitset scanning to locate nonempty levels.
- **Market-making strategies:** fixed-spread, inventory-skew, and Avellaneda–Stoikov-inspired quoting with inventory caps, modeled fees, and terminal liquidation.
- **Synthetic market:** seeded Brownian reference-price shocks, Poisson arrivals on a fixed time grid, background liquidity, and informed-flow stress tests.
- **Research outputs:** fill ledgers, inventory/equity paths, net PnL, drawdown, adverse-selection markouts, bootstrap confidence intervals, and HTML plots.

## Architecture

```text
Synthetic price changes + incoming order flow (seeded)
                        |
               Quoting strategy
           fixed / inventory / A-S
                        |
         C++ MatchingEngine and OrderBook
      FIFO fills / order lifecycle / BBO
                        |
             Fill & accounting ledger
           cash / inventory / fees
                        |
          Python audits and comparisons
           CSV / charts / HTML report
```

### Matching engine

The core engine handles order placement, cancellation, market orders, and modifications. Orders at better prices execute first; at the same price, resting orders execute FIFO. Decreasing an order's remaining quantity keeps its existing priority, while increasing quantity or changing price forfeits priority. Invalid modifications are rejected without deleting the existing resting order.

**Complexity:** accessing a configured price level is O(1); intrusive-queue unlinking is O(1); order-ID hash lookups are *average* O(1). Finding the next nonempty best price can require bitmap scanning, so not every operation is worst-case O(1). The single-threaded engine uses a fixed configured price ladder and order pool.

### Market-making simulator

The simulation constructs synthetic external events before comparing strategies. For a given seed and scenario, policies receive the same underlying price innovations and incoming-order tape, although their actual fills differ with their quotes. Background competitors enter before strategy quotes at the same price, preserving FIFO priority. Incoming marketable limit orders have unfilled remainders canceled (IOC-like behavior).

Each policy adjusts bid/ask quotes on a fixed decision grid:

| Policy | Quoting rule |
| --- | --- |
| Fixed spread | Symmetric prices around the observable reference |
| Inventory skew | Shift both quotes away from accumulating more of the current position |
| A-S inspired | Reservation-price and spread adjustments using inventory, volatility, and a bounded risk horizon |

The A-S-inspired formula is **not** calibrated to the simulation's actual arrival/fill law and is not claimed optimal.

Risk controls include per-run inventory limits, maker/taker fees, and forced terminal liquidation against modeled depth. The model has no exchange connectivity, historical market calibration, realistic latency, market impact, or adaptive competitor.

## Build and run

**Requirements:** Windows PowerShell, a **C++20-capable GCC** (the scripts prefer `C:\msys64\ucrt64\bin\g++.exe`), and Python 3 for Python tests and reports. Install `matplotlib` for the study plots:

```powershell
python -m pip install -r simulation/requirements.txt
```

From the repository root:

```powershell
# C++ engine CLI, guided demo and original tests
.\build.ps1 -Target engine
.\build.ps1 -Target demo
.\build.ps1 -Target test
.\build.ps1 -Target diff
.\build.ps1 -Target bench

# Simulator and its tests
.\run-simulation.ps1 -Mode single -Strategy inventory -Seconds 120 -Seed 1
.\run-simulation.ps1 -Mode test
.\run-simulation.ps1 -Mode study -Seeds 12 -Seconds 120
```

If running from **Windows CMD** instead of PowerShell, use:

```cmd
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Target test
powershell -NoProfile -ExecutionPolicy Bypass -File .\run-simulation.ps1 -Mode test
```

The `build.ps1 -Target diff` target runs **25,000 operations with seed 42**. For a larger differential test, compile the binary first and run it explicitly:

```powershell
.\build.ps1 -Target diff
.\bin\diff_test.exe 100000 42
```

This 100,000-operation command is an **optional additional test**, not a claim that it was run in the latest supplied logs.

The study saves outputs under the locally generated `results/study/` folder, including `runs.csv`, `paired_comparisons.csv`, `aggregate.csv`, `RESULTS.md`, images, and `report.html`. Generated results are ignored by Git and **not included in the repository**; run the study to view them. The report can be regenerated from existing outputs:

```powershell
python simulation/run_study.py --report-only --output results/study
```

For fresh-seed validation after selecting a hypothesis:

```powershell
python simulation/run_study.py --seed 1001 --seeds 20 --output results/validation
```

See [Study Guide](docs/STUDY_GUIDE.md) for the model, parameters, event sequence, accounting, and interpretation.

## Tests and validation

| Test | Coverage | Latest user-run result (Oct 29, 2026) |
| --- | --- | --- |
| [C++ unit tests](tests/UnitTests.cpp) | Memory pool, intrusive list, FIFO, sweeps, cancellation, modification safety, rejects, sparse bitset | **8/8 groups passed** |
| [Differential test](tests/DifferentialTest.cpp) | Optimized engine vs. independent `std::map` reference; invalid inputs and deep-book state | **25,000 operations passed (seed 42)** |
| [Market-making tests](tests/MarketMakingTests.cpp) | Quote policies, ledger accounting, FIFO/IOC, caps/liquidation, markouts, determinism, edge cases | **8/8 groups passed** |
| [Python tests](tests/test_simulation.py) | CLI validation, deterministic files, ledger reconciliation, study-shape and audit-tampering checks | **3 tests passed** |
| [Full study](simulation/run_study.py) | Synthetic comparisons and sensitivity runs | **576/576 simulations completed** |

These results were supplied from a local Windows run; this README change did **not** independently execute the C++ binaries. Passing tests supports correctness on covered cases, not absence of all bugs.

## Experiment design and findings

The default study executes **576 synthetic runs**:

- **144 comparisons:** 4 regimes (quiet, normal, volatile, informed) × 3 strategies × 12 seeds.
- **432 parameter sweeps:** 2 regimes (volatile, informed) × 2 policies (fixed, inventory skew) × 3 quote half-spreads × 3 quote sizes × 12 seeds.
- Default run length: **120 simulated seconds**; time step **0.1 second**.
- Paired strategy comparisons use the same seed, with **3,000 bootstrap resamples** for approximate 95% intervals.

The [study guide](docs/STUDY_GUIDE.md) records these **normal-volatility scenario** findings for the default configuration:

| Metric | Fixed spread | Inventory skew |
| --- | ---: | ---: |
| Mean net PnL (tick-lots) | 4,518.84 | 3,959.60 |
| Mean sampled inventory variance (lots²) | 495.63 | 39.32 |
| Mean sampled max drawdown (tick-lots) | 339.00 | 34.70 |

Inventory skew reduced mean sampled inventory variance by approximately **92.1%**, with approximately **12.4% lower mean net PnL**. The paired mean PnL difference was **−559.24 tick-lots**, with an approximate bootstrap 95% interval of **−873.3 to −235.7 tick-lots**. These are documented outputs from the **synthetic study**, not independently verified market results. Confidence intervals represent variability across synthetic seeds, **not uncertainty about model realism**.

Fill ledgers are reconciled against cash, fees, inventory and final liquidation. Reported adverse selection uses a one-second signed reference-price markout; end-horizon marks are censored and counted separately. The ratio called `step_sharpe` is **not annualized** and is not a live trading Sharpe.

## Benchmarking

The [C++ benchmark](benchmarks/BenchmarkEngine.cpp) includes passive insertion, aggressive matching, live-order cancellation, and mixed-workload throughput with cycle-based timing. Prior local measurements were **over 10 million operations per second** for a mixed workload, but they were not rerun as part of this documentation update. Treat the results as machine-specific; record the CPU, compiler, build flags, workload, P50/P99/P99.9, and repeated runs before making performance comparisons.

## Repository map

```text
benchmarks/                Timing utilities and matching-engine benchmark
docs/                      Implementation notes and detailed study guide
include/matching_engine/   Orders, price ladder, queues, memory pool, engine
include/market_making/     Simulator interface and configuration
src/                       Engine implementations, simulator, CLI
simulation/                Simulation CLI, Python study, requirements
tests/                     Engine, differential, simulator and Python tests
build.ps1                  C++ build, test, demo, benchmark targets
run-simulation.ps1         Single run, simulator tests and full study
```

## Study and source files

- [Engine interface](include/matching_engine/MatchingEngine.hpp) and [order book](include/matching_engine/OrderBook.hpp)
- [Simulator configuration](include/market_making/Simulator.hpp) and [implementation](src/MarketMaking.cpp)
- [Experiment runner and analysis](simulation/run_study.py)
- [Detailed study guide](docs/STUDY_GUIDE.md)
- [Implementation and validation notes](docs/IMPLEMENTATION.md)

---

**Limitations:** simplified synthetic flows, exogenous known reference, discrete simulation grid, priority-reset on quote replacement, simplified background liquidity, no impact or latency calibration, and no claim of profitable live execution.
