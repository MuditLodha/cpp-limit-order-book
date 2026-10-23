#include "matching_engine/OrderBook.hpp"
#include <algorithm>
#include <stdexcept>

#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace matching_engine {

namespace {

OrderBookConfig validateConfig(const OrderBookConfig& config) {
    if (config.minPrice == INVALID_PRICE) {
        throw std::invalid_argument("OrderBookConfig::minPrice must be greater than zero");
    }
    if (config.maxPrice < config.minPrice) {
        throw std::invalid_argument("OrderBookConfig::maxPrice must be >= minPrice");
    }
    return config;
}

} // namespace

// Portable count leading zeros for 64-bit word (word != 0)
static inline int countLeadingZeros64(uint64_t word) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_clzll(word);
#elif defined(_MSC_VER)
    unsigned long index;
    _BitScanReverse64(&index, word);
    return static_cast<int>(63 - index);
#else
    int n = 0;
    if ((word & 0xFFFFFFFF00000000ULL) == 0) { n += 32; word <<= 32; }
    if ((word & 0xFFFF000000000000ULL) == 0) { n += 16; word <<= 16; }
    if ((word & 0xFF00000000000000ULL) == 0) { n += 8;  word <<= 8;  }
    if ((word & 0xF000000000000000ULL) == 0) { n += 4;  word <<= 4;  }
    if ((word & 0xC000000000000000ULL) == 0) { n += 2;  word <<= 2;  }
    if ((word & 0x8000000000000000ULL) == 0) { n += 1; }
    return n;
#endif
}

// Portable count trailing zeros for 64-bit word (word != 0)
static inline int countTrailingZeros64(uint64_t word) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_ctzll(word);
#elif defined(_MSC_VER)
    unsigned long index;
    _BitScanForward64(&index, word);
    return static_cast<int>(index);
#else
    int n = 0;
    if ((word & 0x00000000FFFFFFFFULL) == 0) { n += 32; word >>= 32; }
    if ((word & 0x000000000000FFFFULL) == 0) { n += 16; word >>= 16; }
    if ((word & 0x00000000000000FFULL) == 0) { n += 8;  word >>= 8;  }
    if ((word & 0x000000000000000FULL) == 0) { n += 4;  word >>= 4;  }
    if ((word & 0x0000000000000003ULL) == 0) { n += 2;  word >>= 2;  }
    if ((word & 0x0000000000000001ULL) == 0) { n += 1; }
    return n;
#endif
}

OrderBook::OrderBook(const OrderBookConfig& config)
    : config_(validateConfig(config)),
      numLevels_(static_cast<size_t>(
          static_cast<uint64_t>(config_.maxPrice) - config_.minPrice + 1)),
      orderPool_(config_.maxOrders),
      bestBidPrice_(INVALID_PRICE),
      bestAskPrice_(INVALID_PRICE) {
    
    bidLevels_.resize(numLevels_);
    askLevels_.resize(numLevels_);

    const size_t numWords = (numLevels_ + 63) / 64;
    bidBitmap_.assign(numWords, 0ULL);
    askBitmap_.assign(numWords, 0ULL);

    for (size_t i = 0; i < numLevels_; ++i) {
        const Price p = static_cast<Price>(config_.minPrice + i);
        bidLevels_[i].setPrice(p);
        askLevels_[i].setPrice(p);
    }
}

OrderBook::~OrderBook() {
    clear();
}

Order* OrderBook::addOrder(OrderId id, Price price, Quantity qty, Side side, OrderType type, Timestamp ts) {
    if (id == INVALID_ORDER_ID || !isPriceValid(price) || qty == 0) {
        return nullptr;
    }

    if (activeOrders_.find(id) != activeOrders_.end()) {
        return nullptr; // Duplicate order ID
    }

    Order* order = orderPool_.allocate(id, price, qty, side, type, ts);
    if (order == nullptr) {
        return nullptr; // Out of memory in pre-allocated pool
    }

    const size_t idx = priceToIndex(price);
    PriceLevel* level = getLevel(side, price);
    const bool wasEmpty = level->empty();

    level->addOrder(order);
    activeOrders_[id] = order;

    if (wasEmpty) {
        if (side == Side::Buy) {
            setBit(bidBitmap_, idx);
        } else {
            setBit(askBitmap_, idx);
        }
    }

    // Update top of book
    if (side == Side::Buy) {
        if (bestBidPrice_ == INVALID_PRICE || price > bestBidPrice_) {
            bestBidPrice_ = price;
        }
    } else {
        if (bestAskPrice_ == INVALID_PRICE || price < bestAskPrice_) {
            bestAskPrice_ = price;
        }
    }

    return order;
}

bool OrderBook::cancelOrder(OrderId id) {
    auto it = activeOrders_.find(id);
    if (it == activeOrders_.end()) {
        return false;
    }

    Order* order = it->second;
    activeOrders_.erase(it);

    PriceLevel* level = getLevel(order->side, order->price);
    level->removeOrder(order);

    const Side side = order->side;
    const Price price = order->price;
    const size_t idx = priceToIndex(price);

    orderPool_.deallocate(order);

    if (level->empty()) {
        if (side == Side::Buy) {
            clearBit(bidBitmap_, idx);
            if (price == bestBidPrice_) {
                updateBestBid();
            }
        } else {
            clearBit(askBitmap_, idx);
            if (price == bestAskPrice_) {
                updateBestAsk();
            }
        }
    }

    return true;
}

bool OrderBook::modifyOrder(OrderId id, Price newPrice, Quantity newQty, Timestamp ts) {
    auto it = activeOrders_.find(id);
    if (it == activeOrders_.end()) {
        return false;
    }

    Order* order = it->second;
    if (newQty == 0 || !isPriceValid(newPrice)) {
        return false;
    }

    // Case 1: Same price and quantity decrease -> preserves time priority in FIFO queue
    if (newPrice == order->price && newQty <= order->remainingQuantity) {
        const Quantity diff = order->remainingQuantity - newQty;
        order->remainingQuantity = newQty;
        PriceLevel* level = getLevel(order->side, order->price);
        level->reduceVolume(diff);
        return true;
    }

    // Case 2: Price changed or quantity increased -> loses time priority
    const Side side = order->side;
    const OrderType type = order->type;
    cancelOrder(id);
    return addOrder(id, newPrice, newQty, side, type, ts) != nullptr;
}

Order* OrderBook::findOrder(OrderId id) const {
    auto it = activeOrders_.find(id);
    if (it != activeOrders_.end()) {
        return it->second;
    }
    return nullptr;
}

void OrderBook::removeOrder(Order* order) {
    if (order == nullptr) return;

    activeOrders_.erase(order->id);
    PriceLevel* level = getLevel(order->side, order->price);
    level->removeOrder(order);

    const Side side = order->side;
    const Price price = order->price;
    const size_t idx = priceToIndex(price);

    orderPool_.deallocate(order);

    if (level->empty()) {
        if (side == Side::Buy) {
            clearBit(bidBitmap_, idx);
            if (price == bestBidPrice_) {
                updateBestBid();
            }
        } else {
            clearBit(askBitmap_, idx);
            if (price == bestAskPrice_) {
                updateBestAsk();
            }
        }
    }
}

PriceLevel* OrderBook::bestBidLevel() {
    if (bestBidPrice_ == INVALID_PRICE) return nullptr;
    return &bidLevels_[priceToIndex(bestBidPrice_)];
}

PriceLevel* OrderBook::bestAskLevel() {
    if (bestAskPrice_ == INVALID_PRICE) return nullptr;
    return &askLevels_[priceToIndex(bestAskPrice_)];
}

PriceLevel* OrderBook::getLevel(Side side, Price price) {
    assert(isPriceValid(price));
    const size_t idx = priceToIndex(price);
    return (side == Side::Buy) ? &bidLevels_[idx] : &askLevels_[idx];
}

void OrderBook::updateBestBid() {
    if (bestBidPrice_ == INVALID_PRICE) return;

    size_t idx = priceToIndex(bestBidPrice_);
    size_t wordIdx = idx / 64;
    size_t bitOffset = idx % 64;

    uint64_t mask = (bitOffset == 63) ? ~0ULL : ((1ULL << (bitOffset + 1)) - 1);
    uint64_t word = bidBitmap_[wordIdx] & mask;

    while (true) {
        if (word != 0) {
            int leadingZeros = countLeadingZeros64(word);
            size_t highestBit = 63 - leadingZeros;
            size_t foundIdx = wordIdx * 64 + highestBit;
            bestBidPrice_ = static_cast<Price>(config_.minPrice + foundIdx);
            return;
        }
        if (wordIdx == 0) break;
        --wordIdx;
        word = bidBitmap_[wordIdx];
    }
    bestBidPrice_ = INVALID_PRICE;
}

void OrderBook::updateBestAsk() {
    if (bestAskPrice_ == INVALID_PRICE) return;

    size_t idx = priceToIndex(bestAskPrice_);
    size_t wordIdx = idx / 64;
    size_t bitOffset = idx % 64;

    uint64_t mask = ~0ULL << bitOffset;
    uint64_t word = askBitmap_[wordIdx] & mask;
    const size_t numWords = askBitmap_.size();

    while (wordIdx < numWords) {
        if (word != 0) {
            int trailingZeros = countTrailingZeros64(word);
            size_t foundIdx = wordIdx * 64 + trailingZeros;
            if (foundIdx < numLevels_) {
                bestAskPrice_ = static_cast<Price>(config_.minPrice + foundIdx);
                return;
            }
            break;
        }
        ++wordIdx;
        if (wordIdx < numWords) {
            word = askBitmap_[wordIdx];
        }
    }
    bestAskPrice_ = INVALID_PRICE;
}

void OrderBook::clear() {
    for (auto& pair : activeOrders_) {
        orderPool_.deallocate(pair.second);
    }
    activeOrders_.clear();

    for (size_t i = 0; i < bidLevels_.size(); ++i) {
        bidLevels_[i].reset();
    }
    for (size_t i = 0; i < askLevels_.size(); ++i) {
        askLevels_[i].reset();
    }

    std::fill(bidBitmap_.begin(), bidBitmap_.end(), 0ULL);
    std::fill(askBitmap_.begin(), askBitmap_.end(), 0ULL);

    bestBidPrice_ = INVALID_PRICE;
    bestAskPrice_ = INVALID_PRICE;
}

Quantity OrderBook::totalBidVolume() const {
    Quantity vol = 0;
    for (size_t i = 0; i < bidLevels_.size(); ++i) {
        vol += bidLevels_[i].totalVolume();
    }
    return vol;
}

Quantity OrderBook::totalAskVolume() const {
    Quantity vol = 0;
    for (size_t i = 0; i < askLevels_.size(); ++i) {
        vol += askLevels_[i].totalVolume();
    }
    return vol;
}

std::vector<OrderBook::LevelSummary> OrderBook::getBids(size_t depth) const {
    std::vector<LevelSummary> levels;
    if (bestBidPrice_ == INVALID_PRICE) return levels;

    size_t idx = priceToIndex(bestBidPrice_);
    while (levels.size() < depth) {
        if (!bidLevels_[idx].empty()) {
            levels.push_back(LevelSummary(
                bidLevels_[idx].price(),
                bidLevels_[idx].totalVolume(),
                bidLevels_[idx].orderCount()
            ));
        }
        if (idx == 0) break;
        --idx;
    }
    return levels;
}

std::vector<OrderBook::LevelSummary> OrderBook::getAsks(size_t depth) const {
    std::vector<LevelSummary> levels;
    if (bestAskPrice_ == INVALID_PRICE) return levels;

    size_t idx = priceToIndex(bestAskPrice_);
    while (idx < numLevels_ && levels.size() < depth) {
        if (!askLevels_[idx].empty()) {
            levels.push_back(LevelSummary(
                askLevels_[idx].price(),
                askLevels_[idx].totalVolume(),
                askLevels_[idx].orderCount()
            ));
        }
        ++idx;
    }
    return levels;
}

} // namespace matching_engine
