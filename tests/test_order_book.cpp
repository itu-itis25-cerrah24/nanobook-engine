#include <optional>
#include <vector>

#include "nanobook/order_book.hpp"
#include "test_framework.hpp"

using namespace nanobook;

namespace {

using Trades = std::vector<Trade>;

// Asserts that the book is never left crossed (best bid strictly below best ask).
bool uncrossed(const OrderBook& book) {
    const auto bid = book.best_bid();
    const auto ask = book.best_ask();
    return !bid || !ask || *bid < *ask;
}

}  // namespace

// ---------------------------------------------------------------------------
// Empty book
// ---------------------------------------------------------------------------

NB_TEST(empty_book_has_no_market_data) {
    OrderBook book;
    NB_CHECK(book.empty());
    NB_CHECK(!book.best_bid().has_value());
    NB_CHECK(!book.best_ask().has_value());
    NB_CHECK(!book.spread().has_value());

    const auto snap = book.snapshot(5);
    NB_CHECK(snap.bids.empty());
    NB_CHECK(snap.asks.empty());
    NB_CHECK(!snap.spread().has_value());
}

NB_TEST(market_order_on_empty_book_is_cancelled) {
    OrderBook book;
    Trades    trades;
    const auto r = book.add_market(1, Side::Buy, 100, trades);
    NB_CHECK_EQ(r.status, OrderStatus::Cancelled);
    NB_CHECK_EQ(r.filled, 0);
    NB_CHECK_EQ(r.cancelled, 100);
    NB_CHECK(trades.empty());
    NB_CHECK(book.empty());
}

NB_TEST(cancel_unknown_order_returns_false) {
    OrderBook book;
    NB_CHECK(!book.cancel(42));
}

// ---------------------------------------------------------------------------
// Resting and simple fills
// ---------------------------------------------------------------------------

NB_TEST(non_crossing_limits_rest_on_book) {
    OrderBook book;
    Trades    trades;
    const auto b = book.add_limit(1, Side::Buy, 99, 10, trades);
    const auto a = book.add_limit(2, Side::Sell, 101, 20, trades);

    NB_CHECK_EQ(b.status, OrderStatus::Resting);
    NB_CHECK_EQ(b.resting, 10);
    NB_CHECK_EQ(a.status, OrderStatus::Resting);
    NB_CHECK(trades.empty());
    NB_CHECK_EQ(book.best_bid(), std::optional<Price>{99});
    NB_CHECK_EQ(book.best_ask(), std::optional<Price>{101});
    NB_CHECK_EQ(book.spread(), std::optional<Price>{2});
    NB_CHECK_EQ(book.order_count(), 2u);
}

NB_TEST(exact_fill_removes_both_orders) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Sell, 100, 50, trades);
    const auto r = book.add_limit(2, Side::Buy, 100, 50, trades);

    NB_CHECK_EQ(r.status, OrderStatus::Filled);
    NB_CHECK_EQ(r.filled, 50);
    NB_CHECK_EQ(r.resting, 0);
    NB_CHECK_EQ(trades.size(), 1u);
    NB_CHECK_EQ(trades[0].taker_id, 2u);
    NB_CHECK_EQ(trades[0].maker_id, 1u);
    NB_CHECK_EQ(trades[0].taker_side, Side::Buy);
    NB_CHECK_EQ(trades[0].price, 100);
    NB_CHECK_EQ(trades[0].qty, 50);
    NB_CHECK(book.empty());
    NB_CHECK(!book.contains(1));
    NB_CHECK(!book.contains(2));
}

NB_TEST(partial_fill_of_resting_order) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Sell, 100, 50, trades);
    const auto r = book.add_limit(2, Side::Buy, 100, 20, trades);

    NB_CHECK_EQ(r.status, OrderStatus::Filled);
    NB_CHECK_EQ(r.filled, 20);
    NB_CHECK_EQ(trades.size(), 1u);
    const auto maker = book.find(1);
    NB_CHECK(maker.has_value());
    NB_CHECK_EQ(maker->qty, 30);
    NB_CHECK_EQ(book.snapshot(1).asks.at(0), (LevelView{100, 30, 1}));
}

NB_TEST(partial_fill_of_incoming_order_rests_remainder) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Buy, 100, 30, trades);
    const auto r = book.add_limit(2, Side::Sell, 100, 80, trades);

    NB_CHECK_EQ(r.status, OrderStatus::PartiallyFilled);
    NB_CHECK_EQ(r.filled, 30);
    NB_CHECK_EQ(r.resting, 50);
    NB_CHECK(!book.contains(1));
    NB_CHECK(!book.best_bid().has_value());
    NB_CHECK_EQ(book.best_ask(), std::optional<Price>{100});
    NB_CHECK_EQ(book.find(2)->qty, 50);
}

// ---------------------------------------------------------------------------
// Multi-level sweeps
// ---------------------------------------------------------------------------

NB_TEST(limit_order_sweeps_multiple_levels_and_rests_remainder) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Sell, 101, 10, trades);
    book.add_limit(2, Side::Sell, 102, 20, trades);
    book.add_limit(3, Side::Sell, 103, 30, trades);
    book.add_limit(4, Side::Sell, 105, 40, trades);

    const auto r = book.add_limit(9, Side::Buy, 103, 100, trades);

    NB_CHECK_EQ(r.status, OrderStatus::PartiallyFilled);
    NB_CHECK_EQ(r.filled, 60);
    NB_CHECK_EQ(r.resting, 40);
    NB_CHECK_EQ(trades.size(), 3u);
    // Each fill prints at the maker's price, best price first.
    NB_CHECK_EQ(trades[0], (Trade{1, 9, 1, Side::Buy, 101, 10}));
    NB_CHECK_EQ(trades[1], (Trade{2, 9, 2, Side::Buy, 102, 20}));
    NB_CHECK_EQ(trades[2], (Trade{3, 9, 3, Side::Buy, 103, 30}));

    NB_CHECK_EQ(book.best_bid(), std::optional<Price>{103});
    NB_CHECK_EQ(book.best_ask(), std::optional<Price>{105});
    NB_CHECK_EQ(book.ask_levels(), 1u);
    NB_CHECK(uncrossed(book));
}

NB_TEST(sell_sweep_walks_bids_from_highest_price) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Buy, 98, 10, trades);
    book.add_limit(2, Side::Buy, 100, 10, trades);
    book.add_limit(3, Side::Buy, 99, 10, trades);

    const auto r = book.add_limit(9, Side::Sell, 99, 15, trades);

    NB_CHECK_EQ(r.status, OrderStatus::Filled);
    NB_CHECK_EQ(trades.size(), 2u);
    NB_CHECK_EQ(trades[0].price, 100);
    NB_CHECK_EQ(trades[0].maker_id, 2u);
    NB_CHECK_EQ(trades[1].price, 99);
    NB_CHECK_EQ(trades[1].maker_id, 3u);
    NB_CHECK_EQ(trades[1].qty, 5);
    NB_CHECK_EQ(book.find(3)->qty, 5);  // 10 resting - 5 taken
    NB_CHECK(book.contains(1));         // 98 bid never reached
    NB_CHECK_EQ(book.best_bid(), std::optional<Price>{99});
    NB_CHECK(!book.best_ask().has_value());
}

// ---------------------------------------------------------------------------
// Price-time priority
// ---------------------------------------------------------------------------

NB_TEST(fifo_priority_within_price_level) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Sell, 100, 10, trades);
    book.add_limit(2, Side::Sell, 100, 10, trades);
    book.add_limit(3, Side::Sell, 100, 10, trades);
    NB_CHECK_EQ(book.queue_at(Side::Sell, 100), (std::vector<OrderId>{1, 2, 3}));

    book.add_limit(10, Side::Buy, 100, 15, trades);

    NB_CHECK_EQ(trades.size(), 2u);
    NB_CHECK_EQ(trades[0].maker_id, 1u);
    NB_CHECK_EQ(trades[0].qty, 10);
    NB_CHECK_EQ(trades[1].maker_id, 2u);
    NB_CHECK_EQ(trades[1].qty, 5);
    NB_CHECK_EQ(book.queue_at(Side::Sell, 100), (std::vector<OrderId>{2, 3}));
    NB_CHECK_EQ(book.find(2)->qty, 5);
}

NB_TEST(better_price_beats_earlier_time) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Buy, 99, 10, trades);   // earlier, worse price
    book.add_limit(2, Side::Buy, 100, 10, trades);  // later, better price

    book.add_limit(3, Side::Sell, 99, 10, trades);
    NB_CHECK_EQ(trades.size(), 1u);
    NB_CHECK_EQ(trades[0].maker_id, 2u);
    NB_CHECK_EQ(trades[0].price, 100);
}

// ---------------------------------------------------------------------------
// Market orders
// ---------------------------------------------------------------------------

NB_TEST(market_order_sweeps_until_filled) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Sell, 100, 10, trades);
    book.add_limit(2, Side::Sell, 150, 10, trades);
    book.add_limit(3, Side::Sell, 200, 10, trades);

    const auto r = book.add_market(9, Side::Buy, 15, trades);
    NB_CHECK_EQ(r.status, OrderStatus::Filled);
    NB_CHECK_EQ(r.filled, 15);
    NB_CHECK_EQ(trades.size(), 2u);
    NB_CHECK_EQ(trades[1].price, 150);
    NB_CHECK(!book.contains(9));  // market orders never rest
}

NB_TEST(market_order_remainder_is_cancelled) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Buy, 100, 10, trades);
    book.add_limit(2, Side::Buy, 90, 10, trades);

    const auto r = book.add_market(9, Side::Sell, 50, trades);
    NB_CHECK_EQ(r.status, OrderStatus::Cancelled);
    NB_CHECK_EQ(r.filled, 20);
    NB_CHECK_EQ(r.cancelled, 30);
    NB_CHECK_EQ(r.resting, 0);
    NB_CHECK(book.empty());
}

// ---------------------------------------------------------------------------
// IOC
// ---------------------------------------------------------------------------

NB_TEST(ioc_partial_fill_cancels_remainder) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Sell, 100, 10, trades);
    book.add_limit(2, Side::Sell, 101, 10, trades);
    book.add_limit(3, Side::Sell, 102, 10, trades);

    const auto r = book.add_ioc(9, Side::Buy, 101, 50, trades);
    NB_CHECK_EQ(r.status, OrderStatus::Cancelled);
    NB_CHECK_EQ(r.filled, 20);
    NB_CHECK_EQ(r.cancelled, 30);
    NB_CHECK_EQ(trades.size(), 2u);
    NB_CHECK(!book.contains(9));
    NB_CHECK(!book.best_bid().has_value());  // IOC remainder must not rest
    NB_CHECK_EQ(book.best_ask(), std::optional<Price>{102});
}

NB_TEST(ioc_full_fill) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Buy, 100, 40, trades);
    const auto r = book.add_ioc(9, Side::Sell, 100, 40, trades);
    NB_CHECK_EQ(r.status, OrderStatus::Filled);
    NB_CHECK_EQ(r.cancelled, 0);
    NB_CHECK(book.empty());
}

NB_TEST(ioc_without_cross_is_cancelled_and_book_unchanged) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Sell, 105, 10, trades);

    const auto r = book.add_ioc(9, Side::Buy, 104, 10, trades);
    NB_CHECK_EQ(r.status, OrderStatus::Cancelled);
    NB_CHECK_EQ(r.filled, 0);
    NB_CHECK_EQ(r.cancelled, 10);
    NB_CHECK(trades.empty());
    NB_CHECK_EQ(book.order_count(), 1u);
    NB_CHECK(!book.best_bid().has_value());
    NB_CHECK_EQ(book.find(1)->qty, 10);
}

NB_TEST(submit_dispatches_by_order_type) {
    OrderBook book;
    Trades    trades;
    NB_CHECK_EQ(book.submit({1, Side::Sell, OrderType::Limit, 100, 10}, trades).status,
                OrderStatus::Resting);
    NB_CHECK_EQ(book.submit({2, Side::Buy, OrderType::IOC, 99, 10}, trades).status,
                OrderStatus::Cancelled);
    NB_CHECK_EQ(book.submit({3, Side::Buy, OrderType::Market, 0, 4}, trades).status,
                OrderStatus::Filled);
    NB_CHECK_EQ(book.find(1)->qty, 6);
}

// ---------------------------------------------------------------------------
// Cancel
// ---------------------------------------------------------------------------

NB_TEST(cancel_removes_order_and_empty_level) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Buy, 100, 10, trades);
    book.add_limit(2, Side::Buy, 99, 10, trades);

    NB_CHECK(book.cancel(1));
    NB_CHECK(!book.contains(1));
    NB_CHECK_EQ(book.bid_levels(), 1u);
    NB_CHECK_EQ(book.best_bid(), std::optional<Price>{99});
    NB_CHECK(!book.cancel(1));  // double cancel
}

NB_TEST(cancel_middle_of_queue_preserves_fifo) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Sell, 100, 10, trades);
    book.add_limit(2, Side::Sell, 100, 20, trades);
    book.add_limit(3, Side::Sell, 100, 30, trades);

    NB_CHECK(book.cancel(2));
    NB_CHECK_EQ(book.queue_at(Side::Sell, 100), (std::vector<OrderId>{1, 3}));
    NB_CHECK_EQ(book.snapshot(1).asks.at(0), (LevelView{100, 40, 2}));

    book.add_limit(9, Side::Buy, 100, 15, trades);
    NB_CHECK_EQ(trades.size(), 2u);
    NB_CHECK_EQ(trades[0].maker_id, 1u);
    NB_CHECK_EQ(trades[1].maker_id, 3u);
}

NB_TEST(filled_order_cannot_be_cancelled) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Sell, 100, 10, trades);
    book.add_market(2, Side::Buy, 10, trades);
    NB_CHECK(!book.cancel(1));
}

NB_TEST(pool_slots_are_recycled_after_cancel) {
    OrderBook book(4);
    Trades    trades;
    for (OrderId round = 0; round < 1000; ++round) {
        book.add_limit(round, Side::Buy, 100, 1, trades);
        NB_CHECK(book.cancel(round));
    }
    NB_CHECK(book.empty());
    NB_CHECK_EQ(book.bid_levels(), 0u);
}

// ---------------------------------------------------------------------------
// Modify
// ---------------------------------------------------------------------------

NB_TEST(modify_reduce_quantity_keeps_priority) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Buy, 100, 50, trades);
    book.add_limit(2, Side::Buy, 100, 50, trades);

    const auto r = book.modify(1, 100, 20, trades);
    NB_CHECK_EQ(r.status, OrderStatus::Resting);
    NB_CHECK_EQ(r.resting, 20);
    NB_CHECK_EQ(book.queue_at(Side::Buy, 100), (std::vector<OrderId>{1, 2}));
    NB_CHECK_EQ(book.snapshot(1).bids.at(0), (LevelView{100, 70, 2}));
}

NB_TEST(modify_increase_quantity_loses_priority) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Buy, 100, 50, trades);
    book.add_limit(2, Side::Buy, 100, 50, trades);

    const auto r = book.modify(1, 100, 80, trades);
    NB_CHECK_EQ(r.status, OrderStatus::Resting);
    NB_CHECK_EQ(book.queue_at(Side::Buy, 100), (std::vector<OrderId>{2, 1}));
    NB_CHECK_EQ(book.snapshot(1).bids.at(0), (LevelView{100, 130, 2}));
}

NB_TEST(modify_price_moves_order_to_new_level) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Sell, 105, 10, trades);
    const auto r = book.modify(1, 103, 10, trades);
    NB_CHECK_EQ(r.status, OrderStatus::Resting);
    NB_CHECK_EQ(book.ask_levels(), 1u);
    NB_CHECK_EQ(book.best_ask(), std::optional<Price>{103});
    NB_CHECK_EQ(book.find(1)->price, 103);
}

NB_TEST(modify_into_cross_executes) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Sell, 101, 10, trades);
    book.add_limit(2, Side::Buy, 99, 25, trades);

    const auto r = book.modify(2, 101, 25, trades);
    NB_CHECK_EQ(r.status, OrderStatus::PartiallyFilled);
    NB_CHECK_EQ(r.filled, 10);
    NB_CHECK_EQ(r.resting, 15);
    NB_CHECK_EQ(trades.size(), 1u);
    NB_CHECK_EQ(trades[0].taker_id, 2u);
    NB_CHECK_EQ(trades[0].price, 101);
    NB_CHECK_EQ(book.best_bid(), std::optional<Price>{101});
    NB_CHECK(!book.best_ask().has_value());
}

NB_TEST(modify_rejects_unknown_and_invalid) {
    OrderBook book;
    Trades    trades;
    NB_CHECK_EQ(book.modify(7, 100, 10, trades).reason, RejectReason::UnknownOrder);

    book.add_limit(1, Side::Buy, 100, 10, trades);
    NB_CHECK_EQ(book.modify(1, 100, 0, trades).reason, RejectReason::InvalidQuantity);
    NB_CHECK_EQ(book.modify(1, 100, -5, trades).reason, RejectReason::InvalidQuantity);
    NB_CHECK_EQ(book.modify(1, 0, 10, trades).reason, RejectReason::InvalidPrice);
    NB_CHECK_EQ(book.find(1)->qty, 10);  // untouched on reject
}

// ---------------------------------------------------------------------------
// Self-crossing / aggressive orders
// ---------------------------------------------------------------------------

NB_TEST(aggressive_limit_crosses_spread_at_maker_price) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Buy, 100, 10, trades);
    book.add_limit(2, Side::Sell, 102, 10, trades);

    // Sell priced far through the bid: must trade at the resting bid's price,
    // not its own, and never leave a crossed book.
    const auto r = book.add_limit(3, Side::Sell, 50, 15, trades);
    NB_CHECK_EQ(r.status, OrderStatus::PartiallyFilled);
    NB_CHECK_EQ(trades.size(), 1u);
    NB_CHECK_EQ(trades[0].price, 100);
    NB_CHECK_EQ(book.best_ask(), std::optional<Price>{50});
    NB_CHECK(!book.best_bid().has_value());
    NB_CHECK(uncrossed(book));
}

NB_TEST(book_never_crossed_after_mixed_flow) {
    OrderBook book;
    Trades    trades;
    OrderId   id = 1;
    for (Price p = 90; p <= 110; ++p) {
        book.add_limit(id++, Side::Buy, p, 5, trades);
        NB_CHECK(uncrossed(book));
        book.add_limit(id++, Side::Sell, 200 - p, 7, trades);
        NB_CHECK(uncrossed(book));
    }
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

NB_TEST(invalid_quantities_are_rejected) {
    OrderBook book;
    Trades    trades;
    NB_CHECK_EQ(book.add_limit(1, Side::Buy, 100, 0, trades).reason, RejectReason::InvalidQuantity);
    NB_CHECK_EQ(book.add_limit(2, Side::Buy, 100, -10, trades).reason, RejectReason::InvalidQuantity);
    NB_CHECK_EQ(book.add_market(3, Side::Buy, 0, trades).reason, RejectReason::InvalidQuantity);
    NB_CHECK_EQ(book.add_ioc(4, Side::Sell, 100, -1, trades).reason, RejectReason::InvalidQuantity);
    NB_CHECK(book.empty());
    NB_CHECK(trades.empty());
}

NB_TEST(invalid_prices_are_rejected) {
    OrderBook book;
    Trades    trades;
    const auto r = book.add_limit(1, Side::Buy, 0, 10, trades);
    NB_CHECK_EQ(r.status, OrderStatus::Rejected);
    NB_CHECK_EQ(r.reason, RejectReason::InvalidPrice);
    NB_CHECK_EQ(book.add_limit(2, Side::Sell, -100, 10, trades).reason, RejectReason::InvalidPrice);
    NB_CHECK_EQ(book.add_ioc(3, Side::Buy, 0, 10, trades).reason, RejectReason::InvalidPrice);
    NB_CHECK(book.empty());
}

NB_TEST(duplicate_order_id_is_rejected) {
    OrderBook book;
    Trades    trades;
    book.add_limit(1, Side::Buy, 100, 10, trades);
    NB_CHECK_EQ(book.add_limit(1, Side::Buy, 101, 10, trades).reason, RejectReason::DuplicateOrderId);
    NB_CHECK_EQ(book.add_ioc(1, Side::Sell, 100, 10, trades).reason, RejectReason::DuplicateOrderId);
    NB_CHECK(trades.empty());
    NB_CHECK_EQ(book.find(1)->qty, 10);
}

// ---------------------------------------------------------------------------
// Level-2 market data
// ---------------------------------------------------------------------------

NB_TEST(level2_snapshot_aggregates_and_truncates) {
    OrderBook book;
    Trades    trades;
    OrderId   id = 1;
    for (Price p = 95; p <= 99; ++p) {
        book.add_limit(id++, Side::Buy, p, 10, trades);
        book.add_limit(id++, Side::Buy, p, 5, trades);
    }
    for (Price p = 101; p <= 104; ++p) book.add_limit(id++, Side::Sell, p, 20, trades);

    const auto snap = book.snapshot(3);
    NB_CHECK_EQ(snap.bids.size(), 3u);
    NB_CHECK_EQ(snap.asks.size(), 3u);
    NB_CHECK_EQ(snap.bids[0], (LevelView{99, 15, 2}));
    NB_CHECK_EQ(snap.bids[1], (LevelView{98, 15, 2}));
    NB_CHECK_EQ(snap.bids[2], (LevelView{97, 15, 2}));
    NB_CHECK_EQ(snap.asks[0], (LevelView{101, 20, 1}));
    NB_CHECK_EQ(snap.asks[2], (LevelView{103, 20, 1}));
    NB_CHECK_EQ(snap.best_bid(), std::optional<Price>{99});
    NB_CHECK_EQ(snap.best_ask(), std::optional<Price>{101});
    NB_CHECK_EQ(snap.spread(), std::optional<Price>{2});

    const auto deep = book.snapshot(100);
    NB_CHECK_EQ(deep.bids.size(), 5u);
    NB_CHECK_EQ(deep.asks.size(), 4u);
}

NB_TEST(level2_snapshot_reflects_partial_fills) {
    OrderBook      book;
    Trades         trades;
    Level2Snapshot snap;
    book.add_limit(1, Side::Sell, 100, 30, trades);
    book.add_limit(2, Side::Sell, 100, 30, trades);
    book.add_market(3, Side::Buy, 40, trades);

    book.snapshot(5, snap);
    NB_CHECK_EQ(snap.asks.size(), 1u);
    NB_CHECK_EQ(snap.asks[0], (LevelView{100, 20, 1}));
}

NB_TEST(trade_sequence_numbers_are_monotonic) {
    OrderBook book;
    Trades    trades;
    for (OrderId i = 1; i <= 5; ++i) book.add_limit(i, Side::Sell, 100 + static_cast<Price>(i), 1, trades);
    book.add_market(99, Side::Buy, 5, trades);
    book.add_limit(100, Side::Buy, 50, 1, trades);
    book.add_market(101, Side::Sell, 1, trades);

    NB_CHECK_EQ(trades.size(), 6u);
    for (std::size_t i = 0; i < trades.size(); ++i) NB_CHECK_EQ(trades[i].seq, i + 1);
}

int main() { return nbtest::run_all(); }
