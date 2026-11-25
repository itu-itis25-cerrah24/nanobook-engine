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
    OrderId  id        = 0;
    Side     side      = Side::Buy;
    Price    price     = 0;
    Quantity qty       = 0;
    TraderId trader_id = kNoTrader;
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
// The class is not thread-safe; run one book per matching thread (see SPSCQueue
// for feeding it from another thread).
class OrderBook {
public:
    explicit OrderBook(std::size_t expected_orders = 1u << 16);

    // ---- Order entry -------------------------------------------------------
    ExecResult submit(const OrderRequest& req, std::vector<Trade>& trades);

    ExecResult add_limit(OrderId id, Side side, Price price, Quantity qty, std::vector<Trade>& trades,
                         OrderOptions opts = {});
    ExecResult add_market(OrderId id, Side side, Quantity qty, std::vector<Trade>& trades,
                          OrderOptions opts = {});
    ExecResult add_ioc(OrderId id, Side side, Price price, Quantity qty, std::vector<Trade>& trades,
                       OrderOptions opts = {});
    // Fill-or-Kill: liquidity up to `price` is checked first without touching the
    // book. The order either fills completely or is cancelled with no trades and
    // no state change.
    ExecResult add_fok(OrderId id, Side side, Price price, Quantity qty, std::vector<Trade>& trades,
                       OrderOptions opts = {});

    // Removes a resting order. Returns false if the id is not on the book.
    bool cancel(OrderId id);

    // Changes price and/or open quantity of a resting order.
    //  * Same price, quantity reduced (or unchanged): updated in place, keeps time priority.
    //  * Price change or quantity increase: loses time priority and is re-entered
    //    as a new limit order (same trader / STP mode), which may trade immediately.
    ExecResult modify(OrderId id, Price new_price, Quantity new_qty, std::vector<Trade>& trades);

    // Resting orders cancelled by self-trade prevention (CancelOldest) during the
    // most recent order entry or modify request.
    [[nodiscard]] const std::vector<OrderId>& stp_cancelled() const noexcept { return stp_cancelled_; }

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

    struct MatchOutcome {
        Quantity filled          = 0;
        bool     self_trade_stop = false;  // STP CancelNewest halted matching
    };

    // Matches an incoming order against the opposite side.
    MatchOutcome execute(OrderId taker, Side side, Price limit, bool priced, Quantity qty,
                         OrderOptions opts, std::vector<Trade>& trades);

    template <class Book>
    MatchOutcome sweep(Book& book, OrderId taker, Side side, Price limit, bool priced, Quantity qty,
                       OrderOptions opts, std::vector<Trade>& trades);

    // Read-only liquidity check used by FOK (honours STP semantics).
    template <class Book>
    bool can_fill(const Book& book, Side side, Price limit, Quantity qty, OrderOptions opts) const;

    template <class Book>
    static PriceLevel& level_for(Book& book, Price price);

    template <class Book>
    static void collect(const Book& book, std::size_t depth, std::vector<LevelView>& out);

    ExecResult execute_immediate(OrderId id, Side side, Price limit, bool priced, Quantity qty,
                                 OrderOptions opts, std::vector<Trade>& trades);

    void rest(OrderId id, Side side, Price price, Quantity qty, OrderOptions opts);
    void pop_head(PriceLevel& level, NodeIndex idx);  // remove the level's head order
    void unlink(NodeIndex idx);

    [[nodiscard]] std::uint32_t stp_count() const noexcept {
        return static_cast<std::uint32_t>(stp_cancelled_.size());
    }

    BidMap                                 bids_;
    AskMap                                 asks_;
    std::unordered_map<OrderId, NodeIndex> index_;
    OrderPool                              pool_;
    std::vector<OrderId>                   stp_cancelled_;
    SeqNum                                 next_seq_ = 1;
};

}  // namespace nanobook
