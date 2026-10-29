#include "matching_engine/ReferenceEngine.hpp"
#include <algorithm>

namespace matching_engine {

bool ReferenceEngine::submitLimitOrder(OrderId id, Side side, Price price, Quantity qty, Timestamp ts) {
    if (qty == 0 || !isPriceValid(price) || ordersMap_.find(id) != ordersMap_.end()) {
        return false;
    }

    Quantity remaining = qty;

    if (side == Side::Buy) {
        while (remaining > 0 && !asks_.empty() && asks_.begin()->first <= price) {
            Price askPrice = asks_.begin()->first;
            OrderQueue& queue = asks_.begin()->second;
            RefOrder& maker = queue.front();
            const Quantity matchQty = std::min(remaining, maker.remainingQuantity);

            maker.remainingQuantity -= matchQty;
            remaining -= matchQty;

            if (tradeListener_) {
                ExecutionReport report;
                report.makerOrderId = maker.id;
                report.takerOrderId = id;
                report.takerSide = Side::Buy;
                report.matchPrice = askPrice;
                report.matchQuantity = matchQty;
                report.makerRemainingQuantity = maker.remainingQuantity;
                report.takerRemainingQuantity = remaining;
                report.timestamp = ts;
                tradeListener_(report);
            }

            if (maker.remainingQuantity == 0) {
                ordersMap_.erase(maker.id);
                queue.pop_front();
                if (queue.empty()) {
                    asks_.erase(asks_.begin());
                }
            }
        }

        if (remaining > 0) {
            OrderQueue& queue = bids_[price];
            queue.push_back(RefOrder(id, price, remaining, Side::Buy, ts));
            ordersMap_[id] = OrderLocation(Side::Buy, price, std::prev(queue.end()));
        }
    } else { // Side::Sell
        while (remaining > 0 && !bids_.empty() && bids_.begin()->first >= price) {
            Price bidPrice = bids_.begin()->first;
            OrderQueue& queue = bids_.begin()->second;
            RefOrder& maker = queue.front();
            const Quantity matchQty = std::min(remaining, maker.remainingQuantity);

            maker.remainingQuantity -= matchQty;
            remaining -= matchQty;

            if (tradeListener_) {
                ExecutionReport report;
                report.makerOrderId = maker.id;
                report.takerOrderId = id;
                report.takerSide = Side::Sell;
                report.matchPrice = bidPrice;
                report.matchQuantity = matchQty;
                report.makerRemainingQuantity = maker.remainingQuantity;
                report.takerRemainingQuantity = remaining;
                report.timestamp = ts;
                tradeListener_(report);
            }

            if (maker.remainingQuantity == 0) {
                ordersMap_.erase(maker.id);
                queue.pop_front();
                if (queue.empty()) {
                    bids_.erase(bids_.begin());
                }
            }
        }

        if (remaining > 0) {
            OrderQueue& queue = asks_[price];
            queue.push_back(RefOrder(id, price, remaining, Side::Sell, ts));
            ordersMap_[id] = OrderLocation(Side::Sell, price, std::prev(queue.end()));
        }
    }

    return true;
}

Quantity ReferenceEngine::submitMarketOrder(OrderId id, Side side, Quantity qty, Timestamp ts) {
    if (qty == 0 || ordersMap_.find(id) != ordersMap_.end()) return 0;
    Quantity remaining = qty;

    if (side == Side::Buy) {
        while (remaining > 0 && !asks_.empty()) {
            Price askPrice = asks_.begin()->first;
            OrderQueue& queue = asks_.begin()->second;
            RefOrder& maker = queue.front();
            const Quantity matchQty = std::min(remaining, maker.remainingQuantity);

            maker.remainingQuantity -= matchQty;
            remaining -= matchQty;

            if (tradeListener_) {
                ExecutionReport report;
                report.makerOrderId = maker.id;
                report.takerOrderId = id;
                report.takerSide = Side::Buy;
                report.matchPrice = askPrice;
                report.matchQuantity = matchQty;
                report.makerRemainingQuantity = maker.remainingQuantity;
                report.takerRemainingQuantity = remaining;
                report.timestamp = ts;
                tradeListener_(report);
            }

            if (maker.remainingQuantity == 0) {
                ordersMap_.erase(maker.id);
                queue.pop_front();
                if (queue.empty()) {
                    asks_.erase(asks_.begin());
                }
            }
        }
    } else { // Side::Sell
        while (remaining > 0 && !bids_.empty()) {
            Price bidPrice = bids_.begin()->first;
            OrderQueue& queue = bids_.begin()->second;
            RefOrder& maker = queue.front();
            const Quantity matchQty = std::min(remaining, maker.remainingQuantity);

            maker.remainingQuantity -= matchQty;
            remaining -= matchQty;

            if (tradeListener_) {
                ExecutionReport report;
                report.makerOrderId = maker.id;
                report.takerOrderId = id;
                report.takerSide = Side::Sell;
                report.matchPrice = bidPrice;
                report.matchQuantity = matchQty;
                report.makerRemainingQuantity = maker.remainingQuantity;
                report.takerRemainingQuantity = remaining;
                report.timestamp = ts;
                tradeListener_(report);
            }

            if (maker.remainingQuantity == 0) {
                ordersMap_.erase(maker.id);
                queue.pop_front();
                if (queue.empty()) {
                    bids_.erase(bids_.begin());
                }
            }
        }
    }

    return qty - remaining;
}

bool ReferenceEngine::cancelOrder(OrderId id) {
    auto it = ordersMap_.find(id);
    if (it == ordersMap_.end()) {
        return false;
    }

    const OrderLocation& loc = it->second;
    if (loc.side == Side::Buy) {
        auto mapIt = bids_.find(loc.price);
        if (mapIt != bids_.end()) {
            mapIt->second.erase(loc.it);
            if (mapIt->second.empty()) {
                bids_.erase(mapIt);
            }
        }
    } else {
        auto mapIt = asks_.find(loc.price);
        if (mapIt != asks_.end()) {
            mapIt->second.erase(loc.it);
            if (mapIt->second.empty()) {
                asks_.erase(mapIt);
            }
        }
    }

    ordersMap_.erase(it);
    return true;
}

bool ReferenceEngine::modifyOrder(OrderId id, Price newPrice, Quantity newQty, Timestamp ts) {
    auto it = ordersMap_.find(id);
    if (it == ordersMap_.end() || newQty == 0 || !isPriceValid(newPrice)) {
        return false;
    }

    // Preserve priority on price identical and quantity decrease
    if (newPrice == it->second.price && newQty <= it->second.it->remainingQuantity) {
        it->second.it->remainingQuantity = newQty;
        return true;
    }

    const Side side = it->second.side;
    cancelOrder(id);
    return submitLimitOrder(id, side, newPrice, newQty, ts);
}

Price ReferenceEngine::bestBidPrice() const {
    return bids_.empty() ? INVALID_PRICE : bids_.begin()->first;
}

Price ReferenceEngine::bestAskPrice() const {
    return asks_.empty() ? INVALID_PRICE : asks_.begin()->first;
}

Quantity ReferenceEngine::totalBidVolume() const {
    Quantity vol = 0;
    for (const auto& pair : bids_) {
        const auto& queue = pair.second;
        for (const auto& ord : queue) {
            vol += ord.remainingQuantity;
        }
    }
    return vol;
}

Quantity ReferenceEngine::totalAskVolume() const {
    Quantity vol = 0;
    for (const auto& pair : asks_) {
        const auto& queue = pair.second;
        for (const auto& ord : queue) {
            vol += ord.remainingQuantity;
        }
    }
    return vol;
}

std::vector<ReferenceEngine::LevelSummary> ReferenceEngine::getBids(size_t depth) const {
    std::vector<LevelSummary> res;
    for (auto it = bids_.begin(); it != bids_.end() && res.size() < depth; ++it) {
        Quantity vol = 0;
        for (const auto& ord : it->second) {
            vol += ord.remainingQuantity;
        }
        res.push_back(LevelSummary(it->first, vol, static_cast<uint32_t>(it->second.size())));
    }
    return res;
}

std::vector<ReferenceEngine::LevelSummary> ReferenceEngine::getAsks(size_t depth) const {
    std::vector<LevelSummary> res;
    for (auto it = asks_.begin(); it != asks_.end() && res.size() < depth; ++it) {
        Quantity vol = 0;
        for (const auto& ord : it->second) {
            vol += ord.remainingQuantity;
        }
        res.push_back(LevelSummary(it->first, vol, static_cast<uint32_t>(it->second.size())));
    }
    return res;
}

void ReferenceEngine::reset() {
    bids_.clear();
    asks_.clear();
    ordersMap_.clear();
}

} // namespace matching_engine
