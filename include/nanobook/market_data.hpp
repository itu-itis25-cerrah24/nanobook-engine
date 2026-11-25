#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "nanobook/types.hpp"

namespace nanobook {

// Aggregated view of one price level.
struct LevelView {
    Price         price  = 0;
    Quantity      qty    = 0;  // total open quantity at this price
    std::uint32_t orders = 0;  // number of resting orders at this price

    friend constexpr bool operator==(const LevelView&, const LevelView&) = default;
};

// Level-2 (market-by-price) snapshot. Both sides are ordered best-first:
// bids by descending price, asks by ascending price.
struct Level2Snapshot {
    std::vector<LevelView> bids;
    std::vector<LevelView> asks;

    [[nodiscard]] std::optional<Price> best_bid() const noexcept {
        if (bids.empty()) return std::nullopt;
        return bids.front().price;
    }

    [[nodiscard]] std::optional<Price> best_ask() const noexcept {
        if (asks.empty()) return std::nullopt;
        return asks.front().price;
    }

    // Spread in ticks; empty when either side of the book is empty.
    [[nodiscard]] std::optional<Price> spread() const noexcept {
        if (bids.empty() || asks.empty()) return std::nullopt;
        return asks.front().price - bids.front().price;
    }
};

}  // namespace nanobook
