#pragma once

#include "Types.hpp"
#include "Order.hpp"
#include "PriceLevel.hpp"
#include "MemoryPool.hpp"
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace matching_engine {

/**
 * @brief Configuration parameters for the Array-Indexed Limit Order Book.
 */
struct OrderBookConfig {
    Price  minPrice;
    Price  maxPrice;     // 100,000 tick levels (e.g. $0.01 to $1,000.00)
    size_t maxOrders;    // Resting orders pre-allocated in pool

    OrderBookConfig(Price minP = 1, Price maxP = 100000, size_t maxO = 1000000)
        : minPrice(minP), maxPrice(maxP), maxOrders(maxO) {}
};

/**
 * @brief Ultra-low-latency Limit Order Book utilizing array-indexed price levels.
 *
 * Key Architectural Properties:
 * 1. Array-Indexed Price Ladder: O(1) direct array indexing `price - minPrice`
 *    avoiding RB-tree pointer-chasing and binary search cache misses.
 * 2. Zero-Allocation Hot Path: All Order nodes are pre-allocated via MemoryPool.
 * 3. Price-Time Priority: Orders at each tick are queued using IntrusiveList.
 * 4. Fast Best-Bid / Best-Ask Tracking: 64-bit word bitmap scanning across levels.
 */
class OrderBook {
public:
    explicit OrderBook(const OrderBookConfig& config = OrderBookConfig());
    ~OrderBook();

    // Non-copyable, non-movable
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    /**
     * @brief Allocate and insert a resting limit order into the book in O(1).
     */
    Order* addOrder(OrderId id, Price price, Quantity qty, Side side, OrderType type, Timestamp ts = 0);

    /**
     * @brief Cancel an active resting order by ID in O(1).
     * @return true if found and canceled, false otherwise.
     */
    
    bool cancelOrder(OrderId id);

    /**
     * @brief Modify an active order's price and/or quantity.
     * Price change or quantity increase loses queue priority.
     * Quantity reduction maintains queue priority.
     */
    bool modifyOrder(OrderId id, Price newPrice, Quantity newQty, Timestamp ts = 0);

    /**
     * @brief Find an active order by ID.
     */
    Order* findOrder(OrderId id) const;

    /**
     * @brief Remove order from book and return it to the memory pool.
     */
    void removeOrder(Order* order);

    /**
     * @brief Get best bid price tick, or INVALID_PRICE if no bids.
     */
    Price bestBidPrice() const { return bestBidPrice_; }

    /**
     * @brief Get best ask price tick, or INVALID_PRICE if no asks.
     */
    Price bestAskPrice() const { return bestAskPrice_; }

    /**
     * @brief Get pointer to the best bid PriceLevel.
     */
    PriceLevel* bestBidLevel();

    /**
     * @brief Get pointer to the best ask PriceLevel.
     */
    PriceLevel* bestAskLevel();

    /**
     * @brief Access PriceLevel at exact tick in O(1).
     */
    PriceLevel* getLevel(Side side, Price price);

    const OrderBookConfig& config() const { return config_; }
    size_t orderCount() const { return activeOrders_.size(); }

    /**
     * @brief Update the cached best bid after matching, cancel, or modify.
     */
    void updateBestBid();

    /**
     * @brief Update the cached best ask after matching, cancel, or modify.
     */
    void updateBestAsk();

    /**
     * @brief Reset entire order book to clean state.
     */
    void clear();

    bool isPriceValid(Price price) const {
        return price >= config_.minPrice && price <= config_.maxPrice;
    }

    Quantity totalBidVolume() const;
    Quantity totalAskVolume() const;

    /**
     * @brief Inspect top of book up to specified depth.
     */
    struct LevelSummary {
        Price price;
        Quantity volume;
        uint32_t orderCount;

        LevelSummary(Price p = 0, Quantity v = 0, uint32_t c = 0)
            : price(p), volume(v), orderCount(c) {}
    };

    std::vector<LevelSummary> getBids(size_t depth = 5) const;
    std::vector<LevelSummary> getAsks(size_t depth = 5) const;

private:
    size_t priceToIndex(Price price) const {
        return static_cast<size_t>(price - config_.minPrice);
    }

    static void setBit(std::vector<uint64_t>& bitmap, size_t idx) {
        bitmap[idx / 64] |= (1ULL << (idx % 64));
    }

    static void clearBit(std::vector<uint64_t>& bitmap, size_t idx) {
        bitmap[idx / 64] &= ~(1ULL << (idx % 64));
    }

    OrderBookConfig config_;
    size_t numLevels_;

    // Flat, cache-friendly array of price levels for Bids and Asks
    std::vector<PriceLevel> bidLevels_;
    std::vector<PriceLevel> askLevels_;

    // Fast 64-level bitset skip arrays for O(1) best bid/ask tracking
    std::vector<uint64_t> bidBitmap_;
    std::vector<uint64_t> askBitmap_;

    // Fast order lookup by OrderId
    std::unordered_map<OrderId, Order*> activeOrders_;

    // Memory pool for orders - 1,000,000 orders capacity
    MemoryPool<Order, 1000000> orderPool_;

    // Cached top-of-book prices
    Price bestBidPrice_;
    Price bestAskPrice_;
};

} // namespace matching_engine
