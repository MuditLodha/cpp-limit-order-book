# Implementation and validation

Added a C++ synthetic market-making layer around the existing MatchingEngine API.
No original matching-engine implementation was changed.

New files:
- include/market_making/Simulator.hpp
- src/MarketMaking.cpp
- simulation/main.cpp
- simulation/run_study.py
- simulation/requirements.txt
- tests/MarketMakingTests.cpp
- tests/test_simulation.py
- run-simulation.ps1
- docs/STUDY_GUIDE.md

Updated:
- build.ps1: sim-build, sim, sim-test, all targets and failure propagation.
- README.md: study entry point.
- .gitignore: generated results and Python caches.

Verified:
- Original eight engine unit-test groups.
- Original differential test: 25,000 operations, seed 42.
- Eight simulator groups: accounting, policies, FIFO/IOC, caps/liquidation, markouts,
  reproducibility/common random inputs, causality/fee monotonicity, invalid/zero-flow cases.
- Three Python integration methods: invalid-input subcases, reproducible files,
  ledger reconstruction, study shape and detection of corrupted audit input.
- Default 576-run study, all summary invariants, and independent reconciliation of 12 fill ledgers.

Limitations: synthetic grid, exogenous reference, artificial informed-flow mechanism,
resetting background liquidity, no adaptive competitor, no impact, no margin,
no exchange calibration or realistic network latency.
The A-S-inspired heuristic is not claimed optimal for this fill model.

Start with docs/STUDY_GUIDE.md and results/study/report.html.
