#pragma once

#include <cstdint>
#include <string_view>

namespace nanobook {

// Prices are fixed-point integers expressed in ticks (e.g. 1 tick = 0.0001).
// No floating point is used anywhere on the matching path.
using Price    = std::int64_t;
// Quantities are signed so that malformed input (negative sizes) is detectable
// and can be rejected instead of silently wrapping around.
using Quantity = std::int64_t;
using OrderId  = std::uint64_t;
using SeqNum   = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };

enum class OrderType : std::uint8_t {
    Limit,   // rests any unfilled remainder on the book
    Market,  // executes against any price, remainder is cancelled
    IOC      // executes up to its limit price, remainder is cancelled
};

enum class OrderStatus : std::uint8_t {
    Resting,          // accepted and resting on the book, no execution
    PartiallyFilled,  // executed in part, remainder rests on the book
    Filled,           // fully executed
    Cancelled,        // remainder cancelled (Market / IOC), possibly after fills
    Rejected          // failed validation, book unchanged
};

enum class RejectReason : std::uint8_t {
    None,
    InvalidQuantity,
    InvalidPrice,
    DuplicateOrderId,
    UnknownOrder
};

// A single execution between an incoming (taker) order and a resting (maker) order.
// Trades always print at the maker's price.
struct Trade {
    SeqNum   seq;
    OrderId  taker_id;
    OrderId  maker_id;
    Side     taker_side;
    Price    price;
    Quantity qty;

    friend constexpr bool operator==(const Trade&, const Trade&) = default;
};

// Outcome of an order entry or modify request.
struct ExecResult {
    OrderStatus  status    = OrderStatus::Rejected;
    RejectReason reason    = RejectReason::None;
    Quantity     filled    = 0;  // quantity executed by this request
    Quantity     resting   = 0;  // quantity left resting on the book
    Quantity     cancelled = 0;  // quantity cancelled (Market / IOC remainder)

    [[nodiscard]] constexpr bool ok() const noexcept { return status != OrderStatus::Rejected; }
};

struct OrderRequest {
    OrderId   id    = 0;
    Side      side  = Side::Buy;
    OrderType type  = OrderType::Limit;
    Price     price = 0;  // ignored for Market orders
    Quantity  qty   = 0;
};

[[nodiscard]] constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

[[nodiscard]] constexpr std::string_view to_string(Side s) noexcept {
    return s == Side::Buy ? "Buy" : "Sell";
}

[[nodiscard]] constexpr std::string_view to_string(OrderType t) noexcept {
    switch (t) {
        case OrderType::Limit:  return "Limit";
        case OrderType::Market: return "Market";
        case OrderType::IOC:    return "IOC";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view to_string(OrderStatus s) noexcept {
    switch (s) {
        case OrderStatus::Resting:         return "Resting";
        case OrderStatus::PartiallyFilled: return "PartiallyFilled";
        case OrderStatus::Filled:          return "Filled";
        case OrderStatus::Cancelled:       return "Cancelled";
        case OrderStatus::Rejected:        return "Rejected";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view to_string(RejectReason r) noexcept {
    switch (r) {
        case RejectReason::None:             return "None";
        case RejectReason::InvalidQuantity:  return "InvalidQuantity";
        case RejectReason::InvalidPrice:     return "InvalidPrice";
        case RejectReason::DuplicateOrderId: return "DuplicateOrderId";
        case RejectReason::UnknownOrder:     return "UnknownOrder";
    }
    return "?";
}

}  // namespace nanobook
