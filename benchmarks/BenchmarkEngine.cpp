#include "matching_engine/MatchingEngine.hpp"
#include "LatencyHistogram.hpp"
#include <iostream>
#include <vector>
#include <chrono>
#include <random>

using namespace matching_engine;

void benchmarkLimitOrderPassiveLatency(size_t numOrders = 500000) {
    OrderBookConfig config(1, 100000, 1000000);
    MatchingEngine engine(config);
    LatencyHistogram hist(numOrders);

    const double ghz = HighResolutionTimer::cyclesPerNanosecond();
    const uint64_t overhead = HighResolutionTimer::rdtscOverhead();

    // Warm-up cache
    for (size_t i = 1; i <= 5000; ++i) {
        engine.submitLimitOrder(9000000 + i, Side::Buy, static_cast<Price>(500 + (i % 100)), 10);
    }
    engine.reset();

    // Passive orders spread widely to prevent fills and measure pure book insertion overhead
    for (size_t i = 1; i <= numOrders; ++i) {
        const Price price = static_cast<Price>(1000 + (i % 500));
        const uint64_t t0 = HighResolutionTimer::rdtsc();
        engine.submitLimitOrder(i, Side::Buy, price, 10, t0);
        const uint64_t t1 = HighResolutionTimer::rdtsc();
        const uint64_t cycles = (t1 > t0 + overhead) ? (t1 - t0 - overhead) : 1;
        hist.record(cycles);
    }

    hist.printSummary("Passive Limit Order Insertion Latency", "ns", 1.0 / ghz);
}

void benchmarkAggressiveMatchingLatency(size_t numPairs = 200000) {
    OrderBookConfig config(1, 100000, 1000000);
    MatchingEngine engine(config);
    LatencyHistogram hist(numPairs);

    const double ghz = HighResolutionTimer::cyclesPerNanosecond();
    const uint64_t overhead = HighResolutionTimer::rdtscOverhead();

    // Pre-populate resting asks
    for (size_t i = 1; i <= numPairs; ++i) {
        const Price price = static_cast<Price>(5000 + (i % 100));
        engine.submitLimitOrder(i, Side::Sell, price, 10);
    }

    // Now execute aggressive buys that cross and match instantly
    for (size_t i = 1; i <= numPairs; ++i) {
        const OrderId takerId = numPairs + i;
        const Price price = static_cast<Price>(5000 + (i % 100));

        const uint64_t t0 = HighResolutionTimer::rdtsc();
        engine.submitLimitOrder(takerId, Side::Buy, price, 10, t0);
        const uint64_t t1 = HighResolutionTimer::rdtsc();
        const uint64_t cycles = (t1 > t0 + overhead) ? (t1 - t0 - overhead) : 1;
        hist.record(cycles);
    }

    hist.printSummary("Aggressive Limit Order Matching Latency", "ns", 1.0 / ghz);
}

void benchmarkCancellationLatency(size_t numOrders = 500000) {
    OrderBookConfig config(1, 100000, 1000000);
    MatchingEngine engine(config);
    LatencyHistogram hist(numOrders);

    const double ghz = HighResolutionTimer::cyclesPerNanosecond();
    const uint64_t overhead = HighResolutionTimer::rdtscOverhead();

    // Populate resting orders that are confirmed to be alive
    for (size_t i = 1; i <= numOrders; ++i) {
        const Price price = static_cast<Price>(1000 + (i % 1000));
        engine.submitLimitOrder(i, Side::Buy, price, 10);
    }

    // Measure cancellation latency on active resting orders
    for (size_t i = 1; i <= numOrders; ++i) {
        const uint64_t t0 = HighResolutionTimer::rdtsc();
        engine.cancelOrder(i);
        const uint64_t t1 = HighResolutionTimer::rdtsc();
        const uint64_t cycles = (t1 > t0 + overhead) ? (t1 - t0 - overhead) : 1;
        hist.record(cycles);
    }

    hist.printSummary("O(1) Live Order Cancellation Latency", "ns", 1.0 / ghz);
}

void benchmarkSustainedThroughput(size_t totalOps = 2000000) {
    // Workload 1: Mixed Trading (Limit, Market, Cancel)
    {
        OrderBookConfig config(1, 100000, 1000000);
        MatchingEngine engine(config);

        std::mt19937 rng(1337);
        std::uniform_int_distribution<int> opDist(0, 99);
        std::uniform_int_distribution<Price> priceDist(4950, 5050);
        std::uniform_int_distribution<Quantity> qtyDist(1, 50);

        std::vector<OrderId> activeIds;
        activeIds.reserve(totalOps);
        OrderId nextId = 1;

        std::cout << "\nRunning Mixed Trading Workload Throughput (" << totalOps << " operations)...\n";
        std::cout << " (60% Limit, 20% Market Sweep, 20% Cancel - Alternating Sides)\n";

        const auto start = std::chrono::high_resolution_clock::now();

        for (size_t i = 0; i < totalOps; ++i) {
            int op = opDist(rng);
            if (op < 60 || activeIds.empty()) {
                OrderId id = nextId++;
                Side side = (id % 2 == 0) ? Side::Buy : Side::Sell;
                engine.submitLimitOrder(id, side, priceDist(rng), qtyDist(rng));
                activeIds.push_back(id);
            } else if (op < 80) {
                OrderId id = nextId++;
                Side side = (id % 2 == 0) ? Side::Buy : Side::Sell;
                engine.submitMarketOrder(id, side, qtyDist(rng));
            } else {
                size_t idx = i % activeIds.size();
                engine.cancelOrder(activeIds[idx]);
            }
        }

        const auto end = std::chrono::high_resolution_clock::now();
        const std::chrono::duration<double> duration = end - start;
        const double opsPerSec = static_cast<double>(totalOps) / duration.count();

        std::cout << "=======================================================\n";
        std::cout << " Mixed Workload Throughput:\n";
        std::cout << " Total Operations: " << totalOps << "\n";
        std::cout << " Elapsed Time    : " << duration.count() << " seconds\n";
        std::cout << " Throughput      : " << std::fixed << std::setprecision(2)
                  << (opsPerSec / 1e6) << " Million ops/sec ("
                  << static_cast<uint64_t>(opsPerSec) << " orders/sec)\n";
        std::cout << " Avg Latency     : " << (duration.count() * 1e9 / totalOps) << " ns/op\n";
        std::cout << "=======================================================\n";
    }

    // Workload 2: Pure Passive Insertion (100% Resting Liquidity)
    {
        OrderBookConfig config(1, 100000, 1000000);
        MatchingEngine engine(config);

        std::cout << "\nRunning Passive Liquidity Workload (" << (totalOps / 2) << " orders)...\n";
        std::cout << " (100% Resting Bids - Zero Trades, Pure Book Insertion)\n";

        const auto start = std::chrono::high_resolution_clock::now();
        const size_t passiveOps = totalOps / 2;

        for (size_t i = 1; i <= passiveOps; ++i) {
            const Price price = static_cast<Price>(1000 + (i % 2000));
            engine.submitLimitOrder(i, Side::Buy, price, 10);
        }

        const auto end = std::chrono::high_resolution_clock::now();
        const std::chrono::duration<double> duration = end - start;
        const double opsPerSec = static_cast<double>(passiveOps) / duration.count();

        std::cout << "=======================================================\n";
        std::cout << " Passive Insertion Workload Throughput:\n";
        std::cout << " Total Orders    : " << passiveOps << "\n";
        std::cout << " Elapsed Time    : " << duration.count() << " seconds\n";
        std::cout << " Throughput      : " << std::fixed << std::setprecision(2)
                  << (opsPerSec / 1e6) << " Million orders/sec\n";
        std::cout << " Avg Latency     : " << (duration.count() * 1e9 / passiveOps) << " ns/order\n";
        std::cout << "=======================================================\n";
    }
}

int runBenchmarks() {
    std::cout << "=======================================================\n";
    std::cout << "  High-Frequency Limit Order Book & Engine Benchmark   \n";
    std::cout << "  Timer: Calibrated CPU RDTSC (Sub-nanosecond accuracy)\n";
    std::cout << "  CPU Clock: " << std::fixed << std::setprecision(3)
              << HighResolutionTimer::cyclesPerNanosecond() << " GHz | RDTSC Overhead: "
              << HighResolutionTimer::rdtscOverhead() << " cycles\n";
    std::cout << "=======================================================\n";

    benchmarkLimitOrderPassiveLatency(500000);
    benchmarkAggressiveMatchingLatency(200000);
    benchmarkCancellationLatency(500000);
    benchmarkSustainedThroughput(2000000);

    return 0;
}
