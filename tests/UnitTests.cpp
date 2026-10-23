#include "matching_engine/Types.hpp"
#include "matching_engine/MemoryPool.hpp"
#include "matching_engine/IntrusiveList.hpp"
#include "matching_engine/Order.hpp"
#include "matching_engine/OrderBook.hpp"
#include "matching_engine/MatchingEngine.hpp"

#include <iostream>
#include <cstdlib>
#include <vector>
#include <stdexcept>

using namespace matching_engine;

// Non-suppressible test assertions that cannot be compiled away by -DNDEBUG
#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::cerr << "\n[TEST FAILED] CHECK(" #cond ") failed at " \
                      << __FILE__ << ":" << __LINE__ << std::endl; \
            std::abort(); \
        } \
    } while (0)

#define CHECK_EQ(a, b) \
    do { \
        if (!((a) == (b))) { \
            std::cerr << "\n[TEST FAILED] CHECK_EQ(" #a ", " #b ") failed: (" \
                      << (a) << " != " << (b) << ") at " \
                      << __FILE__ << ":" << __LINE__ << std::endl; \
            std::abort(); \
        } \
    } while (0)

void testMemoryPool() {
    std::cout << "[RUNNING] testMemoryPool..." << std::endl;
    constexpr size_t POOL_SIZE = 100;
    MemoryPool<Order> pool(POOL_SIZE);

    CHECK_EQ(pool.capacity(), POOL_SIZE);
    CHECK_EQ(pool.available(), POOL_SIZE);

    std::vector<Order*> allocated;
    for (size_t i = 0; i < POOL_SIZE; ++i) {
        Order* ord = pool.allocate(i + 1, 100, 10, Side::Buy, OrderType::Limit);
        CHECK(ord != nullptr);
        CHECK(pool.owns(ord));
        // Verify 64-byte alignment
        CHECK_EQ(reinterpret_cast<uintptr_t>(ord) % 64, 0);
        allocated.push_back(ord);
    }

    CHECK_EQ(pool.available(), 0);
    // Exhaustion check
    CHECK(pool.allocate(999, 100, 10, Side::Buy, OrderType::Limit) == nullptr);

    // Deallocate half
    for (size_t i = 0; i < 50; ++i) {
        pool.deallocate(allocated[i]);
    }
    CHECK_EQ(pool.available(), 50);

    // Reallocate
    for (size_t i = 0; i < 50; ++i) {
        Order* ord = pool.allocate(200 + i, 101, 5, Side::Sell, OrderType::Limit);
        CHECK(ord != nullptr);
    }
    CHECK_EQ(pool.available(), 0);

    std::cout << "[PASSED] testMemoryPool" << std::endl;
}

void testIntrusiveList() {
    std::cout << "[RUNNING] testIntrusiveList..." << std::endl;
    IntrusiveList<Order> list;
    CHECK(list.empty());
    CHECK_EQ(list.size(), 0);

    Order o1(1, 100, 10, Side::Buy, OrderType::Limit);
    Order o2(2, 100, 20, Side::Buy, OrderType::Limit);
    Order o3(3, 100, 30, Side::Buy, OrderType::Limit);

    list.push_back(&o1);
    list.push_back(&o2);
    list.push_back(&o3);

    CHECK_EQ(list.size(), 3);
    CHECK_EQ(list.front()->id, 1);
    CHECK_EQ(list.back()->id, 3);

    // Remove middle (o2)
    list.remove(&o2);
    CHECK_EQ(list.size(), 2);
    CHECK_EQ(list.front()->id, 1);
    CHECK_EQ(list.front()->next->id, 3);

    // Pop front (o1)
    Order* popped = list.pop_front();
    CHECK_EQ(popped->id, 1);
    CHECK_EQ(list.size(), 1);
    CHECK_EQ(list.front()->id, 3);

    // Pop last
    popped = list.pop_front();
    CHECK_EQ(popped->id, 3);
    CHECK(list.empty());

    std::cout << "[PASSED] testIntrusiveList" << std::endl;
}

void testPriceTimePriorityFIFO() {
    std::cout << "[RUNNING] testPriceTimePriorityFIFO..." << std::endl;
    OrderBookConfig config(1, 1000, 10000);
    MatchingEngine engine(config);

    std::vector<ExecutionReport> trades;
    engine.setTradeListener([&](const ExecutionReport& report) {
        trades.push_back(report);
    });

    // Resting bids at price 100: Order 1 arrives first, Order 2 arrives second
    engine.submitLimitOrder(1, Side::Buy, 100, 50);
    engine.submitLimitOrder(2, Side::Buy, 100, 50);

    CHECK_EQ(engine.book().bestBidPrice(), 100);
    CHECK_EQ(engine.book().orderCount(), 2);

    // Incoming aggressive sell order matches at price 100 for 70 shares
    // Must fill Order 1 (50 shares) first, then Order 2 (20 shares)
    engine.submitLimitOrder(3, Side::Sell, 100, 70);

    CHECK_EQ(trades.size(), 2);
    // Fill 1
    CHECK_EQ(trades[0].makerOrderId, 1);
    CHECK_EQ(trades[0].takerOrderId, 3);
    CHECK_EQ(trades[0].matchPrice, 100);
    CHECK_EQ(trades[0].matchQuantity, 50);

    // Fill 2
    CHECK_EQ(trades[1].makerOrderId, 2);
    CHECK_EQ(trades[1].takerOrderId, 3);
    CHECK_EQ(trades[1].matchPrice, 100);
    CHECK_EQ(trades[1].matchQuantity, 20);

    // Order 2 should have 30 shares remaining
    Order* remaining = engine.book().findOrder(2);
    CHECK(remaining != nullptr);
    CHECK_EQ(remaining->remainingQuantity, 30);
    CHECK_EQ(engine.book().orderCount(), 1);

    std::cout << "[PASSED] testPriceTimePriorityFIFO" << std::endl;
}

void testMarketOrderSweeps() {
    std::cout << "[RUNNING] testMarketOrderSweeps..." << std::endl;
    OrderBookConfig config(1, 1000, 10000);
    MatchingEngine engine(config);

    std::vector<ExecutionReport> trades;
    engine.setTradeListener([&](const ExecutionReport& report) {
        trades.push_back(report);
    });

    // Post asks at price 101 (qty 10), 102 (qty 20), 103 (qty 30)
    engine.submitLimitOrder(1, Side::Sell, 101, 10);
    engine.submitLimitOrder(2, Side::Sell, 102, 20);
    engine.submitLimitOrder(3, Side::Sell, 103, 30);

    // Incoming Market Buy for 25 shares should consume 10 @ 101 and 15 @ 102
    Quantity filled = engine.submitMarketOrder(10, Side::Buy, 25);
    CHECK_EQ(filled, 25);
    CHECK_EQ(trades.size(), 2);
    CHECK_EQ(trades[0].matchPrice, 101);
    CHECK_EQ(trades[0].matchQuantity, 10);
    CHECK_EQ(trades[1].matchPrice, 102);
    CHECK_EQ(trades[1].matchQuantity, 15);

    CHECK_EQ(engine.book().bestAskPrice(), 102);

    std::cout << "[PASSED] testMarketOrderSweeps" << std::endl;
}

void testOrderCancelAndModify() {
    std::cout << "[RUNNING] testOrderCancelAndModify..." << std::endl;
    OrderBookConfig config(1, 1000, 10000);
    MatchingEngine engine(config);

    engine.submitLimitOrder(1, Side::Buy, 100, 50);
    engine.submitLimitOrder(2, Side::Buy, 100, 50);

    // Modify Order 1 quantity decrease (50 -> 30) => keeps priority
    bool modSuccess = engine.modifyOrder(1, 100, 30);
    CHECK(modSuccess);
    CHECK_EQ(engine.book().findOrder(1)->remainingQuantity, 30);

    // Cancel Order 2
    bool cancelSuccess = engine.cancelOrder(2);
    CHECK(cancelSuccess);
    CHECK(engine.book().findOrder(2) == nullptr);
    CHECK_EQ(engine.book().orderCount(), 1);

    // Cancel Order 1 -> book should now be empty
    engine.cancelOrder(1);
    CHECK_EQ(engine.book().bestBidPrice(), INVALID_PRICE);
    CHECK_EQ(engine.book().orderCount(), 0);

    std::cout << "[PASSED] testOrderCancelAndModify" << std::endl;
}

// Regression test for Issue 2: Invalid modifyOrder parameters must NOT destroy resting order
void testModifyOrderSafety() {
    std::cout << "[RUNNING] testModifyOrderSafety..." << std::endl;
    OrderBookConfig config(1, 200, 1000);
    MatchingEngine engine(config);

    std::vector<std::pair<OrderId, RejectReason>> rejects;
    engine.setRejectListener([&](OrderId id, RejectReason reason) {
        rejects.push_back({id, reason});
    });

    // Resting order #1 at price 100, qty 10
    CHECK(engine.submitLimitOrder(1, Side::Buy, 100, 10));
    CHECK_EQ(engine.book().orderCount(), 1);

    // Attempt modify to out-of-bounds price (999 > maxPrice 200)
    bool modOut = engine.modifyOrder(1, 999, 10);
    CHECK(!modOut);
    CHECK_EQ(rejects.size(), 1);
    CHECK_EQ(static_cast<uint8_t>(rejects[0].second), static_cast<uint8_t>(RejectReason::PriceOutOfRange));

    // CRITICAL: Order #1 must STILL exist in book with original values!
    Order* resting = engine.book().findOrder(1);
    CHECK(resting != nullptr);
    CHECK_EQ(resting->price, 100);
    CHECK_EQ(resting->remainingQuantity, 10);
    CHECK_EQ(engine.book().bestBidPrice(), 100);

    // Attempt modify to qty 0
    bool modZero = engine.modifyOrder(1, 100, 0);
    CHECK(!modZero);
    CHECK_EQ(rejects.size(), 2);
    CHECK_EQ(static_cast<uint8_t>(rejects[1].second), static_cast<uint8_t>(RejectReason::InvalidQuantity));

    // Order #1 must STILL be alive
    resting = engine.book().findOrder(1);
    CHECK(resting != nullptr);
    CHECK_EQ(resting->remainingQuantity, 10);

    std::cout << "[PASSED] testModifyOrderSafety" << std::endl;
}

// Regression test for Issue 3: Proper RejectReasons for out-of-range prices & invalid parameters
void testRejectReasons() {
    std::cout << "[RUNNING] testRejectReasons..." << std::endl;
    OrderBookConfig config(10, 500, 1000);
    MatchingEngine engine(config);

    std::vector<std::pair<OrderId, RejectReason>> rejects;
    engine.setRejectListener([&](OrderId id, RejectReason reason) {
        rejects.push_back({id, reason});
    });

    // 1. Price below minPrice
    CHECK(!engine.submitLimitOrder(1, Side::Buy, 5, 10));
    CHECK_EQ(static_cast<uint8_t>(rejects.back().second), static_cast<uint8_t>(RejectReason::PriceOutOfRange));

    // 2. Price above maxPrice
    CHECK(!engine.submitLimitOrder(2, Side::Buy, 501, 10));
    CHECK_EQ(static_cast<uint8_t>(rejects.back().second), static_cast<uint8_t>(RejectReason::PriceOutOfRange));

    // 3. Price 0
    CHECK(!engine.submitLimitOrder(3, Side::Buy, 0, 10));
    CHECK_EQ(static_cast<uint8_t>(rejects.back().second), static_cast<uint8_t>(RejectReason::PriceOutOfRange));

    // 4. Quantity 0
    CHECK(!engine.submitLimitOrder(4, Side::Buy, 100, 0));
    CHECK_EQ(static_cast<uint8_t>(rejects.back().second), static_cast<uint8_t>(RejectReason::InvalidQuantity));

    // 5. Valid order #10
    CHECK(engine.submitLimitOrder(10, Side::Buy, 100, 25));

    // 6. Duplicate Order ID #10
    CHECK(!engine.submitLimitOrder(10, Side::Buy, 105, 10));
    CHECK_EQ(static_cast<uint8_t>(rejects.back().second), static_cast<uint8_t>(RejectReason::DuplicateOrderId));

    // 7. Cancel non-existent order
    CHECK(!engine.cancelOrder(9999));
    CHECK_EQ(static_cast<uint8_t>(rejects.back().second), static_cast<uint8_t>(RejectReason::OrderNotFound));

    std::cout << "[PASSED] testRejectReasons" << std::endl;
}

// Test fast bitset tracking across sparse price levels (Issue 5)
void testConfiguredOrderCapacity() {
    std::cout << "[RUNNING] testConfiguredOrderCapacity..." << std::endl;
    OrderBookConfig config(1, 100, 1);
    MatchingEngine engine(config);

    std::vector<RejectReason> rejects;
    engine.setRejectListener([&](OrderId, RejectReason reason) {
        rejects.push_back(reason);
    });

    CHECK(engine.submitLimitOrder(1, Side::Buy, 50, 10));
    CHECK_EQ(engine.book().orderCount(), 1);

    // maxOrders must be honored by the actual pool, not just stored in config.
    CHECK(!engine.submitLimitOrder(2, Side::Buy, 49, 10));
    CHECK_EQ(engine.book().orderCount(), 1);
    CHECK(engine.book().findOrder(1) != nullptr);
    CHECK(engine.book().findOrder(2) == nullptr);
    CHECK(!rejects.empty());
    CHECK_EQ(static_cast<uint8_t>(rejects.back()),
             static_cast<uint8_t>(RejectReason::CapacityExceeded));

    CHECK(engine.cancelOrder(1));
    CHECK(engine.submitLimitOrder(2, Side::Buy, 49, 10));
    CHECK(engine.book().findOrder(2) != nullptr);

    std::cout << "[PASSED] testConfiguredOrderCapacity" << std::endl;
}

void testInvalidLimitPriceDoesNotTrade() {
    std::cout << "[RUNNING] testInvalidLimitPriceDoesNotTrade..." << std::endl;
    OrderBookConfig config(1, 200, 10);
    MatchingEngine engine(config);
    std::vector<ExecutionReport> trades;
    engine.setTradeListener([&](const ExecutionReport& report) {
        trades.push_back(report);
    });

    CHECK(engine.submitLimitOrder(1, Side::Buy, 100, 10));
    CHECK(!engine.submitLimitOrder(2, Side::Sell, 0, 10));
    CHECK(trades.empty());
    CHECK_EQ(engine.book().bestBidPrice(), 100);
    CHECK_EQ(engine.book().orderCount(), 1);
    CHECK(engine.book().findOrder(1) != nullptr);

    std::cout << "[PASSED] testInvalidLimitPriceDoesNotTrade" << std::endl;
}

void testInvalidOrderIdIsRejected() {
    std::cout << "[RUNNING] testInvalidOrderIdIsRejected..." << std::endl;
    OrderBookConfig config(1, 100, 10);
    MatchingEngine engine(config);
    std::vector<RejectReason> rejects;
    std::vector<ExecutionReport> trades;
    engine.setRejectListener([&](OrderId, RejectReason reason) {
        rejects.push_back(reason);
    });
    engine.setTradeListener([&](const ExecutionReport& report) {
        trades.push_back(report);
    });

    CHECK(engine.submitLimitOrder(1, Side::Sell, 50, 10));
    CHECK(!engine.submitLimitOrder(INVALID_ORDER_ID, Side::Buy, 50, 10));
    CHECK_EQ(engine.submitMarketOrder(INVALID_ORDER_ID, Side::Buy, 10), 0);
    CHECK(trades.empty());
    CHECK_EQ(rejects.size(), 2);
    CHECK_EQ(static_cast<uint8_t>(rejects[0]),
             static_cast<uint8_t>(RejectReason::InvalidOrderId));
    CHECK_EQ(static_cast<uint8_t>(rejects[1]),
             static_cast<uint8_t>(RejectReason::InvalidOrderId));
    CHECK_EQ(engine.book().orderCount(), 1);

    std::cout << "[PASSED] testInvalidOrderIdIsRejected" << std::endl;
}

void testInvalidOrderBookConfiguration() {
    std::cout << "[RUNNING] testInvalidOrderBookConfiguration..." << std::endl;
    bool rejectedZeroMin = false;
    try {
        OrderBookConfig config(0, 100, 10);
        OrderBook book(config);
    } catch (const std::invalid_argument&) {
        rejectedZeroMin = true;
    }
    CHECK(rejectedZeroMin);

    bool rejectedReversedRange = false;
    try {
        OrderBookConfig config(100, 1, 10);
        OrderBook book(config);
    } catch (const std::invalid_argument&) {
        rejectedReversedRange = true;
    }
    CHECK(rejectedReversedRange);

    std::cout << "[PASSED] testInvalidOrderBookConfiguration" << std::endl;
}

void testSparseBookBitsetScan() {
    std::cout << "[RUNNING] testSparseBookBitsetScan..." << std::endl;
    OrderBookConfig config(1, 100000, 10000);
    MatchingEngine engine(config);

    // Place bid at 50, and bid at 95000 (very sparse ladder)
    CHECK(engine.submitLimitOrder(1, Side::Buy, 50, 10));
    CHECK(engine.submitLimitOrder(2, Side::Buy, 95000, 10));

    CHECK_EQ(engine.book().bestBidPrice(), 95000);

    // Cancel the high bid at 95000
    CHECK(engine.cancelOrder(2));

    // Best bid must immediately update to 50 using bitset word scanning
    CHECK_EQ(engine.book().bestBidPrice(), 50);

    std::cout << "[PASSED] testSparseBookBitsetScan" << std::endl;
}

int runUnitTests() {
    std::cout << "========================================" << std::endl;
    std::cout << " Running Matching Engine Unit Tests     " << std::endl;
    std::cout << "========================================" << std::endl;

    testMemoryPool();
    testIntrusiveList();
    testPriceTimePriorityFIFO();
    testMarketOrderSweeps();
    testOrderCancelAndModify();
    testModifyOrderSafety();
    testRejectReasons();
    testConfiguredOrderCapacity();
    testInvalidLimitPriceDoesNotTrade();
    testInvalidOrderIdIsRejected();
    testInvalidOrderBookConfiguration();
    testSparseBookBitsetScan();

    std::cout << "\n>>> ALL UNIT TESTS PASSED SUCCESSFULLY! <<<" << std::endl;
    return 0;
}
