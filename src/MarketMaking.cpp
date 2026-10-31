#include "market_making/Simulator.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <stdexcept>

namespace market_making {
using namespace matching_engine;

std::string name(Strategy strategy) {
    switch (strategy) {
        case Strategy::Fixed: return "fixed";
        case Strategy::Inventory: return "inventory";
        case Strategy::AvellanedaStoikov: return "as";
    }
    throw std::invalid_argument("Unknown strategy");
}

Strategy parseStrategy(const std::string& strategyName) {
    if (strategyName == "fixed") return Strategy::Fixed;
    if (strategyName == "inventory") return Strategy::Inventory;
    if (strategyName == "as") return Strategy::AvellanedaStoikov;
    throw std::invalid_argument("Strategy must be fixed, inventory or as");
}

void Config::validate() const {
    const double numericParameters[] = {
        seconds, dt, initialPrice, sigma, arrivalRate, informedFraction,
        meanOrderSize, meanTolerance, halfSpread, skew, gamma, kappa, riskHorizon,
        makerFee, takerFee, markoutHorizon, initialCapital
    };
    for (double value : numericParameters) {
        if (!std::isfinite(value)) {
            throw std::invalid_argument("All numeric inputs must be finite");
        }
    }
    if (dt <= 0 || seconds < dt || seconds / dt > 1000000 ||
        std::abs(seconds / dt - std::round(seconds / dt)) > 1e-7) {
        throw std::invalid_argument("seconds must be an integer multiple of dt; 1..1e6 steps");
    }
    if (sigma < 0 || sigma > 100 || arrivalRate < 0 || arrivalRate * dt > 1000 ||
        informedFraction < 0 || informedFraction > 1 || meanOrderSize < 1 ||
        meanOrderSize > 10000 || meanTolerance <= 0 || halfSpread < 0.5 ||
        halfSpread > 100 || skew < 0 || gamma <= 0 || kappa <= 0 || riskHorizon <= 0) {
        throw std::invalid_argument("Invalid market or quoting parameters");
    }
    if (quoteSize < 1 || inventoryLimit < 1 || inventoryLimit > 100000 ||
        backgroundLevels < 1 || backgroundLevels > 100 || backgroundSize < 1 ||
        backgroundSize > 100000 || backgroundSpacing < 1 || backgroundSpacing > 100 ||
        backgroundHalfSpread < 1 || backgroundHalfSpread > 100 ||
        static_cast<long long>(backgroundLevels) * backgroundSize < inventoryLimit) {
        throw std::invalid_argument("Invalid sizes; background depth must cover inventory limit");
    }
    if (initialPrice < 1000 || initialPrice > 19000 || makerFee < 0 || takerFee < 0 ||
        markoutHorizon <= 0 || initialCapital <= 0) {
        throw std::invalid_argument("Invalid price, fees, horizon or capital");
    }
}

Quote makeQuote(const Config& config, double referencePrice, int inventory, double secondsRemaining) {
    double quoteCenter = referencePrice;
    double quoteHalfSpread = config.halfSpread;

    if (config.strategy == Strategy::Inventory) {
        quoteCenter -= config.skew * inventory;
    }
    if (config.strategy == Strategy::AvellanedaStoikov) {
        // Inspired by A-S quoting; fills are not calibrated to its intensity law.
        const double riskHorizon = std::min(config.riskHorizon, std::max(0.0, secondsRemaining));
        const double inventoryRiskTerm = config.gamma * config.sigma * config.sigma * riskHorizon;
        quoteCenter -= inventory * inventoryRiskTerm;
        quoteHalfSpread = 0.5 * inventoryRiskTerm +
                          std::log1p(config.gamma / config.kappa) / config.gamma;
    }

    if (!std::isfinite(quoteCenter) || !std::isfinite(quoteHalfSpread) ||
        quoteCenter - quoteHalfSpread < 1 || quoteCenter + quoteHalfSpread > 20000) {
        throw std::runtime_error("Quote exceeds finite ladder; reduce parameters");
    }

    return {
        static_cast<int>(std::floor(quoteCenter - quoteHalfSpread)),
        static_cast<int>(std::ceil(quoteCenter + quoteHalfSpread)),
        std::max(0, std::min(config.quoteSize, config.inventoryLimit - inventory)),
        std::max(0, std::min(config.quoteSize, config.inventoryLimit + inventory))
    };
}

std::vector<Tick> generateTape(const Config& config) {
    config.validate();
    std::mt19937_64 priceGenerator(config.seed);
    std::mt19937_64 orderFlowGenerator(config.seed ^ 0x9e3779b97f4a7c15ULL);
    std::normal_distribution<double> normalPriceChange(0, 1);
    std::poisson_distribution<int> orderArrivals(config.arrivalRate * config.dt);
    std::geometric_distribution<int> orderSize(1.0 / config.meanOrderSize);
    std::exponential_distribution<double> priceTolerance(1.0 / config.meanTolerance);
    std::uniform_real_distribution<double> randomProbability(0, 1);

    const size_t stepCount = static_cast<size_t>(std::llround(config.seconds / config.dt));
    std::vector<Tick> marketTape;
    marketTape.reserve(stepCount);

    for (size_t stepIndex = 0; stepIndex < stepCount; ++stepIndex) {
        Tick currentTick{config.sigma * std::sqrt(config.dt) * normalPriceChange(priceGenerator), {}};
        const int arrivalCount = orderArrivals(orderFlowGenerator);

        for (int orderIndex = 0; orderIndex < arrivalCount; ++orderIndex) {
            // Use the same random tape when comparing different strategies.
            const bool isInformed = randomProbability(orderFlowGenerator) < config.informedFraction;
            const bool randomBuy = randomProbability(orderFlowGenerator) < 0.5;
            const bool isBuy = isInformed && currentTick.innovation != 0
                ? currentTick.innovation > 0 : randomBuy;
            currentTick.orders.push_back({isBuy ? Side::Buy : Side::Sell, 1 + std::min(orderSize(orderFlowGenerator), 100000), priceTolerance(orderFlowGenerator), isInformed});
        }
        marketTape.push_back(std::move(currentTick));
    }
    return marketTape;
}

void Account::fill(int direction, int quantity, double price, double feePerLot) {
    if ((direction != 1 && direction != -1) || quantity <= 0 || price <= 0 ||
        feePerLot < 0 || !std::isfinite(price) || !std::isfinite(feePerLot)) {
        throw std::invalid_argument("Invalid fill");
    }
    inventory += direction * quantity;
    const double tradeFee = quantity * feePerLot;
    cash -= direction * quantity * price + tradeFee;
    fees += tradeFee;
}

double Result::fillRate() const {
    return postedVolume ? static_cast<double>(passiveVolume) / postedVolume : 0;
}

Result run(const Config& config) {
    return run(config, generateTape(config));
}

Result run(const Config& config, const std::vector<Tick>& marketTape) {
    config.validate();
    const size_t stepCount = static_cast<size_t>(std::llround(config.seconds / config.dt));
    if (marketTape.size() != stepCount) {
        throw std::invalid_argument("Tape length differs from duration");
    }

    MatchingEngine matchingEngine(OrderBookConfig(1, 20000, 10000));
    Result result;
    result.config = config;
    Account account;
    OrderId nextOrderId = 1;
    OrderId bidOrderId = 0;
    OrderId askOrderId = 0;
    OrderId liquidationOrderId = 0;
    std::vector<OrderId> backgroundOrderIds;
    double referencePrice = config.initialPrice;
    double currentTime = 0;
    bool isLiquidating = false;

    matchingEngine.setRejectListener([](OrderId, RejectReason reason) {
        if (reason != RejectReason::NoLiquidity) {
            throw std::runtime_error("Unexpected matching-engine rejection");
        }
    });

    // The matching engine notifies us whenever it executes a trade.
    matchingEngine.setTradeListener([&](const ExecutionReport& trade) {
        int direction = 0;
        if (trade.makerOrderId == bidOrderId) direction = 1;
        if (trade.makerOrderId == askOrderId) direction = -1;
        if (trade.takerOrderId == liquidationOrderId && isLiquidating) {
            direction = trade.takerSide == Side::Buy ? 1 : -1;
        }
        if (!direction) return;

        const double feePerLot = isLiquidating ? config.takerFee : config.makerFee;
        account.fill(direction, static_cast<int>(trade.matchQuantity), trade.matchPrice, feePerLot);
        result.fills.push_back({currentTime, static_cast<double>(trade.matchPrice), referencePrice, feePerLot * trade.matchQuantity, direction, static_cast<int>(trade.matchQuantity), isLiquidating, trade.makerOrderId, trade.takerOrderId});
        if (!isLiquidating) result.passiveVolume += trade.matchQuantity;
        if (std::abs(account.inventory) > config.inventoryLimit) {
            throw std::runtime_error("Inventory limit breached");
        }
    });

    auto cancelOrderIfLive = [&](OrderId orderId) {
        if (orderId && matchingEngine.book().findOrder(orderId)) {
            matchingEngine.cancelOrder(orderId);
        }
    };

    auto cancelCurrentQuotes = [&]() {
        cancelOrderIfLive(bidOrderId);
        cancelOrderIfLive(askOrderId);
        bidOrderId = askOrderId = 0;
    };

    auto refreshBackgroundLiquidity = [&]() {
        for (OrderId orderId : backgroundOrderIds) cancelOrderIfLive(orderId);
        backgroundOrderIds.clear();

        for (int levelIndex = 0; levelIndex < config.backgroundLevels; ++levelIndex) {
            const int distance = config.backgroundHalfSpread +
                                 levelIndex * config.backgroundSpacing;
            const int backgroundBid = static_cast<int>(std::floor(referencePrice - distance));
            const int backgroundAsk = static_cast<int>(std::ceil(referencePrice + distance));
            if (backgroundBid < 1 || backgroundAsk > 20000) {
                throw std::runtime_error("Reference left price ladder");
            }
            for (auto [side, price] : {std::pair{Side::Buy, backgroundBid},
                                       std::pair{Side::Sell, backgroundAsk}}) {
                OrderId orderId = nextOrderId++;
                if (!matchingEngine.submitLimitOrder(orderId, side, price, config.backgroundSize)) {
                    throw std::runtime_error("Background order rejected");
                }
                backgroundOrderIds.push_back(orderId);
            }
        }
    };

    auto recordSample = [&](int quotedBid, int quotedAsk) {
        result.samples.push_back({currentTime, referencePrice, account.cash, account.equity(referencePrice), account.inventory, quotedBid, quotedAsk});
    };

    // Requote, process incoming flow, then move the reference price.
    for (size_t stepIndex = 0; stepIndex < stepCount; ++stepIndex) {
        currentTime = stepIndex * config.dt;
        cancelCurrentQuotes();
        refreshBackgroundLiquidity(); // Background orders have FIFO priority at identical prices.

        Quote quote = makeQuote(config, referencePrice, account.inventory, config.seconds - currentTime);
        // Only post passive orders, so neither quote crosses the opposite side.
        quote.bid = std::min(quote.bid, static_cast<int>(matchingEngine.book().bestAskPrice()) - 1);
        quote.ask = std::max(quote.ask, static_cast<int>(matchingEngine.book().bestBidPrice()) + 1);
        if (quote.bid >= quote.ask) {
            throw std::runtime_error("Crossed strategy quotes");
        }
        const Timestamp timestamp = static_cast<Timestamp>(std::llround(currentTime * 1e9));

        if (quote.buySize > 0) {
            bidOrderId = nextOrderId++;
            matchingEngine.submitLimitOrder(bidOrderId, Side::Buy, quote.bid, quote.buySize, timestamp);
            result.postedVolume += quote.buySize;
        }
        if (quote.sellSize > 0) {
            askOrderId = nextOrderId++;
            matchingEngine.submitLimitOrder(askOrderId, Side::Sell, quote.ask, quote.sellSize, timestamp);
            result.postedVolume += quote.sellSize;
        }
        recordSample(quote.buySize ? quote.bid : 0, quote.sellSize ? quote.ask : 0);

        // Strategy quotes before the new random price movement is revealed.
        const Tick& currentTick = marketTape[stepIndex];
        if (!std::isfinite(currentTick.innovation)) {
            throw std::invalid_argument("Invalid innovation");
        }
        for (const Flow& incomingOrder : currentTick.orders) {
            if (incomingOrder.quantity <= 0 || !std::isfinite(incomingOrder.tolerance) ||
                incomingOrder.tolerance < 0) {
                throw std::invalid_argument("Invalid incoming flow");
            }
            const double requestedLimitPrice = incomingOrder.side == Side::Buy
                ? std::floor(referencePrice + incomingOrder.tolerance)
                : std::ceil(referencePrice - incomingOrder.tolerance);
            const Price limitPrice = static_cast<Price>(std::clamp(requestedLimitPrice, 1.0, 20000.0));
            OrderId incomingOrderId = nextOrderId++;
            result.incomingVolume += incomingOrder.quantity;
            matchingEngine.submitLimitOrder(incomingOrderId, incomingOrder.side, limitPrice, incomingOrder.quantity, timestamp);

            // Incoming orders are IOC-like: cancel their unfilled remainder.
            if (auto* remainingOrder = matchingEngine.book().findOrder(incomingOrderId)) {
                result.unfilledIncomingVolume += remainingOrder->remainingQuantity;
                matchingEngine.cancelOrder(incomingOrderId);
            }
        }
        referencePrice += currentTick.innovation;
        if (!std::isfinite(referencePrice) || referencePrice < 100 || referencePrice > 19900) {
            throw std::runtime_error("Price path outside supported ladder");
        }
    }

    // Close the final inventory so that strategies have comparable net PnL.
    currentTime = config.seconds;
    result.preLiquidationPnl = account.equity(referencePrice);
    cancelCurrentQuotes();
    refreshBackgroundLiquidity();
    if (account.inventory != 0) {
        isLiquidating = true;
        liquidationOrderId = nextOrderId++;
        const Side liquidationSide = account.inventory > 0 ? Side::Sell : Side::Buy;
        const Quantity liquidationQuantity = static_cast<Quantity>(std::abs(account.inventory));
        if (matchingEngine.submitMarketOrder(liquidationOrderId, liquidationSide, liquidationQuantity, static_cast<Timestamp>(std::llround(currentTime * 1e9))) != liquidationQuantity) {
            throw std::runtime_error("Insufficient final liquidation depth");
        }
    }
    recordSample(0, 0);
    result.finalInventory = account.inventory;
    result.netPnl = account.cash;
    result.fees = account.fees;
    result.liquidationCost = result.preLiquidationPnl - result.netPnl;
    calculateMetrics(result);
    return result;
}

void calculateMetrics(Result& result) {
    const Config& config = result.config;
    const size_t numberOfSteps = result.samples.size() - 1;
    double sumInventory = 0;
    double sumSquaredInventory = 0;
    double sumAbsoluteInventory = 0;
    double peakEquity = 0;
    double sumReturns = 0;
    double sumSquaredReturns = 0;
    result.maxDrawdown = 0;
    result.maxAbsInventory = 0;

    for (size_t sampleIndex = 0; sampleIndex < result.samples.size(); ++sampleIndex) {
        const Sample& sample = result.samples[sampleIndex];
        peakEquity = std::max(peakEquity, sample.equity);
        result.maxDrawdown = std::max(result.maxDrawdown, peakEquity - sample.equity);
        if (sampleIndex < numberOfSteps) {
            sumInventory += sample.inventory;
            sumSquaredInventory += static_cast<double>(sample.inventory) * sample.inventory;
            sumAbsoluteInventory += std::abs(sample.inventory);
        }
        result.maxAbsInventory = std::max(result.maxAbsInventory, std::abs(sample.inventory));
        if (sampleIndex > 0) {
            const double stepReturn = (sample.equity - result.samples[sampleIndex - 1].equity) /
                                      config.initialCapital;
            sumReturns += stepReturn;
            sumSquaredReturns += stepReturn * stepReturn;
        }
    }

    int runningInventory = 0;
    for (const Fill& fill : result.fills) {
        runningInventory += fill.direction * fill.quantity;
        result.maxAbsInventory = std::max(result.maxAbsInventory, std::abs(runningInventory));
    }

    const double meanInventory = sumInventory / numberOfSteps;
    result.inventoryVariance = std::max(0.0, sumSquaredInventory / numberOfSteps - meanInventory * meanInventory);
    result.meanAbsInventory = sumAbsoluteInventory / numberOfSteps;
    const double returnVariance = numberOfSteps > 1
        ? std::max(0.0, (sumSquaredReturns - sumReturns * sumReturns / numberOfSteps) / (numberOfSteps - 1)) : 0;
    result.stepSharpe = returnVariance > 0 ? (sumReturns / numberOfSteps) / std::sqrt(returnVariance) : 0;

    double totalSpreadCapture = 0;
    double totalAdverseSelection = 0;
    double totalExecutionMarkout = 0;
    result.markoutVolume = 0;
    result.censoredMarkoutVolume = 0;
    for (const Fill& fill : result.fills) {
        if (fill.liquidation) continue;
        totalSpreadCapture += fill.quantity * fill.direction * (fill.reference - fill.price);
        const size_t futureStepIndex = static_cast<size_t>(std::ceil((fill.time + config.markoutHorizon) / config.dt - 1e-9));
        if (futureStepIndex >= result.samples.size()) {
            result.censoredMarkoutVolume += fill.quantity;
            continue;
        }
        const double futureReferencePrice = result.samples[futureStepIndex].reference;
        totalAdverseSelection += -fill.quantity * fill.direction *
                                 (futureReferencePrice - fill.reference);
        totalExecutionMarkout += fill.quantity * fill.direction *
                                 (futureReferencePrice - fill.price);
        result.markoutVolume += fill.quantity;
    }
    result.spreadCapture = result.passiveVolume
        ? totalSpreadCapture / result.passiveVolume : 0;
    result.adverseSelection = result.markoutVolume
        ? totalAdverseSelection / result.markoutVolume : 0;
    result.executionMarkout = result.markoutVolume
        ? totalExecutionMarkout / result.markoutVolume : 0;
}

void writeSummaryHeader(std::ostream& output) {
    output << "group,regime,strategy,seed,seconds,dt,sigma,informed_fraction,half_spread,quote_size,"
           << "inventory_limit,net_pnl,pre_liquidation_pnl,liquidation_cost,fees,max_drawdown,"
           << "inventory_variance,mean_abs_inventory,max_abs_inventory,step_sharpe,passive_volume,"
           << "posted_volume,fill_rate,incoming_volume,unfilled_incoming_volume,spread_capture,"
           << "adverse_selection,execution_markout,markout_volume,censored_markout_volume,final_inventory\n";
}

void writeSummary(std::ostream& output, const Result& result, const std::string& group, const std::string& regime) {
    const Config& config = result.config;
    output << std::setprecision(12) << group << ',' << regime << ','
           << name(config.strategy) << ',' << config.seed << ',' << config.seconds << ','
           << config.dt << ',' << config.sigma << ',' << config.informedFraction << ','
           << config.halfSpread << ',' << config.quoteSize << ',' << config.inventoryLimit << ','
           << result.netPnl << ',' << result.preLiquidationPnl << ',' << result.liquidationCost << ','
           << result.fees << ',' << result.maxDrawdown << ',' << result.inventoryVariance << ','
           << result.meanAbsInventory << ',' << result.maxAbsInventory << ',' << result.stepSharpe << ','
           << result.passiveVolume << ',' << result.postedVolume << ',' << result.fillRate() << ','
           << result.incomingVolume << ',' << result.unfilledIncomingVolume << ','
           << result.spreadCapture << ',' << result.adverseSelection << ','
           << result.executionMarkout << ',' << result.markoutVolume << ','
           << result.censoredMarkoutVolume << ',' << result.finalInventory << '\n';
}

void writeDetails(const Result& result, const std::string& directory) {
    std::filesystem::create_directories(directory);
    std::ofstream trajectoryFile(std::filesystem::path(directory) / "trajectory.csv");
    std::ofstream fillsFile(std::filesystem::path(directory) / "fills.csv");
    if (!trajectoryFile || !fillsFile) {
        throw std::runtime_error("Cannot write detail CSV");
    }
    trajectoryFile << std::setprecision(12) << "time,reference,cash,equity,inventory,bid,ask\n";
    for (const Sample& sample : result.samples) {
        trajectoryFile << sample.time << ',' << sample.reference << ',' << sample.cash << ','
                       << sample.equity << ',' << sample.inventory << ',' << sample.bid << ','
                       << sample.ask << '\n';
    }
    fillsFile << std::setprecision(12)
              << "time,direction,quantity,price,reference,fee,liquidation,maker_id,taker_id\n";
    for (const Fill& fill : result.fills) {
        fillsFile << fill.time << ',' << fill.direction << ',' << fill.quantity << ','
                  << fill.price << ',' << fill.reference << ',' << fill.fee << ','
                  << fill.liquidation << ',' << fill.makerId << ',' << fill.takerId << '\n';
    }
    if (!trajectoryFile.good() || !fillsFile.good()) {
        throw std::runtime_error("Failed writing detail CSV");
    }
}

} // namespace market_making
