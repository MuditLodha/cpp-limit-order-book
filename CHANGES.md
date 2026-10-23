# Project cleanup and simple command-line interface

- Removed build helper scripts. The project now uses one direct `g++` command to compile a single executable.
- Added an `argc` / `argv` dispatcher so the same executable can run interactive trading mode, demo, unit tests, differential tests, or benchmarks.
- Renamed the test and benchmark entry functions so all project modes can be linked into the same executable.
- Set differential testing to run 25,000 operations with seed 42 by default; users can override both values from the command line.
- Made `OrderBookConfig::maxOrders` control the runtime memory-pool capacity.
- Kept the memory pool's aligned object storage and object-lifetime handling, with regression tests.
- Added regression tests for invalid price ranges, reserved order ID zero, configured capacity, and invalid prices that must not execute trades.
- Removed stale claims that the whole hot path is allocation-free. The active-order `std::unordered_map` can allocate internal nodes.
- Standardized the documented build on C++14 and kept the source compatible with that standard.
