# Ultra-Low-Latency Limit Order Book & Matching Engine (C++20)

## Market-making research extension

A C++ / Python simulator now uses this engine's actual FIFO fills:
fixed-spread, inventory-skewed and Avellaneda-Stoikov-inspired policies; position caps;
fees and end liquidation; synthetic Poisson flow; informed-flow stress; reproducible comparisons.

**Start with [the study guide](docs/STUDY_GUIDE.md).**
Run one simulation: .\run-simulation.ps1 -Mode single
Run the grid and report: .\run-simulation.ps1 -Mode study
Run new tests: .\run-simulation.ps1 -Mode test

The delivered report is [results/study/report.html](results/study/report.html).
This is an educational synthetic study, not a historical backtest or live trading system.
The original engine is unchanged. [Implementation details](docs/IMPLEMENTATION.md).


A high-frequency trading (HFT) matching engine built in modern C++20 with **Price-Time Priority (FIFO)**, direct **array-indexed price levels**, **intrusive doubly-linked queues**, and a **pre-allocated pool allocator** to eliminate heap allocations on the critical path.

---

## ⚡ Performance Highlights (Calibrated CPU RDTSC @ 2.5 GHz)

| Metric | Result |
| :--- | :--- |
| **Sustained Throughput (Mixed Trading)** | **13.1+ Million ops/second (76.3 ns / op)** |
| **Sustained Throughput (Passive Influx)** | **12.9+ Million orders/second (77.3 ns / order)** |
| **Passive Limit Order Insertion (P50 / P99)** | **32.4 ns / 213.9 ns** |
| **Aggressive Trade Execution (P50 / P99)** | **67.3 ns / 487.9 ns** |
| **O(1) Live Order Cancellation (P50 / P99)** | **33.2 ns / 162.2 ns** |
| **Differential Fuzzing Verification** | **100% Match vs Reference Oracle (Trades, Depth, & Invalids)** |

---

## 📁 Repository Structure

```
lob+matchingengine/
├── .gitignore                           # Git ignore rules for build artifacts and binaries
├── build.ps1                            # Standalone C++20 build runner & CLI launcher
├── README.md                            # Comprehensive architectural documentation
├── include/
│   └── matching_engine/
│       ├── Types.hpp                    # Core types: OrderId, Price (ticks), Quantity, Side, ExecutionReport
│       ├── IntrusiveList.hpp            # Zero-allocation intrusive doubly-linked list for FIFO queues
│       ├── MemoryPool.hpp               # Cache-aligned (64B) block pool allocator using std::align_val_t
│       ├── Order.hpp                    # Cache-aligned (64B) order struct with intrusive node hooks
│       ├── PriceLevel.hpp               # Aggregated level volume, order count, and FIFO order list
│       ├── OrderBook.hpp                # Direct array-indexed ladder with 64-bit word bitset skip scanning
│       ├── MatchingEngine.hpp           # Price-Time priority engine with templated aggressive matching
│       └── ReferenceEngine.hpp          # Canonical std::map reference engine used as fuzzing oracle
├── src/
│   ├── OrderBook.cpp                    # Array indexing, bitset bounds tracking, and depth management
│   ├── MatchingEngine.cpp               # Templated Limit, Market, Cancel, and Modify matching pipelines
│   ├── ReferenceEngine.cpp              # Canonical reference implementation with deep ladder queries
│   └── main.cpp                         # Interactive visual CLI order book and trading command REPL
├── tests/
│   ├── UnitTests.cpp                    # Unit tests with non-suppressible CHECK assertions
│   └── DifferentialTest.cpp             # Randomized fuzzer with invalid input & deep ladder verification
└── benchmarks/
    ├── LatencyHistogram.hpp             # RDTSC cycle timer with CPU calibration and statistical percentiles
    └── BenchmarkEngine.cpp              # Multi-workload throughput & sub-nanosecond latency benchmarks
```

---

## 🏗️ Core Architectural Design

### 1. Pre-Allocated 64-Byte Aligned Memory Pool (`MemoryPool.hpp`)
Standard implementations invoke `new`/`malloc` on every incoming order. In high-frequency environments, dynamic heap allocations introduce unpredictable latency spikes (OS locks, page faults, memory fragmentation).
- Pre-allocates a contiguous memory region for 1,000,000+ orders upfront.
- Uses `std::align_val_t{64}` to guarantee true 64-byte cache line alignment for each `Order`.
- Employs a cache-warm LIFO free list for true $O(1)$ allocation and deallocation without heap syscalls.

### 2. Intrusive Doubly-Linked Queues (`IntrusiveList.hpp`)
Instead of `std::list<Order*>` or `std::deque` which allocate independent node wrappers:
- `Order` inherits directly from `IntrusiveNode<Order>`, embedding `prev` and `next` pointers within the order itself.
- Orders are enqueued and removed in $O(1)$ with zero pointer hops and high cache locality.

### 3. Array-Indexed Ladder with 64-Bit Word Bitset (`OrderBook.hpp`)
Traditional order books use Red-Black trees (`std::map`), which require $O(\log N)$ tree traversal and suffer pointer chasing across fragmented heap memory.
- Price levels are mapped using direct array offset: $\text{Index} = \text{Price} - \text{MinPrice}$.
- Provides genuine $O(1)$ direct cache-friendly array access to any price tick.
- **Hierarchical 64-bit word bitmaps**: When tracking the Best Bid and Best Ask across sparse books, the engine scans non-empty levels using `std::countl_zero` / `std::countr_zero` CPU instructions, skipping 64 empty price levels in a single clock cycle.

### 4. Single-Threaded Deterministic Architecture
- The matching engine is deliberately single-threaded by design (the standard architecture for tier-1 exchange matching cores like CME and NASDAQ).
- Eliminates mutex contention, atomic bus locking, and inter-core cache line bouncing. Orders are processed in strict, deterministic arrival order.

### 5. Order Priority Preservation Rules
- **Quantity Decreases**: An order reducing its quantity preserves its existing queue priority position.
- **Quantity Increases or Price Changes**: The order loses queue priority and is re-queued at the tail of the new price level (or matched aggressively if crossing).
- **Safety**: Modifying an order with invalid parameters (e.g. out of range price or 0 quantity) immediately rejects the request without affecting or destroying the resting order.

---

## 🔬 Testing & Verification

### Unit Testing (`tests/UnitTests.cpp`)
Covers critical low-level primitives and high-level matching rules:
1. `MemoryPool`: Slot re-use, exhaustion detection, and in-place construction.
2. `IntrusiveList`: Head, middle, tail removals, and FIFO queue iteration.
3. `PriceTimePriorityFIFO`: Strict price-time order fills under matching pressure.
4. `MarketOrderSweeps`: Multi-level depth sweep until filled.
5. `OrderCancelAndModify`: Queue priority preservation and cancellations.

### Randomized Differential Fuzzing (`tests/DifferentialTest.cpp`)
To guarantee correctness, orders are fuzzed across a randomized stream of operations comparing the optimized engine against `ReferenceEngine` (built with `std::map` and `std::list`):
- Generates 25,000+ randomized limit, market, cancel, and modify operations.
- Asserts identical trade execution reports (maker, taker, price, quantity) after each operation.
- Validates identical Top-of-Book (Best Bid & Best Ask) states throughout the run.

---

## 🚀 Build and Run

The project has **zero external dependencies** and does not require CMake. You can build and run everything with the standalone PowerShell runner ([build.ps1](file:///c:/Users/lodha/OneDrive/Documents/lob+matchingengine/build.ps1)) or directly with `g++`.

### 1. Interactive Trading Terminal & CLI Commands

```powershell
# Launch the Interactive Trading Terminal (REPL prompt: lob>)
.\build.ps1 -Target engine

# Or run orders directly via command-line arguments:
.\bin\engine.exe limit buy 100 50 101       # Place Limit Buy Order #101: 50 @ 100 ticks
.\bin\engine.exe limit sell 105 25 201      # Place Limit Sell Order #201: 25 @ 105 ticks
.\bin\engine.exe market buy 30 301          # Aggressive Market Buy: 30 shares
.\bin\engine.exe modify 101 99 40           # Modify Order #101 (Qty: 40 @ Price: 99)
.\bin\engine.exe cancel 101                 # Cancel Order #101
.\bin\engine.exe book 10                    # Print top 10 depth levels
.\bin\engine.exe demo                       # Run full 5-step guided walkthrough
.\bin\engine.exe help                       # Print trading command cheat-sheet
```

---

### 2. Testing and Benchmarks

```powershell
# 1. Run the guided walkthrough demonstration
.\build.ps1 -Target demo

# 2. Run comprehensive unit tests
.\build.ps1 -Target test

# 3. Run randomized differential testing against oracle (25,000 ops)
.\build.ps1 -Target diff

# 4. Run low-latency and throughput benchmarks
.\build.ps1 -Target bench

# 5. Compile all binaries into bin/
.\build.ps1 -Target all
```

---

### 3. Direct Single-Line `g++` Compilation

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH
g++ -std=c++20 -O3 -Wall -Wextra -Iinclude src/main.cpp src/OrderBook.cpp src/MatchingEngine.cpp src/ReferenceEngine.cpp -o bin/engine.exe
.\bin\engine.exe
```

