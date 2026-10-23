#pragma once

#include "Types.hpp"
#include "OrderBook.hpp"
#include <functional>
#include <vector>

namespace matching_engine {

/**
 * @brief High-performance Matching Engine implementing Price-Time (FIFO) priority.
 *
 * Coordinates order validation, matching aggressive incoming orders against resting
 * liquidity, generating execution reports, and maintaining the order book state.
 */
class MatchingEngine {
public:
    using TradeListener  = std::function<void(const ExecutionReport&)>;
    using CancelListener = std::function<void(OrderId)>;
    using RejectListener = std::function<void(OrderId, RejectReason)>;

    explicit MatchingEngine(const OrderBookConfig& config = OrderBookConfig());
    ~MatchingEngine() = default;

    // Callbacks / Event Handlers
    void setTradeListener(TradeListener listener)   { tradeListener_ = listener; }
    void setCancelListener(CancelListener listener) { cancelListener_ = listener; }
    void setRejectListener(RejectListener listener) { rejectListener_ = listener; }

    /**
     * @brief Process an incoming Limit Order.
     * Matches aggressively if crossing the spread, then rests any remainder.
     */
    bool submitLimitOrder(OrderId id, Side side, Price price, Quantity qty, Timestamp ts = 0);

    /**
     * @brief Process an incoming Market Order.
     * Sweeps the book at available prices until filled or liquidity exhausted.
     */
    Quantity submitMarketOrder(OrderId id, Side side, Quantity qty, Timestamp ts = 0);

    /**
     * @brief Cancel a resting order by OrderId.
     */
    bool cancelOrder(OrderId id);

    /**
     * @brief Modify an active order's price and/or quantity.
     * Price changes or quantity increases lose queue priority.
     * Quantity decreases preserve queue priority.
     */
    bool modifyOrder(OrderId id, Price newPrice, Quantity newQty, Timestamp ts = 0);

    const OrderBook& book() const { return book_; }
    OrderBook& book()             { return book_; }

    void reset();

private:
    Quantity executeAggressiveMatch(OrderId id, Side side, Price price, Quantity qty, bool isMarket, Timestamp ts);

    OrderBook book_;
    TradeListener tradeListener_;
    CancelListener cancelListener_;
    RejectListener rejectListener_;
};

} // namespace matching_engine
