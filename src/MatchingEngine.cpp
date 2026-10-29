#include "matching_engine/MatchingEngine.hpp"
#include <algorithm>

namespace matching_engine {

MatchingEngine::MatchingEngine(const OrderBookConfig& config)
    : book_(config) {}

Quantity MatchingEngine::executeAggressiveMatch(OrderId id, Side side, Price price, Quantity qty, bool isMarket, Timestamp ts) {
    Quantity remaining = qty;
    while (remaining > 0) {
        Price bestPrice;
        PriceLevel* level = NULL;

        if (side == Side::Buy) {
            bestPrice = book_.bestAskPrice();
            if (bestPrice == INVALID_PRICE || (!isMarket && price < bestPrice)) {
                break;
            }
            level = book_.bestAskLevel();
        } else {
            bestPrice = book_.bestBidPrice();
            if (bestPrice == INVALID_PRICE || (!isMarket && price > bestPrice)) {
                break;
            }
            level = book_.bestBidLevel();
        }

        if (level == NULL || level->empty()) {
            break;
        }

        Order* maker = level->frontOrder();
        const Quantity matchQty = std::min(remaining, maker->remainingQuantity);
        const Price matchPrice = maker->price;

        maker->fill(matchQty);
        level->reduceVolume(matchQty);
        remaining -= matchQty;

        if (tradeListener_) {
            ExecutionReport report;
            report.makerOrderId = maker->id;
            report.takerOrderId = id;
            report.takerSide = side;
            report.matchPrice = matchPrice;
            report.matchQuantity = matchQty;
            report.makerRemainingQuantity = maker->remainingQuantity;
            report.takerRemainingQuantity = remaining;
            report.timestamp = ts;
            tradeListener_(report);
        }

        if (maker->isFilled()) {
            book_.removeOrder(maker);
        }
    }
    return remaining;
}

bool MatchingEngine::submitLimitOrder(OrderId id, Side side, Price price, Quantity qty, Timestamp ts) {
    if (qty == 0) {
        if (rejectListener_) rejectListener_(id, RejectReason::InvalidQuantity);
        return false;
    }

    if (!book_.isPriceValid(price)) {
        if (rejectListener_) rejectListener_(id, RejectReason::PriceOutOfRange);
        return false;
    }

    if (book_.findOrder(id) != NULL) {
        if (rejectListener_) rejectListener_(id, RejectReason::DuplicateOrderId);
        return false;
    }

    Quantity remaining = executeAggressiveMatch(id, side, price, qty, false, ts);

    // Rest any unfilled remaining quantity
    if (remaining > 0) {
        Order* order = book_.addOrder(id, price, remaining, side, OrderType::Limit, ts);
        if (order == NULL) {
            if (rejectListener_) rejectListener_(id, RejectReason::CapacityExceeded);
            return false;
        }
    }

    return true;
}

Quantity MatchingEngine::submitMarketOrder(OrderId id, Side side, Quantity qty, Timestamp ts) {
    if (qty == 0) {
        if (rejectListener_) rejectListener_(id, RejectReason::InvalidQuantity);
        return 0;
    }

    if (book_.findOrder(id) != NULL) {
        if (rejectListener_) rejectListener_(id, RejectReason::DuplicateOrderId);
        return 0;
    }

    Quantity remaining = executeAggressiveMatch(id, side, INVALID_PRICE, qty, true, ts);

    const Quantity filled = qty - remaining;
    if (remaining > 0 && rejectListener_) {
        // Any unfillable market order remainder expires immediately
        rejectListener_(id, RejectReason::NoLiquidity);
    }

    return filled;
}

bool MatchingEngine::cancelOrder(OrderId id) {
    const bool success = book_.cancelOrder(id);
    if (success) {
        if (cancelListener_) cancelListener_(id);
    } else {
        if (rejectListener_) rejectListener_(id, RejectReason::OrderNotFound);
    }
    return success;
}

bool MatchingEngine::modifyOrder(OrderId id, Price newPrice, Quantity newQty, Timestamp ts) {
    Order* existing = book_.findOrder(id);
    if (existing == NULL) {
        if (rejectListener_) rejectListener_(id, RejectReason::OrderNotFound);
        return false;
    }

    // Validate parameters BEFORE modifying or canceling to ensure resting orders are never destroyed!
    if (newQty == 0) {
        if (rejectListener_) rejectListener_(id, RejectReason::InvalidQuantity);
        return false;
    }

    if (!book_.isPriceValid(newPrice)) {
        if (rejectListener_) rejectListener_(id, RejectReason::PriceOutOfRange);
        return false;
    }

    // Fast path: same price and quantity reduction -> maintains FIFO time priority in place
    if (newPrice == existing->price && newQty <= existing->remainingQuantity) {
        return book_.modifyOrder(id, newPrice, newQty, ts);
    }

    // Price change or quantity increase: loses time priority.
    // If the new price now crosses the opposite book, it must be matched aggressively!
    const Side side = existing->side;
    book_.cancelOrder(id);

    return submitLimitOrder(id, side, newPrice, newQty, ts);
}

void MatchingEngine::reset() {
    book_.clear();
}

} // namespace matching_engine
