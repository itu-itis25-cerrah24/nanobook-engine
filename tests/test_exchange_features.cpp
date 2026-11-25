// Fill-or-Kill and Self-Trade Prevention tests.

#include <vector>

#include "nanobook/order_book.hpp"
#include "test_framework.hpp"

using namespace nanobook;

namespace {

using Trades = std::vector<Trade>;

constexpr OrderOptions kNewest(TraderId t) { return OrderOptions{t, STPMode::CancelNewest}; }
constexpr OrderOptions kOldest(TraderId t) { return OrderOptions{t, STPMode::CancelOldest}; }
constexpr OrderOptions kTrader(TraderId t) { return OrderOptions{t, STPMode::None}; }

// Full depth of both sides, for "nothing changed" assertions.
struct BookState {
    std::vector<LevelView> bids;
    std::vector<LevelView> asks;
    bool operator==(const BookState&) const = default;
};

BookState state(const OrderBook& book) {
    auto snap = book.snapshot(1'000);
    return BookState{std::move(snap.bids), std::move(snap.asks)};
}

}  // namespace

// ---------------------------------------------------------------------------
// Fill-or-Kill
// ---------------------------------------------------------------------------

NB_TEST(fok_full_fill_at_single_level) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Sell, 100, 50, trades);

    const auto r = book.add_fok(2, Side::Buy, 100, 30, trades);
    NB_CHECK_EQ(r.status, OrderStatus::Filled);
    NB_CHECK_EQ(r.filled, 30);
    NB_CHECK_EQ(r.cancelled, 0);
    NB_CHECK_EQ(trades.size(), 1u);
    NB_CHECK_EQ(trades[0], (Trade{1, 2, 1, Side::Buy, 100, 30}));
    NB_CHECK_EQ(book.find(1)->qty, 20);
    NB_CHECK(!book.contains(2));
}

NB_TEST(fok_multi_level_fill) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Sell, 101, 10, trades);
    book.add_limit(2, Side::Sell, 102, 20, trades);
    book.add_limit(3, Side::Sell, 103, 30, trades);

    const auto r = book.add_fok(9, Side::Buy, 103, 60, trades);
    NB_CHECK_EQ(r.status, OrderStatus::Filled);
    NB_CHECK_EQ(r.filled, 60);
    NB_CHECK_EQ(trades.size(), 3u);
    NB_CHECK_EQ(trades[0].price, 101);
    NB_CHECK_EQ(trades[1].price, 102);
    NB_CHECK_EQ(trades[2].price, 103);
    NB_CHECK(book.empty());
}

NB_TEST(fok_sell_sweeps_bids_from_best) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Buy, 99, 5, trades);
    book.add_limit(2, Side::Buy, 100, 5, trades);
    book.add_limit(3, Side::Buy, 98, 5, trades);

    const auto r = book.add_fok(9, Side::Sell, 99, 10, trades);
    NB_CHECK_EQ(r.status, OrderStatus::Filled);
    NB_CHECK_EQ(trades.size(), 2u);
    NB_CHECK_EQ(trades[0].price, 100);
    NB_CHECK_EQ(trades[1].price, 99);
    NB_CHECK_EQ(book.best_bid(), std::optional<Price>{98});
}

NB_TEST(fok_exact_liquidity_boundary) {
    Trades trades;
    {
        OrderBook book;
        book.add_limit(1, Side::Sell, 101, 10, trades);
        book.add_limit(2, Side::Sell, 102, 20, trades);
        NB_CHECK_EQ(book.add_fok(9, Side::Buy, 102, 30, trades).status, OrderStatus::Filled);
        NB_CHECK(book.empty());
    }
    {
        OrderBook book;
        book.add_limit(1, Side::Sell, 101, 10, trades);
        book.add_limit(2, Side::Sell, 102, 20, trades);
        const auto r = book.add_fok(9, Side::Buy, 102, 31, trades);  // one short
        NB_CHECK_EQ(r.status, OrderStatus::Cancelled);
        NB_CHECK_EQ(book.order_count(), 2u);
    }
}

NB_TEST(fok_partial_liquidity_is_killed_without_mutation) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Sell, 101, 10, trades);
    book.add_limit(2, Side::Sell, 102, 20, trades);
    book.add_limit(3, Side::Sell, 102, 5, trades);
    book.add_limit(4, Side::Sell, 105, 100, trades);  // beyond the FOK limit
    book.add_limit(5, Side::Buy, 99, 40, trades);
    const BookState before = state(book);

    const auto r = book.add_fok(9, Side::Buy, 102, 36, trades);
    NB_CHECK_EQ(r.status, OrderStatus::Cancelled);
    NB_CHECK_EQ(r.cancel_reason, CancelReason::FOKUnfillable);
    NB_CHECK_EQ(r.filled, 0);
    NB_CHECK_EQ(r.cancelled, 36);
    NB_CHECK_EQ(r.resting, 0);
    NB_CHECK(trades.empty());
    NB_CHECK(state(book) == before);
    NB_CHECK_EQ(book.queue_at(Side::Sell, 102), (std::vector<OrderId>{2, 3}));
    NB_CHECK(!book.contains(9));

    // No trade sequence number was consumed by the killed order.
    book.add_market(10, Side::Buy, 1, trades);
    NB_CHECK_EQ(trades.at(0).seq, 1u);
}

NB_TEST(fok_respects_limit_price) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Sell, 105, 1'000, trades);
    const auto r = book.add_fok(2, Side::Buy, 104, 1, trades);
    NB_CHECK_EQ(r.status, OrderStatus::Cancelled);
    NB_CHECK_EQ(r.cancel_reason, CancelReason::FOKUnfillable);
    NB_CHECK(trades.empty());
    NB_CHECK_EQ(book.find(1)->qty, 1'000);
}

NB_TEST(fok_on_empty_book_is_killed) {
    OrderBook  book;
    Trades     trades;
    const auto r = book.add_fok(1, Side::Sell, 100, 10, trades);
    NB_CHECK_EQ(r.status, OrderStatus::Cancelled);
    NB_CHECK_EQ(r.cancel_reason, CancelReason::FOKUnfillable);
    NB_CHECK(book.empty());
}

NB_TEST(fok_rejects_invalid_input) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Sell, 100, 10, trades);
    NB_CHECK_EQ(book.add_fok(2, Side::Buy, 100, 0, trades).reason, RejectReason::InvalidQuantity);
    NB_CHECK_EQ(book.add_fok(3, Side::Buy, 100, -4, trades).reason, RejectReason::InvalidQuantity);
    NB_CHECK_EQ(book.add_fok(4, Side::Buy, 0, 5, trades).reason, RejectReason::InvalidPrice);
    NB_CHECK_EQ(book.add_fok(1, Side::Buy, 100, 5, trades).reason, RejectReason::DuplicateOrderId);
    NB_CHECK(trades.empty());
    NB_CHECK_EQ(book.find(1)->qty, 10);
}

NB_TEST(fok_via_submit) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Sell, 100, 10, trades);

    OrderRequest req;
    req.id    = 2;
    req.side  = Side::Buy;
    req.type  = OrderType::FOK;
    req.price = 100;
    req.qty   = 11;
    NB_CHECK_EQ(book.submit(req, trades).status, OrderStatus::Cancelled);

    req.qty = 10;
    NB_CHECK_EQ(book.submit(req, trades).status, OrderStatus::Filled);
    NB_CHECK(book.empty());
}

// ---------------------------------------------------------------------------
// Self-Trade Prevention
// ---------------------------------------------------------------------------

NB_TEST(stp_anonymous_orders_are_never_checked) {
    OrderBook book;
    Trades    trades;
    // trader_id 0 means "no STP" even when a mode is requested.
    book.add_limit(1, Side::Sell, 100, 10, trades, kNewest(kNoTrader));
    const auto r = book.add_limit(2, Side::Buy, 100, 10, trades, kNewest(kNoTrader));
    NB_CHECK_EQ(r.status, OrderStatus::Filled);
    NB_CHECK_EQ(trades.size(), 1u);
}

NB_TEST(stp_mode_none_allows_self_match) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Sell, 100, 10, trades, kTrader(7));
    const auto r = book.add_limit(2, Side::Buy, 100, 10, trades, kTrader(7));
    NB_CHECK_EQ(r.status, OrderStatus::Filled);
    NB_CHECK_EQ(trades.size(), 1u);
}

NB_TEST(stp_different_traders_match_normally) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Sell, 100, 10, trades, kNewest(1));
    const auto r = book.add_limit(2, Side::Buy, 100, 10, trades, kNewest(2));
    NB_CHECK_EQ(r.status, OrderStatus::Filled);
    NB_CHECK_EQ(r.stp_cancels, 0u);
}

NB_TEST(stp_cancel_newest_cancels_incoming_and_keeps_resting) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Sell, 100, 10, trades, kTrader(7));

    const auto r = book.add_limit(2, Side::Buy, 100, 5, trades, kNewest(7));
    NB_CHECK_EQ(r.status, OrderStatus::Cancelled);
    NB_CHECK_EQ(r.cancel_reason, CancelReason::SelfTrade);
    NB_CHECK_EQ(r.filled, 0);
    NB_CHECK_EQ(r.cancelled, 5);
    NB_CHECK(trades.empty());
    NB_CHECK_EQ(book.find(1)->qty, 10);
    NB_CHECK(!book.contains(2));
    NB_CHECK(!book.best_bid().has_value());  // remainder must not rest
}

NB_TEST(stp_cancel_newest_after_partial_fill) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Sell, 100, 10, trades, kTrader(1));
    book.add_limit(2, Side::Sell, 100, 10, trades, kTrader(7));
    book.add_limit(3, Side::Sell, 100, 10, trades, kTrader(2));

    const auto r = book.add_limit(9, Side::Buy, 101, 25, trades, kNewest(7));
    NB_CHECK_EQ(r.status, OrderStatus::Cancelled);
    NB_CHECK_EQ(r.cancel_reason, CancelReason::SelfTrade);
    NB_CHECK_EQ(r.filled, 10);
    NB_CHECK_EQ(r.cancelled, 15);
    NB_CHECK_EQ(trades.size(), 1u);
    NB_CHECK_EQ(trades[0].maker_id, 1u);
    NB_CHECK_EQ(book.queue_at(Side::Sell, 100), (std::vector<OrderId>{2, 3}));
    NB_CHECK(!book.best_bid().has_value());
}

NB_TEST(stp_cancel_oldest_removes_resting_and_continues) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Sell, 100, 10, trades, kTrader(7));
    book.add_limit(2, Side::Sell, 100, 10, trades, kTrader(2));

    const auto r = book.add_limit(9, Side::Buy, 100, 15, trades, kOldest(7));
    NB_CHECK_EQ(r.status, OrderStatus::PartiallyFilled);
    NB_CHECK_EQ(r.filled, 10);
    NB_CHECK_EQ(r.resting, 5);
    NB_CHECK_EQ(r.stp_cancels, 1u);
    NB_CHECK_EQ(book.stp_cancelled(), (std::vector<OrderId>{1}));
    NB_CHECK_EQ(trades.size(), 1u);
    NB_CHECK_EQ(trades[0].maker_id, 2u);
    NB_CHECK(!book.contains(1));
    NB_CHECK(!book.best_ask().has_value());
    NB_CHECK_EQ(book.best_bid(), std::optional<Price>{100});
    NB_CHECK_EQ(book.find(9)->trader_id, 7u);
}

NB_TEST(stp_cancel_oldest_across_levels_with_market_order) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Sell, 100, 5, trades, kTrader(7));
    book.add_limit(2, Side::Sell, 101, 5, trades, kTrader(3));
    book.add_limit(3, Side::Sell, 102, 5, trades, kTrader(7));
    book.add_limit(4, Side::Sell, 103, 5, trades, kTrader(3));

    const auto r = book.add_market(9, Side::Buy, 10, trades, kOldest(7));
    NB_CHECK_EQ(r.status, OrderStatus::Filled);
    NB_CHECK_EQ(r.filled, 10);
    NB_CHECK_EQ(r.stp_cancels, 2u);
    NB_CHECK_EQ(book.stp_cancelled(), (std::vector<OrderId>{1, 3}));
    NB_CHECK_EQ(trades.size(), 2u);
    NB_CHECK_EQ(trades[0].price, 101);
    NB_CHECK_EQ(trades[1].price, 103);
    NB_CHECK(book.empty());
}

NB_TEST(stp_cancel_newest_on_market_and_ioc) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Buy, 100, 10, trades, kTrader(5));

    const auto m = book.add_market(2, Side::Sell, 4, trades, kNewest(5));
    NB_CHECK_EQ(m.status, OrderStatus::Cancelled);
    NB_CHECK_EQ(m.cancel_reason, CancelReason::SelfTrade);

    const auto i = book.add_ioc(3, Side::Sell, 100, 4, trades, kNewest(5));
    NB_CHECK_EQ(i.status, OrderStatus::Cancelled);
    NB_CHECK_EQ(i.cancel_reason, CancelReason::SelfTrade);

    // A plain liquidity miss is reported differently.
    const auto miss = book.add_ioc(4, Side::Sell, 101, 4, trades);
    NB_CHECK_EQ(miss.cancel_reason, CancelReason::NoLiquidity);

    NB_CHECK(trades.empty());
    NB_CHECK_EQ(book.find(1)->qty, 10);
}

NB_TEST(stp_applies_to_modify_reprice) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Buy, 99, 10, trades, kNewest(7));
    book.add_limit(2, Side::Sell, 101, 10, trades, kTrader(7));

    // Repricing into the trader's own ask re-enters with the same trader and mode.
    const auto r = book.modify(1, 101, 10, trades);
    NB_CHECK_EQ(r.status, OrderStatus::Cancelled);
    NB_CHECK_EQ(r.cancel_reason, CancelReason::SelfTrade);
    NB_CHECK(trades.empty());
    NB_CHECK(!book.contains(1));
    NB_CHECK_EQ(book.find(2)->qty, 10);
}

NB_TEST(stp_cancelled_list_resets_per_request) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Sell, 100, 5, trades, kTrader(7));
    book.add_limit(2, Side::Sell, 100, 5, trades, kTrader(1));
    book.add_market(3, Side::Buy, 5, trades, kOldest(7));
    NB_CHECK_EQ(book.stp_cancelled().size(), 1u);

    book.add_limit(4, Side::Buy, 90, 5, trades);
    NB_CHECK(book.stp_cancelled().empty());
}

// ---------------------------------------------------------------------------
// FOK x STP
// ---------------------------------------------------------------------------

NB_TEST(fok_with_stp_cancel_newest_counts_only_liquidity_ahead_of_own_order) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Sell, 100, 5, trades, kTrader(1));
    book.add_limit(2, Side::Sell, 100, 10, trades, kTrader(7));
    book.add_limit(3, Side::Sell, 100, 10, trades, kTrader(2));
    const BookState before = state(book);

    // 25 is on the book, but only 5 is reachable before the trader's own order.
    const auto killed = book.add_fok(9, Side::Buy, 100, 10, trades, kNewest(7));
    NB_CHECK_EQ(killed.status, OrderStatus::Cancelled);
    NB_CHECK_EQ(killed.cancel_reason, CancelReason::FOKUnfillable);
    NB_CHECK(trades.empty());
    NB_CHECK(state(book) == before);

    const auto filled = book.add_fok(10, Side::Buy, 100, 5, trades, kNewest(7));
    NB_CHECK_EQ(filled.status, OrderStatus::Filled);
    NB_CHECK_EQ(trades.size(), 1u);
    NB_CHECK_EQ(trades[0].maker_id, 1u);
}

NB_TEST(fok_with_stp_cancel_oldest_excludes_own_quantity) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Sell, 100, 10, trades, kTrader(7));
    book.add_limit(2, Side::Sell, 100, 5, trades, kTrader(2));

    // Killed FOK must not cancel the trader's resting order either.
    const auto killed = book.add_fok(9, Side::Buy, 100, 10, trades, kOldest(7));
    NB_CHECK_EQ(killed.status, OrderStatus::Cancelled);
    NB_CHECK(book.stp_cancelled().empty());
    NB_CHECK(book.contains(1));
    NB_CHECK(trades.empty());

    const auto filled = book.add_fok(10, Side::Buy, 100, 5, trades, kOldest(7));
    NB_CHECK_EQ(filled.status, OrderStatus::Filled);
    NB_CHECK_EQ(filled.stp_cancels, 1u);
    NB_CHECK(!book.contains(1));
    NB_CHECK(book.empty());
}
