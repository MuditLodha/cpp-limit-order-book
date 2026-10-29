# Limit Order Book & Matching Engine

A C++ limit order book with price-time priority matching, an order-book benchmark, and a market-making simulator built on top of the same engine.

The matching engine handles order execution and maintains the book. The simulator uses it to test how different quoting strategies affect inventory, fills and PnL.

## Matching engine

- Limit and market orders, partial fills, multi-level sweeps, cancellations and modifications.
- Price-time priority: better prices execute first; orders at the same price execute FIFO.
- Array-indexed price levels with bitmaps for finding the next non-empty bid or ask.
- Intrusive doubly-linked lists for order queues and a pre-allocated memory pool.
- Average O(1) order-ID lookup using a hash table.
- Quantity reductions keep queue priority; price changes and quantity increases lose it.

The engine is single-threaded and uses integer prices (ticks). See [MatchingEngine.hpp](include/matching_engine/MatchingEngine.hpp) and [OrderBook.hpp](include/matching_engine/OrderBook.hpp).

## Market-making simulator

The simulator places quotes through the matching engine and processes fills using the same FIFO matching logic.

Three quoting strategies are implemented:

- **Fixed spread:** posts bid and ask quotes around the reference price.
- **Inventory skew:** moves quotes based on the current position to reduce inventory exposure.
- **Avellaneda–Stoikov inspired:** adjusts quotes using inventory, volatility and time horizon.

The simulated market includes Poisson order arrivals, reference-price movements, background liquidity and informed order flow. It also models maker/taker fees, inventory limits and liquidation at the end of each run.

All strategies are compared on seeded market scenarios. This is a synthetic simulation, not a historical-data backtest.

## Results

The study runs **576 simulations** across quiet, normal, volatile and informed-flow markets, including spread and quote-size sweeps.

For the normal-volatility scenario:

| Metric | Fixed spread | Inventory skew |
| --- | ---: | ---: |
| Mean net PnL (tick-lots) | 4,518.84 | 3,959.60 |
| Mean inventory variance | 495.63 | 39.32 |
| Mean max drawdown (tick-lots) | 339.00 | 34.70 |

Inventory skew reduced mean inventory variance by about **92%**, with about **12% lower net PnL** in this scenario. Results depend on the simulator's assumptions and parameters.

The Python analysis generates per-run metrics, paired bootstrap intervals, CSV summaries and an HTML report.

## Build and run

Requirements: Windows PowerShell, g++ (MSYS2 UCRT64), Python 3 and matplotlib for the study report.

From the repository root:

~~~powershell
# Build and launch the matching engine
.\build.ps1 -Target engine

# Run a short demo
.\build.ps1 -Target demo

# Run engine unit tests and differential tests
.\build.ps1 -Target test
.\build.ps1 -Target diff

# Run benchmarks
.\build.ps1 -Target bench
~~~

The default differential test runs 25,000 operations. To run 100,000:

~~~powershell
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
.\bin\diff_test.exe 100000 42
~~~

### Simulation

~~~powershell
# Install the plotting dependency
python -m pip install -r simulation/requirements.txt

# Run one simulation
.\run-simulation.ps1 -Mode single -Strategy inventory -Seed 1

# Run simulator and Python tests
.\run-simulation.ps1 -Mode test

# Run the full 576-run study
.\run-simulation.ps1 -Mode study
~~~

Study results are written to `results/study/`, including `runs.csv`, `aggregate.csv`, `paired_comparisons.csv` and `report.html`. The generated results directory is gitignored.

## Testing

- **8 unit-test groups** covering the memory pool, FIFO matching, order modifications, cancellations, rejection cases and sparse-book scanning.
- **100,000 randomized operations** compared against an independent `std::map` reference engine, with matching results.
- **8 market-making test groups** covering quoting, fees, inventory limits, liquidation, markouts and reproducibility.
- **3 Python tests** covering CLI inputs, output consistency and ledger checks.

## Project structure

~~~text
include/matching_engine/   Order book, matching engine and data structures
include/market_making/     Simulator configuration and interfaces
src/                       C++ implementations
simulation/                Simulation CLI and Python analysis
benchmarks/                Throughput and latency benchmarks
tests/                     Unit, differential and simulation tests
docs/                      Implementation notes and study guide
build.ps1                  Engine build and test script
run-simulation.ps1         Simulation runner
~~~

For implementation details and the simulation model, see [IMPLEMENTATION.md](docs/IMPLEMENTATION.md) and [STUDY_GUIDE.md](docs/STUDY_GUIDE.md).
