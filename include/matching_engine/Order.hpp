#pragma once

#include "Types.hpp"
#include "IntrusiveList.hpp"
#include <cstdint>
#include <cassert>

namespace matching_engine {

/**
 * @brief Represents an individual order in the order book.
 *
 * Inherits from IntrusiveNode<Order> to allow direct insertion into
 * intrusive doubly-linked queues at each price level without dynamic
 * node allocations.
 */
struct alignas(64) Order : public IntrusiveNode<Order> {
    OrderId   id;
    Price     price;
    Quantity  initialQuantity;
    Quantity  remainingQuantity;
    Side      side;
    OrderType type;
    OrderStatus status;
    Timestamp timestamp;

    // Default constructor
    Order()
        : id(INVALID_ORDER_ID),
          price(INVALID_PRICE),
          initialQuantity(0),
          remainingQuantity(0),
          side(Side::Buy),
          type(OrderType::Limit),
          status(OrderStatus::New),
          timestamp(0) {}

    // Parameterized constructor
    Order(OrderId orderId, Price p, Quantity qty, Side s, OrderType t, Timestamp ts = 0)
        : id(orderId),
          price(p),
          initialQuantity(qty),
          remainingQuantity(qty),
          side(s),
          type(t),
          status(OrderStatus::New),
          timestamp(ts) {}

    bool isFilled() const {
        return remainingQuantity == 0;
    }

    Quantity filledQuantity() const {
        return initialQuantity - remainingQuantity;
    }

    /**
     * @brief Execute a fill against this order.
     * @param qty Number of shares/contracts to fill.
     * @return Actual quantity filled.
     */
    Quantity fill(Quantity qty) {
        assert(qty <= remainingQuantity);
        remainingQuantity -= qty;
        if (remainingQuantity == 0) {
            status = OrderStatus::Filled;
        } else {
            status = OrderStatus::PartiallyFilled;
        }
        return qty;
    }
};

} // namespace matching_engine
