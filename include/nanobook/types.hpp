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
// Owner of an order for self-trade prevention. 0 means "anonymous": never STP-checked.
using TraderId = std::uint32_t;

inline constexpr TraderId kNoTrader = 0;

enum class Side : std::uint8_t { Buy, Sell };

enum class OrderType : std::uint8_t {
    Limit,   // rests any unfilled remainder on the book
    Market,  // executes against any price, remainder is cancelled
    IOC,     // Immediate-or-Cancel: executes up to its limit price, remainder is cancelled
    FOK      // Fill-or-Kill: executes in full up to its limit price, or not at all
};

// Self-trade prevention, applied when an incoming order would match a resting
// order of the same (non-zero) trader. The incoming order's mode decides.
enum class STPMode : std::uint8_t {
    None,          // self-matching allowed
    CancelNewest,  // cancel the incoming order's remaining quantity
    CancelOldest   // cancel the resting order and keep matching
};

enum class OrderStatus : std::uint8_t {
    Resting,          // accepted and resting on the book, no execution
    PartiallyFilled,  // executed in part, remainder rests on the book
    Filled,           // fully executed
    Cancelled,        // remainder cancelled (see CancelReason), possibly after fills
    Rejected          // failed validation, book unchanged
};

enum class RejectReason : std::uint8_t {
    None,
    InvalidQuantity,
    InvalidPrice,
    DuplicateOrderId,
    UnknownOrder
};

// Why the remainder of an accepted order was cancelled.
enum class CancelReason : std::uint8_t {
    None,
    NoLiquidity,     // Market / IOC ran out of crossing liquidity
    FOKUnfillable,   // Fill-or-Kill could not be filled in full: nothing executed
    SelfTrade        // STP CancelNewest stopped the incoming order
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

// Optional per-order attributes. Defaults keep the original (anonymous) behaviour.
struct OrderOptions {
    TraderId trader_id = kNoTrader;
    STPMode  stp       = STPMode::None;
};

// Outcome of an order entry or modify request.
struct ExecResult {
    OrderStatus   status        = OrderStatus::Rejected;
    RejectReason  reason        = RejectReason::None;
    Quantity      filled        = 0;  // quantity executed by this request
    Quantity      resting       = 0;  // quantity left resting on the book
    Quantity      cancelled     = 0;  // quantity of this order cancelled
    CancelReason  cancel_reason = CancelReason::None;
    std::uint32_t stp_cancels   = 0;  // resting orders removed by STP CancelOldest

    [[nodiscard]] constexpr bool ok() const noexcept { return status != OrderStatus::Rejected; }
};

struct OrderRequest {
    OrderId   id        = 0;
    Side      side      = Side::Buy;
    OrderType type      = OrderType::Limit;
    Price     price     = 0;  // ignored for Market orders
    Quantity  qty       = 0;
    TraderId  trader_id = kNoTrader;
    STPMode   stp       = STPMode::None;
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
        case OrderType::FOK:    return "FOK";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view to_string(STPMode m) noexcept {
    switch (m) {
        case STPMode::None:         return "None";
        case STPMode::CancelNewest: return "CancelNewest";
        case STPMode::CancelOldest: return "CancelOldest";
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

[[nodiscard]] constexpr std::string_view to_string(CancelReason r) noexcept {
    switch (r) {
        case CancelReason::None:          return "None";
        case CancelReason::NoLiquidity:   return "NoLiquidity";
        case CancelReason::FOKUnfillable: return "FOKUnfillable";
        case CancelReason::SelfTrade:     return "SelfTrade";
    }
    return "?";
}

}  // namespace nanobook
