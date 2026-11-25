#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "nanobook/market_data.hpp"
#include "nanobook/order_pool.hpp"
#include "nanobook/types.hpp"

namespace nanobook {

// Read-only view of a resting order.
struct OrderView {
    OrderId  id    = 0;
    Side     side  = Side::Buy;
    Price    price = 0;
    Quantity qty   = 0;
};

// Single-instrument limit order book with price-time (FIFO) priority matching.
//
//  * Price levels: one ordered map per side, best price at begin() (O(1) access).
//  * Orders: pooled nodes in an intrusive FIFO list per level.
//  * Index: OrderId -> node, giving O(1) lookup, cancel and in-place modify
//    (removing an emptied level is O(log L) in the number of levels).
//
// Trades produced by a request are appended to the caller-supplied vector, so a
// hot loop can reuse one buffer and avoid per-order allocations.
// The class is not thread-safe; run one book per matching thread.
class OrderBook {
public:
    explicit OrderBook(std::size_t expected_orders = 1u << 16);

    // ---- Order entry -------------------------------------------------------
    ExecResult submit(const OrderRequest& req, std::vector<Trade>& trades);

    ExecResult add_limit(OrderId id, Side side, Price price, Quantity qty, std::vector<Trade>& trades);
    ExecResult add_market(OrderId id, Side side, Quantity qty, std::vector<Trade>& trades);
    ExecResult add_ioc(OrderId id, Side side, Price price, Quantity qty, std::vector<Trade>& trades);

    // Removes a resting order. Returns false if the id is not on the book.
    bool cancel(OrderId id);

    // Changes price and/or open quantity of a resting order.
    //  * Same price, quantity reduced (or unchanged): updated in place, keeps time priority.
    //  * Price change or quantity increase: loses time priority and is re-entered
    //    as a new limit order, which may trade immediately if it now crosses.
    ExecResult modify(OrderId id, Price new_price, Quantity new_qty, std::vector<Trade>& trades);

    // ---- Market data -------------------------------------------------------
    [[nodiscard]] std::optional<Price> best_bid() const noexcept;
    [[nodiscard]] std::optional<Price> best_ask() const noexcept;
    [[nodiscard]] std::optional<Price> spread() const noexcept;

    [[nodiscard]] Level2Snapshot snapshot(std::size_t depth) const;
    void snapshot(std::size_t depth, Level2Snapshot& out) const;  // reuses out's buffers

    // ---- Introspection -----------------------------------------------------
    [[nodiscard]] bool                     contains(OrderId id) const;
    [[nodiscard]] std::optional<OrderView> find(OrderId id) const;
    [[nodiscard]] std::vector<OrderId>     queue_at(Side side, Price price) const;  // FIFO order

    [[nodiscard]] std::size_t order_count() const noexcept { return index_.size(); }
    [[nodiscard]] std::size_t bid_levels() const noexcept { return bids_.size(); }
    [[nodiscard]] std::size_t ask_levels() const noexcept { return asks_.size(); }
    [[nodiscard]] bool        empty() const noexcept { return index_.empty(); }

private:
    using BidMap = std::map<Price, PriceLevel, std::greater<Price>>;
    using AskMap = std::map<Price, PriceLevel, std::less<Price>>;

    // Matches an incoming order against the opposite side. Returns filled quantity.
    Quantity execute(OrderId taker, Side side, Price limit, bool priced, Quantity qty,
                     std::vector<Trade>& trades);

    template <class Book>
    Quantity sweep(Book& book, OrderId taker, Side side, Price limit, bool priced, Quantity qty,
                   std::vector<Trade>& trades);

    template <class Book>
    static PriceLevel& level_for(Book& book, Price price);

    template <class Book>
    static void collect(const Book& book, std::size_t depth, std::vector<LevelView>& out);

    ExecResult execute_immediate(OrderId id, Side side, Price limit, bool priced, Quantity qty,
                                 std::vector<Trade>& trades);

    void rest(OrderId id, Side side, Price price, Quantity qty);
    void unlink(NodeIndex idx);

    BidMap                                bids_;
    AskMap                                asks_;
    std::unordered_map<OrderId, NodeIndex> index_;
    OrderPool                             pool_;
    SeqNum                                next_seq_ = 1;
};

}  // namespace nanobook
