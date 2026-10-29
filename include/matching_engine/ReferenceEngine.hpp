#pragma once

#include "Types.hpp"
#include <map>
#include <list>
#include <unordered_map>
#include <vector>
#include <functional>

namespace matching_engine {

/**
 * @brief Plain standard-library reference implementation used as an oracle for differential testing.
 *
 * Implements the exact same price-time priority rules using std::map and std::list.
 * Simple, intuitive, and independently verified to assert correctness of the
 * low-latency, array-indexed, memory-pooled MatchingEngine.
 */
class ReferenceEngine {
public:
    struct RefOrder {
        OrderId   id;
        Price     price;
        Quantity  remainingQuantity;
        Side      side;
        Timestamp timestamp;

        RefOrder(OrderId i = INVALID_ORDER_ID, Price p = INVALID_PRICE, Quantity q = 0, Side s = Side::Buy, Timestamp t = 0)
            : id(i), price(p), remainingQuantity(q), side(s), timestamp(t) {}
    };

    using TradeListener = std::function<void(const ExecutionReport&)>;

    explicit ReferenceEngine(Price minP = 1, Price maxP = 100000)
        : minPrice_(minP), maxPrice_(maxP) {}

    void setTradeListener(TradeListener listener) { tradeListener_ = listener; }

    bool isPriceValid(Price price) const {
        return price >= minPrice_ && price <= maxPrice_;
    }

    bool submitLimitOrder(OrderId id, Side side, Price price, Quantity qty, Timestamp ts = 0);
    Quantity submitMarketOrder(OrderId id, Side side, Quantity qty, Timestamp ts = 0);
    bool cancelOrder(OrderId id);
    bool modifyOrder(OrderId id, Price newPrice, Quantity newQty, Timestamp ts = 0);

    Price bestBidPrice() const;
    Price bestAskPrice() const;
    Quantity totalBidVolume() const;
    Quantity totalAskVolume() const;
    size_t orderCount() const { return ordersMap_.size(); }

    struct LevelSummary {
        Price price;
        Quantity volume;
        uint32_t orderCount;

        LevelSummary(Price p = 0, Quantity v = 0, uint32_t c = 0)
            : price(p), volume(v), orderCount(c) {}
    };

    std::vector<LevelSummary> getBids(size_t depth = 5) const;
    std::vector<LevelSummary> getAsks(size_t depth = 5) const;

    void reset();

private:
    using OrderQueue = std::list<RefOrder>;
    using BidMap     = std::map<Price, OrderQueue, std::greater<Price>>;
    using AskMap     = std::map<Price, OrderQueue, std::less<Price>>;

    struct OrderLocation {
        Side side;
        Price price;
        OrderQueue::iterator it;

        OrderLocation() : side(Side::Buy), price(0) {}
        OrderLocation(Side s, Price p, OrderQueue::iterator i)
            : side(s), price(p), it(i) {}
    };

    Price minPrice_;
    Price maxPrice_;
    BidMap bids_;
    AskMap asks_;
    std::unordered_map<OrderId, OrderLocation> ordersMap_;
    TradeListener tradeListener_;
};

} // namespace matching_engine
