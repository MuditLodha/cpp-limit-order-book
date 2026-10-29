#include "matching_engine/MatchingEngine.hpp"
#include "matching_engine/ReferenceEngine.hpp"
#include <iostream>
#include <random>
#include <vector>
#include <cstdlib>
#include <iomanip>

using namespace matching_engine;

// Non-suppressible test assertion macro that cannot be disabled by -DNDEBUG
#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "\n[DIFFERENTIAL TEST FAILED] " << msg << "\n" \
                      << "  Condition: " #cond << "\n" \
                      << "  File: " << __FILE__ << ":" << __LINE__ << std::endl; \
            std::abort(); \
        } \
    } while (0)

int main(int argc, char** argv) {
    size_t numOperations = 50000;
    uint32_t seed = 42;

    if (argc > 1) {
        numOperations = std::stoull(argv[1]);
    }
    if (argc > 2) {
        seed = static_cast<uint32_t>(std::stoul(argv[2]));
    }

    std::cout << "========================================================\n";
    std::cout << " Limit Order Book - Randomized Differential Testing     \n";
    std::cout << " Operations: " << numOperations << " | Seed: " << seed << "\n";
    std::cout << " Features: Invalid Input Fuzzing + Deep Ladder Checks   \n";
    std::cout << "========================================================\n";

    constexpr Price MIN_PRICE = 1;
    constexpr Price MAX_PRICE = 200;
    OrderBookConfig config(MIN_PRICE, MAX_PRICE, 200000);
    MatchingEngine fastEngine(config);
    ReferenceEngine refEngine(MIN_PRICE, MAX_PRICE);

    std::vector<ExecutionReport> fastTrades;
    std::vector<ExecutionReport> refTrades;

    fastEngine.setTradeListener([&](const ExecutionReport& r) {
        fastTrades.push_back(r);
    });

    refEngine.setTradeListener([&](const ExecutionReport& r) {
        refTrades.push_back(r);
    });

    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> opDist(0, 99);
    std::uniform_int_distribution<int> sideDist(0, 1);
    std::uniform_int_distribution<Price> validPriceDist(90, 110);
    std::uniform_int_distribution<Quantity> validQtyDist(1, 50);

    // Generators for fuzzing invalid / edge-case inputs (Bugs 2 & 3 regression tests)
    std::uniform_int_distribution<int> fuzzDist(0, 99);
    std::uniform_int_distribution<Price> invalidPriceDist(201, 300); // Beyond MAX_PRICE

    std::vector<OrderId> activeOrderIds;
    OrderId nextOrderId = 1;

    for (size_t i = 1; i <= numOperations; ++i) {
        fastTrades.clear();
        refTrades.clear();

        const int op = opDist(rng);
        const Side side = (sideDist(rng) == 0) ? Side::Buy : Side::Sell;
        const Timestamp ts = static_cast<Timestamp>(i);
        const int fuzzChance = fuzzDist(rng);

        if (op < 55 || activeOrderIds.empty()) {
            // Limit Order (with ~8% invalid inputs: out-of-range prices, 0 qty, duplicate IDs)
            OrderId id = nextOrderId++;
            Price price;
            Quantity qty;

            if (fuzzChance < 3) {
                price = 0; // Invalid price 0
                qty = validQtyDist(rng);
            } else if (fuzzChance < 6) {
                price = invalidPriceDist(rng); // Out-of-range price > MAX_PRICE
                qty = validQtyDist(rng);
            } else if (fuzzChance < 8) {
                price = validPriceDist(rng);
                qty = 0; // Invalid qty 0
            } else if (fuzzChance < 10 && !activeOrderIds.empty()) {
                // Try submitting a duplicate of an existing active order ID
                id = activeOrderIds[rng() % activeOrderIds.size()];
                price = validPriceDist(rng);
                qty = validQtyDist(rng);
            } else {
                price = validPriceDist(rng);
                qty = validQtyDist(rng);
            }

            bool fRes = fastEngine.submitLimitOrder(id, side, price, qty, ts);
            bool rRes = refEngine.submitLimitOrder(id, side, price, qty, ts);
            CHECK(fRes == rRes, "Limit order acceptance divergence at step " + std::to_string(i));

            if (fastEngine.book().findOrder(id) != nullptr) {
                activeOrderIds.push_back(id);
            }
        } else if (op < 70) {
            // Market Order (with ~5% qty 0 or duplicate IDs)
            OrderId id = nextOrderId++;
            Quantity qty = (fuzzChance < 5) ? 0 : validQtyDist(rng);

            Quantity fFill = fastEngine.submitMarketOrder(id, side, qty, ts);
            Quantity rFill = refEngine.submitMarketOrder(id, side, qty, ts);
            CHECK(fFill == rFill, "Market order fill quantity divergence at step " + std::to_string(i));
        } else if (op < 85) {
            // Cancel Order (with ~10% cancellations of dead/non-existent IDs)
            OrderId id;
            if (fuzzChance < 10 || activeOrderIds.empty()) {
                id = 999999999ULL + (rng() % 1000); // Dead ID
                bool fCancel = fastEngine.cancelOrder(id);
                bool rCancel = refEngine.cancelOrder(id);
                CHECK(fCancel == rCancel, "Dead cancel order divergence at step " + std::to_string(i));
            } else {
                std::uniform_int_distribution<size_t> idxDist(0, activeOrderIds.size() - 1);
                size_t idx = idxDist(rng);
                id = activeOrderIds[idx];

                bool fCancel = fastEngine.cancelOrder(id);
                bool rCancel = refEngine.cancelOrder(id);
                CHECK(fCancel == rCancel, "Cancel order result divergence at step " + std::to_string(i));

                activeOrderIds[idx] = activeOrderIds.back();
                activeOrderIds.pop_back();
            }
        } else {
            // Modify Order (with ~10% invalid modifications: out-of-range price, qty 0)
            OrderId id;
            Price newPrice;
            Quantity newQty;

            if (fuzzChance < 5) {
                // Modify non-existent order
                id = 88888888ULL + (rng() % 500);
                newPrice = validPriceDist(rng);
                newQty = validQtyDist(rng);
            } else {
                std::uniform_int_distribution<size_t> idxDist(0, activeOrderIds.size() - 1);
                size_t idx = idxDist(rng);
                id = activeOrderIds[idx];

                if (fuzzChance < 10) {
                    newPrice = invalidPriceDist(rng); // Invalid out-of-range price
                    newQty = validQtyDist(rng);
                } else if (fuzzChance < 15) {
                    newPrice = validPriceDist(rng);
                    newQty = 0; // Invalid qty 0
                } else {
                    newPrice = validPriceDist(rng);
                    newQty = validQtyDist(rng);
                }
            }

            bool fMod = fastEngine.modifyOrder(id, newPrice, newQty, ts);
            bool rMod = refEngine.modifyOrder(id, newPrice, newQty, ts);
            CHECK(fMod == rMod, "Modify order result divergence at step " + std::to_string(i));
        }

        // 1. Assert identical trade count
        CHECK(fastTrades.size() == refTrades.size(),
              "Trade count mismatch at step " + std::to_string(i) +
              " (Fast: " + std::to_string(fastTrades.size()) +
              ", Ref: " + std::to_string(refTrades.size()) + ")");

        // 2. Assert identical trade executions
        for (size_t t = 0; t < fastTrades.size(); ++t) {
            const auto& ft = fastTrades[t];
            const auto& rt = refTrades[t];
            CHECK(ft.matchPrice == rt.matchPrice &&
                  ft.matchQuantity == rt.matchQuantity &&
                  ft.makerOrderId == rt.makerOrderId &&
                  ft.takerOrderId == rt.takerOrderId,
                  "Trade details mismatch at step " + std::to_string(i));
        }

        // 3. Assert Top-of-Book
        CHECK(fastEngine.book().bestBidPrice() == refEngine.bestBidPrice(),
              "Best Bid mismatch at step " + std::to_string(i) +
              " (Fast: " + std::to_string(fastEngine.book().bestBidPrice()) +
              ", Ref: " + std::to_string(refEngine.bestBidPrice()) + ")");

        CHECK(fastEngine.book().bestAskPrice() == refEngine.bestAskPrice(),
              "Best Ask mismatch at step " + std::to_string(i) +
              " (Fast: " + std::to_string(fastEngine.book().bestAskPrice()) +
              ", Ref: " + std::to_string(refEngine.bestAskPrice()) + ")");

        // 4. Periodically (every 250 ops), perform deep snapshot inspection:
        //    Volume totals, order counts, and top-5 depth levels
        if (i % 250 == 0 || i == numOperations) {
            CHECK(fastEngine.book().orderCount() == refEngine.orderCount(),
                  "Order count divergence at step " + std::to_string(i) +
                  " (Fast: " + std::to_string(fastEngine.book().orderCount()) +
                  ", Ref: " + std::to_string(refEngine.orderCount()) + ")");

            CHECK(fastEngine.book().totalBidVolume() == refEngine.totalBidVolume(),
                  "Total Bid Volume divergence at step " + std::to_string(i) +
                  " (Fast: " + std::to_string(fastEngine.book().totalBidVolume()) +
                  ", Ref: " + std::to_string(refEngine.totalBidVolume()) + ")");

            CHECK(fastEngine.book().totalAskVolume() == refEngine.totalAskVolume(),
                  "Total Ask Volume divergence at step " + std::to_string(i) +
                  " (Fast: " + std::to_string(fastEngine.book().totalAskVolume()) +
                  ", Ref: " + std::to_string(refEngine.totalAskVolume()) + ")");

            auto fAsks = fastEngine.book().getAsks(5);
            auto rAsks = refEngine.getAsks(5);
            CHECK(fAsks.size() == rAsks.size(), "Ask ladder depth level count mismatch at step " + std::to_string(i));
            for (size_t d = 0; d < fAsks.size(); ++d) {
                CHECK(fAsks[d].price == rAsks[d].price &&
                      fAsks[d].volume == rAsks[d].volume &&
                      fAsks[d].orderCount == rAsks[d].orderCount,
                      "Ask level " + std::to_string(d) + " mismatch at step " + std::to_string(i));
            }

            auto fBids = fastEngine.book().getBids(5);
            auto rBids = refEngine.getBids(5);
            CHECK(fBids.size() == rBids.size(), "Bid ladder depth level count mismatch at step " + std::to_string(i));
            for (size_t d = 0; d < fBids.size(); ++d) {
                CHECK(fBids[d].price == rBids[d].price &&
                      fBids[d].volume == rBids[d].volume &&
                      fBids[d].orderCount == rBids[d].orderCount,
                      "Bid level " + std::to_string(d) + " mismatch at step " + std::to_string(i));
            }
        }

        if (i % 10000 == 0 || i == numOperations) {
            std::cout << " Processed " << std::setw(6) << i << " operations. Status: IN SYNC!"
                      << " (Orders: " << fastEngine.book().orderCount()
                      << " | Bid Vol: " << fastEngine.book().totalBidVolume()
                      << " | Ask Vol: " << fastEngine.book().totalAskVolume()
                      << " | BBO: " << fastEngine.book().bestBidPrice()
                      << "/" << fastEngine.book().bestAskPrice() << ")\n";
        }
    }

    std::cout << "\n>>> DIFFERENTIAL FUZZING PASSED! 100% IDENTICAL BEHAVIOR! <<<\n";
    return 0;
}
