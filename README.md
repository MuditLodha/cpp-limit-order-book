# Limit Order Book and Matching Engine (C++14)

A single-threaded C++ simulation of a limit order book. It accepts buy and sell orders, matches compatible orders using price-time priority, and keeps track of orders that have not traded yet.

This is a learning and portfolio project for C++ systems programming and trading infrastructure. It is **not** a trading strategy and does not predict prices or demonstrate profitability.

## The simple way to use it

There is no `build.bat`, `build.ps1`, or CMake. You compile the project once from the command line, then pass a mode or trading command to the executable through `argc` / `argv`.

### 1. Compile once

Install GCC (`g++`) and open a terminal in the project folder. In Windows Command Prompt, run:

```cmd
if not exist bin mkdir bin
g++ -std=c++14 -O2 -Wall -Wextra -Iinclude -Ibenchmarks src/main.cpp src/OrderBook.cpp src/MatchingEngine.cpp src/ReferenceEngine.cpp tests/UnitTests.cpp tests/DifferentialTest.cpp benchmarks/BenchmarkEngine.cpp -o bin\lob.exe
```

In PowerShell, use the same compile command. Run the generated program using `.\bin\lob.exe` (PowerShell) or `bin\lob.exe` (Command Prompt).

### 2. Run a mode

| Command Prompt command | What it does |
|---|---|
| `bin\lob.exe` | Starts interactive trading mode |
| `bin\lob.exe demo` | Runs the guided demo |
| `bin\lob.exe test` | Runs all unit tests |
| `bin\lob.exe diff` | Runs 25,000 randomized differential operations with seed 42 |
| `bin\lob.exe diff 100000 42` | Runs 100,000 differential operations with seed 42 |
| `bin\lob.exe bench` | Runs performance benchmarks |
| `bin\lob.exe help` | Shows the command list |

In PowerShell, prefix each run command with `.\`, e.g. `.\bin\lob.exe test`.

### 3. Pass trading commands directly

You can pass a single trading action as command-line arguments, similar to `int main(int argc, char* argv[])`:

```cmd
bin\lob.exe limit buy 100 50 101
bin\lob.exe limit sell 105 25 201
bin\lob.exe market buy 30 301
bin\lob.exe book 10
bin\lob.exe cancel 101
bin\lob.exe modify 201 104 20
```

Each direct trading command starts a fresh engine process, so its book is not retained for the next command. To run several trading commands against the same book, start interactive mode with `bin\lob.exe` and enter commands at the `lob>` prompt.

## What it does

- Limit orders can trade immediately or rest in the book.
- Market orders consume available liquidity and stop when filled or when the book runs out of liquidity.
- Better prices have priority. At the same price, the oldest resting order is matched first (FIFO).
- Supports partial fills, cancellations, order modifications, and top-of-book / market-depth queries.
- Uses an array-indexed price ladder, intrusive FIFO lists, a 64-bit bitmap to find occupied price levels, and a preallocated order pool.
- Includes unit tests, a randomized differential test against a simpler `std::map` / `std::list` reference engine, and performance benchmarks.

## Folder guide

```text
include/matching_engine/  Shared types and data structures
  Types.hpp                Prices, quantities, sides, statuses, trade reports
  Order.hpp                One order and its state
  IntrusiveList.hpp        Linked list used to keep FIFO priority
  MemoryPool.hpp           Preallocated, aligned storage for order objects
  PriceLevel.hpp           Orders and total volume at one price
  OrderBook.hpp            Price ladder, lookups, market depth and configuration
  MatchingEngine.hpp       Public trading operations and callbacks
  ReferenceEngine.hpp      Simpler implementation used to cross-check behavior

src/                       Engine implementation and main CLI (argc / argv)
tests/                     Unit tests and randomized differential test
benchmarks/                Latency / throughput benchmark code
bin/                       Generated executable location
```

## How matching works

1. Validate the incoming order ID, price, and quantity (ID zero is reserved as an invalid sentinel).
2. Inspect the best price on the opposite side of the book.
3. If prices cross, match against the oldest order at that price.
4. Continue across orders and price levels until the incoming order is filled or no eligible price remains.
5. Store any unfilled limit-order quantity in the book. Unfilled market-order quantity expires.

The resting order's price is the execution price. For example, an incoming buy limit at 103 can trade against a resting sell at 102 for 102.

## Design notes and limitations

- `OrderBookConfig::maxOrders` controls the size of the preallocated order pool.
- The pool's object storage is aligned for `Order`; the active-order `std::unordered_map` can still allocate internal nodes, so the whole order-processing path is not allocation-free.
- The engine is single-threaded and does not implement networking, exchange protocols, persistence, risk checks, or a live market-data feed.
- Benchmark results depend on the CPU, compiler, flags, and workload. Re-run `bench` on your machine before quoting performance numbers on a resume. Results from different machines are not directly comparable.

## Recommended learning order

1. `Types.hpp` and `Order.hpp`
2. `IntrusiveList.hpp` and `PriceLevel.hpp`
3. `MemoryPool.hpp`
4. `OrderBook.hpp` and `src/OrderBook.cpp`
5. `MatchingEngine.hpp` and `src/MatchingEngine.cpp`
6. `tests/` and `benchmarks/`

Read the implementation alongside the README: comments describe intent, but the source code and tests determine actual behavior.
