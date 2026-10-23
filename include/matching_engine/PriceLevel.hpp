#pragma once

#include "Types.hpp"
#include "Order.hpp"
#include "IntrusiveList.hpp"
#include <cstdint>
#include <cassert>

namespace matching_engine {

/**
 * @brief Represents a single price level containing a FIFO queue of orders.
 *
 * Tracks aggregated quantity and order count to allow O(1) depth queries
 * and O(1) matching against the oldest orders at this price point.
 */
class PriceLevel {
public:
    PriceLevel()
        : price_(INVALID_PRICE),
          totalVolume_(0),
          orderCount_(0) {}

    explicit PriceLevel(Price price)
        : price_(price),
          totalVolume_(0),
          orderCount_(0) {}

    Price price() const { return price_; }
    Quantity totalVolume() const { return totalVolume_; }
    uint32_t orderCount() const { return orderCount_; }
    bool empty() const { return orderList_.empty(); }

    Order* frontOrder() { return orderList_.front(); }
    const Order* frontOrder() const { return orderList_.front(); }

    IntrusiveList<Order>& orders() { return orderList_; }
    const IntrusiveList<Order>& orders() const { return orderList_; }

    /**
     * @brief Add an order to the tail of this price level (FIFO priority).
     */
    void addOrder(Order* order) {
        assert(order != nullptr);
        orderList_.push_back(order);
        totalVolume_ += order->remainingQuantity;
        ++orderCount_;
    }

    /**
     * @brief Remove an order from this price level (cancel, fill, or modify).
     */
    void removeOrder(Order* order) {
        assert(order != nullptr);
        assert(totalVolume_ >= order->remainingQuantity);
        assert(orderCount_ > 0);

        orderList_.remove(order);
        totalVolume_ -= order->remainingQuantity;
        --orderCount_;
    }

    /**
     * @brief Reduce volume when an order at this level is partially filled.
     */
    void reduceVolume(Quantity fillQty) {
        assert(totalVolume_ >= fillQty);
        totalVolume_ -= fillQty;
    }

    void setPrice(Price p) {
        price_ = p;
    }

    void reset() {
        orderList_.clear();
        totalVolume_ = 0;
        orderCount_ = 0;
    }

private:
    Price price_;
    Quantity totalVolume_;
    uint32_t orderCount_;
    IntrusiveList<Order> orderList_;
};

} // namespace matching_engine
