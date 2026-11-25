#include "nanobook/order_book.hpp"

#include <algorithm>

namespace nanobook {

namespace {

constexpr ExecResult rejected(RejectReason reason) noexcept {
    ExecResult r;
    r.status = OrderStatus::Rejected;
    r.reason = reason;
    return r;
}

constexpr bool stp_active(OrderOptions opts) noexcept {
    return opts.trader_id != kNoTrader && opts.stp != STPMode::None;
}

constexpr bool crosses(Side taker, Price limit, Price level_price) noexcept {
    return taker == Side::Buy ? level_price <= limit : level_price >= limit;
}

}  // namespace

OrderBook::OrderBook(std::size_t expected_orders) : pool_(expected_orders) {
    index_.reserve(expected_orders);
    stp_cancelled_.reserve(64);
}

// ---------------------------------------------------------------------------
// Order entry
// ---------------------------------------------------------------------------

ExecResult OrderBook::submit(const OrderRequest& req, std::vector<Trade>& trades) {
    const OrderOptions opts{req.trader_id, req.stp};
    switch (req.type) {
        case OrderType::Limit:  return add_limit(req.id, req.side, req.price, req.qty, trades, opts);
        case OrderType::Market: return add_market(req.id, req.side, req.qty, trades, opts);
        case OrderType::IOC:    return add_ioc(req.id, req.side, req.price, req.qty, trades, opts);
        case OrderType::FOK:    return add_fok(req.id, req.side, req.price, req.qty, trades, opts);
    }
    return rejected(RejectReason::InvalidPrice);
}

ExecResult OrderBook::add_limit(OrderId id, Side side, Price price, Quantity qty,
                                std::vector<Trade>& trades, OrderOptions opts) {
    stp_cancelled_.clear();
    if (qty <= 0) return rejected(RejectReason::InvalidQuantity);
    if (price <= 0) return rejected(RejectReason::InvalidPrice);
    if (index_.contains(id)) return rejected(RejectReason::DuplicateOrderId);

    const MatchOutcome m         = execute(id, side, price, /*priced=*/true, qty, opts, trades);
    const Quantity     remaining = qty - m.filled;

    ExecResult r;
    r.filled      = m.filled;
    r.stp_cancels = stp_count();

    if (remaining == 0) {
        r.status = OrderStatus::Filled;
    } else if (m.self_trade_stop) {
        // CancelNewest: the remainder must not rest opposite the trader's own order.
        r.status        = OrderStatus::Cancelled;
        r.cancelled     = remaining;
        r.cancel_reason = CancelReason::SelfTrade;
    } else {
        rest(id, side, price, remaining, opts);
        r.status  = m.filled > 0 ? OrderStatus::PartiallyFilled : OrderStatus::Resting;
        r.resting = remaining;
    }
    return r;
}

ExecResult OrderBook::add_market(OrderId id, Side side, Quantity qty, std::vector<Trade>& trades,
                                 OrderOptions opts) {
    stp_cancelled_.clear();
    if (qty <= 0) return rejected(RejectReason::InvalidQuantity);
    return execute_immediate(id, side, 0, /*priced=*/false, qty, opts, trades);
}

ExecResult OrderBook::add_ioc(OrderId id, Side side, Price price, Quantity qty,
                              std::vector<Trade>& trades, OrderOptions opts) {
    stp_cancelled_.clear();
    if (qty <= 0) return rejected(RejectReason::InvalidQuantity);
    if (price <= 0) return rejected(RejectReason::InvalidPrice);
    return execute_immediate(id, side, price, /*priced=*/true, qty, opts, trades);
}

ExecResult OrderBook::add_fok(OrderId id, Side side, Price price, Quantity qty,
                              std::vector<Trade>& trades, OrderOptions opts) {
    stp_cancelled_.clear();
    if (qty <= 0) return rejected(RejectReason::InvalidQuantity);
    if (price <= 0) return rejected(RejectReason::InvalidPrice);
    if (index_.contains(id)) return rejected(RejectReason::DuplicateOrderId);

    const bool fillable = side == Side::Buy ? can_fill(asks_, side, price, qty, opts)
                                            : can_fill(bids_, side, price, qty, opts);
    ExecResult r;
    if (!fillable) {
        r.status        = OrderStatus::Cancelled;
        r.cancelled     = qty;
        r.cancel_reason = CancelReason::FOKUnfillable;
        return r;
    }

    // The pre-check guarantees a complete fill.
    const MatchOutcome m = execute(id, side, price, /*priced=*/true, qty, opts, trades);
    r.status      = OrderStatus::Filled;
    r.filled      = m.filled;
    r.stp_cancels = stp_count();
    return r;
}

ExecResult OrderBook::execute_immediate(OrderId id, Side side, Price limit, bool priced,
                                        Quantity qty, OrderOptions opts,
                                        std::vector<Trade>& trades) {
    // Ids must be unique against resting orders so trade reports stay unambiguous.
    if (index_.contains(id)) return rejected(RejectReason::DuplicateOrderId);

    const MatchOutcome m = execute(id, side, limit, priced, qty, opts, trades);

    ExecResult r;
    r.filled      = m.filled;
    r.cancelled   = qty - m.filled;
    r.stp_cancels = stp_count();
    if (r.cancelled == 0) {
        r.status = OrderStatus::Filled;
    } else {
        r.status        = OrderStatus::Cancelled;
        r.cancel_reason = m.self_trade_stop ? CancelReason::SelfTrade : CancelReason::NoLiquidity;
    }
    return r;
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
    stp_cancelled_.clear();
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
        ExecResult r;
        r.status  = OrderStatus::Resting;
        r.resting = new_qty;
        return r;
    }

    // Anything else is cancel/replace: priority is lost and the order may trade.
    const Side         side = node.side;
    const OrderOptions opts{node.trader, node.stp};
    index_.erase(it);
    unlink(idx);
    pool_.release(idx);
    return add_limit(id, side, new_price, new_qty, trades, opts);
}

// ---------------------------------------------------------------------------
// Matching
// ---------------------------------------------------------------------------

OrderBook::MatchOutcome OrderBook::execute(OrderId taker, Side side, Price limit, bool priced,
                                           Quantity qty, OrderOptions opts,
                                           std::vector<Trade>& trades) {
    return side == Side::Buy ? sweep(asks_, taker, side, limit, priced, qty, opts, trades)
                             : sweep(bids_, taker, side, limit, priced, qty, opts, trades);
}

template <class Book>
OrderBook::MatchOutcome OrderBook::sweep(Book& book, OrderId taker, Side side, Price limit,
                                         bool priced, Quantity qty, OrderOptions opts,
                                         std::vector<Trade>& trades) {
    const bool stp       = stp_active(opts);
    Quantity   remaining = qty;
    bool       stopped   = false;

    while (remaining > 0 && !stopped && !book.empty()) {
        const auto  level_it = book.begin();
        PriceLevel& level    = level_it->second;

        if (priced && !crosses(side, limit, level.price)) break;

        // Walk the FIFO queue from the oldest order.
        while (remaining > 0 && level.head != kNullNode) {
            const NodeIndex idx   = level.head;
            OrderNode&      maker = pool_[idx];

            if (stp && maker.trader == opts.trader_id) [[unlikely]] {
                if (opts.stp == STPMode::CancelNewest) {
                    stopped = true;
                    break;
                }
                // CancelOldest: drop the trader's resting order and keep matching.
                stp_cancelled_.push_back(maker.id);
                level.total_qty -= maker.qty;
                pop_head(level, idx);
                continue;
            }

            const Quantity fill = std::min(remaining, maker.qty);
            trades.push_back(Trade{next_seq_++, taker, maker.id, side, level.price, fill});

            remaining       -= fill;
            maker.qty       -= fill;
            level.total_qty -= fill;

            if (maker.qty == 0) pop_head(level, idx);
        }

        if (level.head == kNullNode) book.erase(level_it);
    }

    return MatchOutcome{qty - remaining, stopped};
}

template <class Book>
bool OrderBook::can_fill(const Book& book, Side side, Price limit, Quantity qty,
                         OrderOptions opts) const {
    const bool stp       = stp_active(opts);
    Quantity   available = 0;

    for (const auto& [price, level] : book) {
        if (!crosses(side, limit, price)) return false;

        if (!stp) {
            available += level.total_qty;  // O(1) per level via the running total
        } else {
            // STP changes what is reachable, so walk the queue in priority order.
            for (NodeIndex idx = level.head; idx != kNullNode; idx = pool_[idx].next) {
                const OrderNode& maker = pool_[idx];
                if (maker.trader == opts.trader_id) {
                    // CancelNewest stops at the first own order; CancelOldest skips it.
                    if (opts.stp == STPMode::CancelNewest) return available >= qty;
                    continue;
                }
                available += maker.qty;
                if (available >= qty) return true;
            }
        }
        if (available >= qty) return true;
    }
    return false;
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

void OrderBook::rest(OrderId id, Side side, Price price, Quantity qty, OrderOptions opts) {
    PriceLevel& level = side == Side::Buy ? level_for(bids_, price) : level_for(asks_, price);

    const NodeIndex idx = pool_.allocate();
    pool_[idx] = OrderNode{id, price, qty, &level, level.tail, kNullNode, opts.trader_id, side, opts.stp};

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

void OrderBook::pop_head(PriceLevel& level, NodeIndex idx) {
    const OrderNode& node = pool_[idx];
    level.head = node.next;
    if (level.head != kNullNode) {
        pool_[level.head].prev = kNullNode;
    } else {
        level.tail = kNullNode;
    }
    --level.count;
    index_.erase(node.id);
    pool_.release(idx);  // overwrites node.next: must come last
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
    return OrderView{node.id, node.side, node.price, node.qty, node.trader};
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
