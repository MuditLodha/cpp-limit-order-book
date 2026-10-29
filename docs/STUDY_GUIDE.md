# Study guide: market making on your matching engine

## Start here

This project asks: **how much inventory risk does a quoting policy remove, and what does it sacrifice in fills and earnings?**

Your existing engine acts as an exchange. This extension adds an experimental market, a trader submitting orders, a ledger, and three quoting policies. It is synthetic research, not a historical backtest or live trading system.

Project directory: C:\Users\lodha\Documents\Projects\Coding\lob+matchingengine

Open results/study/report.html for the completed 576-run study, with embedded plots.

### Run commands (PowerShell, from the project folder)

~~~powershell
# One simulation; no Python packages required
.\run-simulation.ps1 -Mode single -Strategy inventory -Seconds 120 -Seed 1

# Original engine tests
.\build.ps1 -Target test
.\build.ps1 -Target diff

# Simulator unit tests and integration checks
.\run-simulation.ps1 -Mode test

# Full study and report; matplotlib required
python -m pip install -r simulation/requirements.txt
.\run-simulation.ps1 -Mode study -Seeds 12 -Seconds 120

# Recreate the report without rerunning simulations
python simulation/run_study.py --report-only --output results/study

# Fresh seeds for a hypothesis chosen BEFORE this run
python simulation/run_study.py --seed 1001 --seeds 20 --output results/validation
~~~

Python and matplotlib were already available on this machine. No packages were installed.
The C++ simulator has no new third-party dependencies. On another Windows machine, ensure your compiler's DLL directory is on PATH.

### Read these files in order

1. include/market_making/Simulator.hpp: parameters and data structures.
2. src/MarketMaking.cpp, Account::fill: cash and inventory.
3. src/MarketMaking.cpp, makeQuote: three policies.
4. src/MarketMaking.cpp, generateTape: synthetic market.
5. src/MarketMaking.cpp, run: orders passed into your engine.
6. src/MarketMaking.cpp, calculateMetrics: metric definitions.
7. tests/MarketMakingTests.cpp: hand-checkable examples.
8. simulation/run_study.py: independent audit, statistics and charts.
9. simulation/main.cpp: command line and experiment grid.

The original matching-engine implementation is unchanged.

## 1. The simplest example

At reference price 10000 ticks, quote a bid of 9998 and ask of 10002, five lots each.
If you buy five at 9998, inventory becomes +5 and cash falls by 49990 plus fees.
Equity equals cash plus inventory times the reference.
If you later sell five at 10002, gross round-trip profit is 20 tick-lots.
If the reference falls before you sell, your inventory loses value.
A spread is an opportunity to earn money, not a guarantee.

One tick-lot is one lot multiplied by one price tick. No dollar or rupee conversion is implied.

## 2. The exact event timeline

Default decision interval: 0.1 simulated seconds.

At time t:
1. Cancel the strategy's previous quotes.
2. Cancel old background orders and rebuild competing depth around the current reference.
3. Compute strategy quotes from current reference, inventory and public configuration.
4. Clip them to avoid crossing the opposite book; submit through MatchingEngine.
5. Record the account and quotes before incoming flow.
6. Process incoming requests through the engine; immediately cancel unfilled remainders.
7. Update the reference by the price innovation hidden from the policy.

At the horizon, cancel strategy quotes, refresh background depth, and flatten inventory using a market order through the engine.

Consequences:
- Background liquidity is first in FIFO at an equal price.
- Cancel/repost loses priority.
- Every fill comes from the engine.
- The policy never receives the future innovation or the informed-trader flag.
- Poisson arrivals are BATCHED on a fixed grid, not processed at continuous event times.
- All policies have identical decision frequency and inventory limits.

## 3. The synthetic market

### Reference price
S(next) = S(now) + sigma * sqrt(dt) * Z, with standard-normal Z.
Default initial reference: 10000 ticks. Sigma: 1.5 ticks per square-root second.

This is an observable EXTERNAL reference, not the midpoint of our own book.
Orders cannot move it. Out-of-range paths fail explicitly rather than being silently clipped.

### Incoming flow
- Orders per interval: Poisson(arrival_rate * dt); default rate 8 per second, both sides combined.
- Quantity: 1 + geometric random variable, mean 12 lots, extreme safety cap.
- Price tolerance: exponential random variable, mean 4 ticks.
- Neutral direction: independent fair buy/sell draw.

A buy pays at most floor(S + tolerance). A sell accepts at least ceil(S - tolerance).
These are immediate-or-cancel LIMIT requests, not unlimited market orders.
An arbitrarily distant quote therefore does not automatically receive a fill.
Execution sweeps levels only up to the limit, respecting volume and priority.

### Background liquidity
Ten levels per side, eight lots per level. Nearest quotes are three ticks from reference;
further levels are two ticks apart. All background liquidity refreshes every interval.
This competitor has no learning, capital constraint or realistic cancellation process.

### Informed-flow stress
With probability 0.6, an incoming trader knows the sign of the NEXT price innovation:
it buys before an increase and sells before a decrease.
Our policy does not know it. The other orders have random direction.

This is an artificial informational disadvantage, not a calibrated model of real informed trading.
Separate price/flow generators and a fixed order of random draws ensure identical external tapes
for the same seed and market parameters. Strategies can get different fills because their orders differ.

## 4. Quoting policies

### Fixed spread
Bid = floor(S - h); ask = ceil(S + h). Default h = 2 ticks.
The quote center ignores inventory; the hard inventory cap still applies.

### Linear inventory skew
r = S - beta*q
Bid = floor(r - h); ask = ceil(r + h).
Default beta = 0.12; q is signed inventory.

Positive inventory shifts BOTH quotes lower. The bid becomes less attractive to sellers,
and the ask becomes more attractive to buyers, encouraging inventory reduction.
Negative inventory shifts both quotes higher.

### Avellaneda-Stoikov-inspired
tau = min(risk_horizon, time_remaining)
r = S - q*gamma*sigma^2*tau
h = 0.5*gamma*sigma^2*tau + log(1 + gamma/kappa)/gamma

Default gamma = 0.02, kappa = 0.5, risk_horizon = 2 seconds.
Gamma controls inventory aversion. Kappa comes from the original model's fill-intensity decay.
Here it is an uncalibrated sensitivity parameter.
The strategy knows the scenario sigma; this version does not estimate volatility.

The original paper's fill-intensity assumptions differ from our engine, discrete ticks,
multi-lot orders, competing depth, rolling horizon, post-only clipping and hard caps.
This is an A-S-INSPIRED heuristic, not an optimal-control solution for this simulator.

The half-spread option affects fixed and inventory policies. A-S computes its own width.

### Inventory protection
Buy size = max(0, min(quote_size, limit - q)).
Sell size = max(0, min(quote_size, limit + q)).

At q=+50 there is no bid; at q=-50 there is no ask.
At q=+48 and size=5, bid size is only 2.
The limits account for outstanding quote quantities before orders are submitted.

An aggressive skewed price is clipped to one tick inside the opposite resting price.
Quoting remains passive. Terminal liquidation is the deliberate exception.

## 5. Accounting

For direction d=+1 on a buy, -1 on a sell, price p and quantity v:
q_new = q_old + d*v
cash_new = cash_old - d*v*p - fee_per_lot*v
equity = cash + q*S

Maker fee: 0.02 ticks per lot. Terminal taker fee: 0.10 ticks per lot. No rebates.
Cash ledger starts at zero; it tracks PnL cash flows.
Fixed reference capital of 1,000,000 tick-lots normalizes grid returns only.
There are no margin calls or financing costs in the model.

Terminal liquidation sweeps the modeled background book. Large positions pay depth costs.
Final inventory must be zero; net PnL is therefore final cash.
Liquidation cost = pre-liquidation marked equity - final cash.

The smallest execution test:
- Background bid: eight lots at 9997; our bid is behind it.
- Incoming sell: ten lots. Background receives eight; we receive two.
- Buy two at 9997; finally sell two at 9997.
- Gross PnL zero; net PnL -0.24 from both fees.

This verifies FIFO, partial fills, fees and liquidation with hand-checkable numbers.

## 6. Metrics

| Metric | Meaning |
|---|---|
| Net PnL | Final cash after fees and actual terminal liquidation |
| Pre-liquidation PnL | Cash + position * terminal reference before liquidation |
| Liquidation cost | Pre-liquidation equity minus final cash |
| Inventory variance | Population variance of pre-flow grid snapshots |
| Mean absolute inventory | Mean absolute position at those snapshots |
| Maximum absolute inventory | Maximum across EVERY fill, as well as snapshots |
| Maximum drawdown | Largest sampled equity peak-to-trough decline, including liquidation |
| Fill fraction | Passive filled lots / ALL quote lots submitted, counting reposts again |
| Spread capture | Volume-weighted d*(reference_at_fill - execution_price), before fees |
| Adverse-selection proxy | Volume-weighted -d*(future_reference - reference_at_fill) |
| Execution markout | Volume-weighted d*(future_reference - execution_price), before fees |
| Step Sharpe | Mean/sample-std of fixed-capital grid returns; NOT annualized |

Positive adverse-selection proxy means an unfavorable future move.
Example: buying just before the reference falls.
This is a signed markout, not complete causal attribution.

Default markout horizon: one second. A non-grid horizon rounds UP to the next grid point.
Fills near the end without a complete future horizon are excluded; their volume is reported.
Spread capture uses ALL passive fills, whereas markouts use only eligible fills.
Thus the aggregate spread and adverse-selection means do not necessarily subtract exactly
to aggregate execution markout.

Step Sharpe includes liquidation, uses zero risk-free rate and a fixed capital denominator.
Do not compare it with published annual trading Sharpe ratios.
When variance is zero, the software outputs 0 by convention.
Inventory variance and drawdown are grid-sampled, not continuous-time measurements.

## 7. Experiment design

Comparison: four regimes * three strategies * 12 seeds = 144 runs.
- Quiet: sigma 0.5, neutral flow.
- Normal: sigma 1.5, neutral flow.
- Volatile: sigma 3.0, neutral flow.
- Informed: sigma 1.5, informed fraction 0.6.

Sweep: two regimes * two policies * three widths * three sizes * 12 seeds = 432 runs.
- Regimes: volatile and informed.
- Policies: fixed and linear inventory skew.
- Half-spreads: 1, 2, 4 ticks.
- Quote sizes: 2, 5, 10 lots.
- A-S excluded because it calculates its own width.

Total 576 runs, each 120 seconds, dt 0.1; seeds 1..12.
Policies share external events for a given seed/scenario.
The report calculates same-seed differences before bootstrapping paired effects.
95% intervals use 3,000 bootstrap resamples across simulation seeds.

Twelve seeds are an educational sample. Intervals measure seed uncertainty INSIDE this model,
not uncertainty about whether the model represents reality.
Sweeps are exploratory: selecting the best cell is not out-of-sample validation.
Use fresh seeds after choosing a hypothesis or parameter setting.

C++ random distributions reproduce with the same compiler/standard library, but are not guaranteed
bit-identical across toolchains. The Python runner records command, compiler and source/binary hashes.

## 8. Findings from the delivered study

Normal scenario:
- Fixed: mean net PnL 4518.84 tick-lots; inventory variance 495.63.
- Inventory skew: mean net PnL 3959.60 tick-lots; inventory variance 39.32.
- About 92.1% lower mean inventory variance, at about 12.4% lower mean net PnL.
- Mean sampled drawdown decreased from 339.00 to 34.70 tick-lots.
- Paired PnL change: -559.24 tick-lots; approximate 95% interval -873.3 to -235.7.

These values describe the delivered default experiment, not all parameter choices.
Inventory control buys lower exposure at a cost in spread earnings and/or fills.
A-S's stronger inventory penalty at high volatility also sacrifices earnings.

Large positive synthetic PnLs reflect a generous model: known reference, no impact,
no realistic latency, and a simplistic competitor. They do not establish real-market edge.

## 9. Outputs

- runs.csv: one row per run with configuration and metrics.
- metadata.json: assumptions, compiler, command and reproduction hashes.
- aggregate.csv: regime/strategy means and confidence intervals.
- paired_comparisons.csv: same-seed differences and confidence intervals.
- report.html: standalone illustrated report.
- RESULTS.md: generated findings.
- comparison.png: strategy results and uncertainty.
- example_paths.png: illustrative first-seed histories.
- sweep_volatile.png and sweep_informed.png: width/size sensitivity.
- examples/REGIME/STRATEGY/fills.csv: fills, liquidation, fees and engine order IDs.
- examples/REGIME/STRATEGY/trajectory.csv: reference, cash, equity, inventory and quotes.

Example paths use the first seed selected in advance, not the best-performing run.

## 10. Five study sessions

### Session 1: cash and positions
Read Account::fill and its test. Calculate a buy, a price fall and a sale.
Explain why positive cash can coexist with a losing short position.

### Session 2: quotes
Compute bids/asks for q=-20, 0, +20.
Explain why positive inventory moves both prices down.
Try beta=0: otherwise identical linear-skew and fixed policies should match.

### Session 3: execution
Read the run loop and fifoAndIoc test. Find a partial fill in fills.csv.
Explain why touching a price does not guarantee a fill.
Explain why Poisson arrivals are not the same as randomly assigned fills.

### Session 4: evidence
Read report.html. Explain both the benefit and cost in each comparison.
Change quote size while holding spread fixed, then do the reverse.

### Session 5: challenge assumptions
Write a hypothesis before running an experiment.
Try higher fees, lower price tolerance, different background depth, or another decision interval.
Changing dt changes quote refresh frequency AND informed traders' information horizon:
it is not purely numerical refinement.
Keep competitor and execution assumptions identical across policies.
Validate selected settings with fresh seeds.

## 11. Questions to prepare

1. Why can spread capture still lose money?
2. How do inventory risk and adverse selection differ?
3. What exactly can the strategy observe?
4. Which model assumptions make profit too easy?
5. Why share external market tapes across strategies?
6. How are fees and terminal inventory accounted for?
7. Does lower inventory variance prove a better strategy?
8. Why is the A-S formula not optimal in this environment?
9. What is fill rate's denominator? How does reposting affect it?
10. Why is annualizing this Sharpe misleading?
11. What do the confidence intervals omit?
12. What result would change your mind?

## 12. Resume wording after you can reproduce and explain the work

Market-Making Strategy Simulator | C++ / Python

- Extended a C++ FIFO matching engine with fixed-spread, inventory-skewed and Avellaneda-Stoikov-inspired quoting policies, enforcing position limits and execution-based accounting.
- Compared strategies across 576 synthetic runs covering volatility and informed-order-flow scenarios; included fees, terminal liquidation, signed markouts and paired bootstrap confidence intervals.
- Measured roughly 92% lower mean inventory variance at roughly 12% lower mean net PnL for linear skew versus fixed quoting in the default normal-volatility scenario.

The last bullet is specific to this simulation. Regenerate it if you change the study.
These are not live returns, historical backtest results or proof of a tradable edge.

## References

- Avellaneda and Stoikov: https://math.nyu.edu/~avellane/HighFrequencyTrading.pdf
- Market Simulation under Adverse Selection: https://arxiv.org/abs/2409.12721

Papers motivate methods and limitations. Our report uses only our own simulation results.
