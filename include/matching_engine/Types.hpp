#pragma once

#include <cstdint>
#include <limits>
#include <array>

namespace matching_engine {

// Strongly typed aliases for core domain concepts
using OrderId   = uint64_t;
using Price     = uint32_t;    // Integer ticks (e.g., 10000 = $100.00 with tick size 0.01)
using Quantity  = uint32_t;    // Lot count / shares
using Timestamp = uint64_t;    // Nanoseconds or CPU cycle count

// Sentinel values
static const Price    INVALID_PRICE    = 0;
static const OrderId  INVALID_ORDER_ID = 0;
static const Quantity ZERO_QUANTITY    = 0;

// Order side: Buy (Bid) or Sell (Ask)
enum class Side : uint8_t {
    Buy  = 0,
    Sell = 1
};

// Returns a human-readable string for a Side value
inline const char* sideToString(Side side) {
    switch (side) {
        case Side::Buy:  return "BUY";
        case Side::Sell: return "SELL";
        default:         return "UNKNOWN";
    }
}

// Order type
enum class OrderType : uint8_t {
    Limit  = 0,
    Market = 1
};

// Returns a human-readable string for an OrderType value
inline const char* orderTypeToString(OrderType type) {
    switch (type) {
        case OrderType::Limit:  return "LIMIT";
        case OrderType::Market: return "MARKET";
        default:                return "UNKNOWN";
    }
}

// Order lifecycle / execution status
enum class OrderStatus : uint8_t {
    New             = 0,
    PartiallyFilled = 1,
    Filled          = 2,
    Canceled        = 3,
    Rejected        = 4
};

// Returns a human-readable string for an OrderStatus value
inline const char* orderStatusToString(OrderStatus status) {
    switch (status) {
        case OrderStatus::New:             return "NEW";
        case OrderStatus::PartiallyFilled: return "PARTIALLY_FILLED";
        case OrderStatus::Filled:          return "FILLED";
        case OrderStatus::Canceled:        return "CANCELED";
        case OrderStatus::Rejected:        return "REJECTED";
        default:                           return "UNKNOWN";
    }
}

// Reason for rejection if an order cannot be processed
enum class RejectReason : uint8_t {
    None             = 0,
    DuplicateOrderId = 1,
    OrderNotFound    = 2,
    InvalidPrice     = 3,
    InvalidQuantity  = 4,
    NoLiquidity      = 5,
    PriceOutOfRange  = 6,
    CapacityExceeded = 7
};

// Trade execution event generated on order matching
struct ExecutionReport {
    OrderId  makerOrderId;
    OrderId  takerOrderId;
    Side     takerSide;
    Price    matchPrice;
    Quantity matchQuantity;
    Quantity makerRemainingQuantity;
    Quantity takerRemainingQuantity;
    Timestamp timestamp;

    ExecutionReport()
        : makerOrderId(INVALID_ORDER_ID),
          takerOrderId(INVALID_ORDER_ID),
          takerSide(Side::Buy),
          matchPrice(0),
          matchQuantity(0),
          makerRemainingQuantity(0),
          takerRemainingQuantity(0),
          timestamp(0) {}
};

} // namespace matching_engine
