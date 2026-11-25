#include "nanobook/order_book.hpp"

#include <algorithm>

namespace nanobook {

namespace {

constexpr ExecResult rejected(RejectReason reason) noexcept {
    return ExecResult{OrderStatus::Rejected, reason, 0, 0, 0};
}

}  // namespace

OrderBook::OrderBook(std::size_t expected_orders) : pool_(expected_orders) {
    index_.reserve(expected_orders);
}

// ---------------------------------------------------------------------------
// Order entry
// ---------------------------------------------------------------------------

ExecResult OrderBook::submit(const OrderRequest& req, std::vector<Trade>& trades) {
    switch (req.type) {
        case OrderType::Limit:  return add_limit(req.id, req.side, req.price, req.qty, trades);
        case OrderType::Market: return add_market(req.id, req.side, req.qty, trades);
        case OrderType::IOC:    return add_ioc(req.id, req.side, req.price, req.qty, trades);
    }
    return rejected(RejectReason::InvalidPrice);
}

ExecResult OrderBook::add_limit(OrderId id, Side side, Price price, Quantity qty,
                                std::vector<Trade>& trades) {
    if (qty <= 0) return rejected(RejectReason::InvalidQuantity);
    if (price <= 0) return rejected(RejectReason::InvalidPrice);
    if (index_.contains(id)) return rejected(RejectReason::DuplicateOrderId);

    const Quantity filled    = execute(id, side, price, /*priced=*/true, qty, trades);
    const Quantity remaining = qty - filled;

    if (remaining == 0) return ExecResult{OrderStatus::Filled, RejectReason::None, filled, 0, 0};

    rest(id, side, price, remaining);
    const OrderStatus status = filled > 0 ? OrderStatus::PartiallyFilled : OrderStatus::Resting;
    return ExecResult{status, RejectReason::None, filled, remaining, 0};
}

ExecResult OrderBook::add_market(OrderId id, Side side, Quantity qty, std::vector<Trade>& trades) {
    if (qty <= 0) return rejected(RejectReason::InvalidQuantity);
    return execute_immediate(id, side, 0, /*priced=*/false, qty, trades);
}

ExecResult OrderBook::add_ioc(OrderId id, Side side, Price price, Quantity qty,
                              std::vector<Trade>& trades) {
    if (qty <= 0) return rejected(RejectReason::InvalidQuantity);
    if (price <= 0) return rejected(RejectReason::InvalidPrice);
    return execute_immediate(id, side, price, /*priced=*/true, qty, trades);
}

ExecResult OrderBook::execute_immediate(OrderId id, Side side, Price limit, bool priced,
                                        Quantity qty, std::vector<Trade>& trades) {
    // Ids must be unique against resting orders so trade reports stay unambiguous.
    if (index_.contains(id)) return rejected(RejectReason::DuplicateOrderId);

    const Quantity filled    = execute(id, side, limit, priced, qty, trades);
    const Quantity cancelled = qty - filled;
    const OrderStatus status = cancelled == 0 ? OrderStatus::Filled : OrderStatus::Cancelled;
    return ExecResult{status, RejectReason::None, filled, 0, cancelled};
}

bool OrderBook::cancel(OrderId id) {
    const auto it = index_.find(id);
    if (it == index_.end()) return false;

    const NodeIndex idx = it->second;
    index_.erase(it);
    unlink(idx);
    pool_.release(idx);
    return true;
}

ExecResult OrderBook::modify(OrderId id, Price new_price, Quantity new_qty,
                             std::vector<Trade>& trades) {
    if (new_qty <= 0) return rejected(RejectReason::InvalidQuantity);
    if (new_price <= 0) return rejected(RejectReason::InvalidPrice);

    const auto it = index_.find(id);
    if (it == index_.end()) return rejected(RejectReason::UnknownOrder);

    const NodeIndex idx  = it->second;
    OrderNode&      node = pool_[idx];

    // Size-down at the same price keeps the order's place in the queue.
    if (new_price == node.price && new_qty <= node.qty) {
        node.level->total_qty -= node.qty - new_qty;
        node.qty = new_qty;
        return ExecResult{OrderStatus::Resting, RejectReason::None, 0, new_qty, 0};
    }

    // Anything else is cancel/replace: priority is lost and the order may trade.
    const Side side = node.side;
    index_.erase(it);
    unlink(idx);
    pool_.release(idx);
    return add_limit(id, side, new_price, new_qty, trades);
}

// ---------------------------------------------------------------------------
// Matching
// ---------------------------------------------------------------------------

Quantity OrderBook::execute(OrderId taker, Side side, Price limit, bool priced, Quantity qty,
                            std::vector<Trade>& trades) {
    return side == Side::Buy ? sweep(asks_, taker, side, limit, priced, qty, trades)
                             : sweep(bids_, taker, side, limit, priced, qty, trades);
}

template <class Book>
Quantity OrderBook::sweep(Book& book, OrderId taker, Side side, Price limit, bool priced,
                          Quantity qty, std::vector<Trade>& trades) {
    Quantity remaining = qty;

    while (remaining > 0 && !book.empty()) {
        const auto  level_it = book.begin();
        PriceLevel& level    = level_it->second;

        if (priced) {
            const bool crosses = side == Side::Buy ? level.price <= limit : level.price >= limit;
            if (!crosses) break;
        }

        // Walk the FIFO queue from the oldest order.
        while (remaining > 0 && level.head != kNullNode) {
            const NodeIndex idx   = level.head;
            OrderNode&      maker = pool_[idx];
            const Quantity  fill  = std::min(remaining, maker.qty);

            trades.push_back(Trade{next_seq_++, taker, maker.id, side, level.price, fill});

            remaining       -= fill;
            maker.qty       -= fill;
            level.total_qty -= fill;

            if (maker.qty == 0) {
                level.head = maker.next;
                if (level.head != kNullNode) {
                    pool_[level.head].prev = kNullNode;
                } else {
                    level.tail = kNullNode;
                }
                --level.count;
                index_.erase(maker.id);
                pool_.release(idx);
            }
        }

        if (level.head == kNullNode) book.erase(level_it);
    }

    return qty - remaining;
}

// ---------------------------------------------------------------------------
// Book maintenance
// ---------------------------------------------------------------------------

template <class Book>
PriceLevel& OrderBook::level_for(Book& book, Price price) {
    auto [it, inserted] = book.try_emplace(price);
    if (inserted) it->second.price = price;
    return it->second;
}

void OrderBook::rest(OrderId id, Side side, Price price, Quantity qty) {
    PriceLevel& level = side == Side::Buy ? level_for(bids_, price) : level_for(asks_, price);

    const NodeIndex idx = pool_.allocate();
    pool_[idx] = OrderNode{id, price, qty, &level, level.tail, kNullNode, side};

    if (level.tail != kNullNode) {
        pool_[level.tail].next = idx;
    } else {
        level.head = idx;
    }
    level.tail = idx;
    ++level.count;
    level.total_qty += qty;

    index_.emplace(id, idx);
}

void OrderBook::unlink(NodeIndex idx) {
    const OrderNode& node  = pool_[idx];
    PriceLevel&      level = *node.level;

    if (node.prev != kNullNode) {
        pool_[node.prev].next = node.next;
    } else {
        level.head = node.next;
    }
    if (node.next != kNullNode) {
        pool_[node.next].prev = node.prev;
    } else {
        level.tail = node.prev;
    }

    level.total_qty -= node.qty;
    --level.count;

    if (level.count == 0) {
        if (node.side == Side::Buy) {
            bids_.erase(node.price);
        } else {
            asks_.erase(node.price);
        }
    }
}

// ---------------------------------------------------------------------------
// Market data
// ---------------------------------------------------------------------------

std::optional<Price> OrderBook::best_bid() const noexcept {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const noexcept {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

std::optional<Price> OrderBook::spread() const noexcept {
    if (bids_.empty() || asks_.empty()) return std::nullopt;
    return asks_.begin()->first - bids_.begin()->first;
}

template <class Book>
void OrderBook::collect(const Book& book, std::size_t depth, std::vector<LevelView>& out) {
    out.clear();
    for (auto it = book.begin(); it != book.end() && out.size() < depth; ++it) {
        out.push_back(LevelView{it->first, it->second.total_qty, it->second.count});
    }
}

Level2Snapshot OrderBook::snapshot(std::size_t depth) const {
    Level2Snapshot snap;
    snap.bids.reserve(std::min(depth, bids_.size()));
    snap.asks.reserve(std::min(depth, asks_.size()));
    snapshot(depth, snap);
    return snap;
}

void OrderBook::snapshot(std::size_t depth, Level2Snapshot& out) const {
    collect(bids_, depth, out.bids);
    collect(asks_, depth, out.asks);
}

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------

bool OrderBook::contains(OrderId id) const { return index_.contains(id); }

std::optional<OrderView> OrderBook::find(OrderId id) const {
    const auto it = index_.find(id);
    if (it == index_.end()) return std::nullopt;
    const OrderNode& node = pool_[it->second];
    return OrderView{node.id, node.side, node.price, node.qty};
}

std::vector<OrderId> OrderBook::queue_at(Side side, Price price) const {
    std::vector<OrderId> ids;
    const PriceLevel*    level = nullptr;

    if (side == Side::Buy) {
        if (const auto it = bids_.find(price); it != bids_.end()) level = &it->second;
    } else {
        if (const auto it = asks_.find(price); it != asks_.end()) level = &it->second;
    }
    if (level == nullptr) return ids;

    ids.reserve(level->count);
    for (NodeIndex idx = level->head; idx != kNullNode; idx = pool_[idx].next) {
        ids.push_back(pool_[idx].id);
    }
    return ids;
}

}  // namespace nanobook
