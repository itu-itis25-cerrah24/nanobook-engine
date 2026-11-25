#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "nanobook/types.hpp"

namespace nanobook {

using NodeIndex = std::uint32_t;
inline constexpr NodeIndex kNullNode = std::numeric_limits<NodeIndex>::max();

struct PriceLevel;

// A resting order. Nodes live in a contiguous pool and are linked into their
// price level's FIFO queue by index (intrusive doubly-linked list), so unlinking
// an order is O(1) and never touches the allocator.
struct OrderNode {
    OrderId     id     = 0;
    Price       price  = 0;
    Quantity    qty    = 0;  // open quantity
    PriceLevel* level  = nullptr;
    NodeIndex   prev   = kNullNode;
    NodeIndex   next   = kNullNode;
    TraderId    trader = kNoTrader;
    Side        side   = Side::Buy;
    STPMode     stp    = STPMode::None;  // kept so a cancel/replace re-enters with the same mode
};
static_assert(sizeof(OrderNode) <= 48, "OrderNode should stay compact (<= 48 bytes)");

// All orders resting at one price, in time priority (head = oldest).
struct PriceLevel {
    Price         price     = 0;
    Quantity      total_qty = 0;
    std::uint32_t count     = 0;
    NodeIndex     head      = kNullNode;
    NodeIndex     tail      = kNullNode;
};

// Free-list slab allocator for order nodes. Released slots are recycled before
// the underlying vector grows, so a steady-state book performs no allocations.
// Nodes are addressed by index: references may be invalidated by allocate().
class OrderPool {
public:
    explicit OrderPool(std::size_t capacity) { nodes_.reserve(capacity); }

    [[nodiscard]] NodeIndex allocate() {
        if (free_head_ != kNullNode) {
            const NodeIndex idx = free_head_;
            free_head_ = nodes_[idx].next;
            return idx;
        }
        nodes_.emplace_back();
        return static_cast<NodeIndex>(nodes_.size() - 1);
    }

    void release(NodeIndex idx) noexcept {
        nodes_[idx].next = free_head_;
        free_head_ = idx;
    }

    [[nodiscard]] OrderNode&       operator[](NodeIndex idx) noexcept { return nodes_[idx]; }
    [[nodiscard]] const OrderNode& operator[](NodeIndex idx) const noexcept { return nodes_[idx]; }

    [[nodiscard]] std::size_t capacity() const noexcept { return nodes_.capacity(); }

private:
    std::vector<OrderNode> nodes_;
    NodeIndex              free_head_ = kNullNode;
};

}  // namespace nanobook
