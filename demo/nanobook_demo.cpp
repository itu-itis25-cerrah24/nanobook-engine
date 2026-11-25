// NanoBook live terminal visualizer.
//
// Drives the matching engine with a synthetic market feed and renders an
// ANSI-colored Level-2 order book with depth bars, spread / mid and a scrolling
// tape of executed trades.
//
//   nanobook_demo                       live, redraws until Ctrl+C
//   nanobook_demo --frames 1            one static frame (same as --snapshot)
//   nanobook_demo --symbol NVDA --depth 8 --fps 15

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "nanobook/order_book.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace nanobook;

namespace {

// ---------------------------------------------------------------------------
// Instruments
// ---------------------------------------------------------------------------

struct Instrument {
    std::string_view symbol;
    std::string_view unit;      // quantity unit shown in the stats line
    int              price_dp;  // decimals of one price tick
    int              qty_dp;    // decimals of one quantity unit
    Price            start;     // opening fair value, in ticks
    Price            step;      // price-level granularity, in ticks
    Quantity         min_qty;
    Quantity         max_qty;
};

constexpr Instrument kBtcUsd{"BTC-USD", "BTC", 2, 4, 6'743'250, 50, 10, 12'000};
constexpr Instrument kNvda{"NVDA", "shares", 2, 0, 18'250, 1, 1, 900};

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------

std::string group_thousands(std::string digits) {
    for (auto i = static_cast<std::ptrdiff_t>(digits.size()) - 3; i > 0; i -= 3) {
        digits.insert(static_cast<std::size_t>(i), ",");
    }
    return digits;
}

// Renders a fixed-point integer, e.g. (6743250, 2) -> "67,432.50".
std::string fixed(std::int64_t value, int decimals) {
    const bool    negative = value < 0;
    std::uint64_t mag = negative ? 0 - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value);
    std::uint64_t scale = 1;
    for (int i = 0; i < decimals; ++i) scale *= 10;

    std::string out = negative ? "-" : "";
    out += group_thousands(std::to_string(mag / scale));
    if (decimals > 0) {
        std::string frac = std::to_string(mag % scale);
        frac.insert(0, static_cast<std::size_t>(decimals) - frac.size(), '0');
        out += '.';
        out += frac;
    }
    return out;
}

std::string clock_text(std::int64_t ms) {
    const auto two = [](std::int64_t v) {
        std::string s = std::to_string(v);
        return s.size() < 2 ? "0" + s : s;
    };
    std::string millis = std::to_string(ms % 1000);
    millis.insert(0, 3 - millis.size(), '0');
    return two(ms / 3'600'000 % 24) + ":" + two(ms / 60'000 % 60) + ":" + two(ms / 1000 % 60) + "." + millis;
}

namespace ansi {
constexpr std::string_view reset  = "\x1b[0m";
constexpr std::string_view bold   = "\x1b[1m";
constexpr std::string_view dim    = "\x1b[2m";
constexpr std::string_view red    = "\x1b[31m";
constexpr std::string_view green  = "\x1b[32m";
constexpr std::string_view yellow = "\x1b[33m";
constexpr std::string_view cyan   = "\x1b[36m";
constexpr std::string_view bred   = "\x1b[1;91m";
constexpr std::string_view bgreen = "\x1b[1;92m";
constexpr std::string_view title  = "\x1b[1;30;46m";
}  // namespace ansi

// A terminal line that tracks its visible width separately from its byte length,
// so ANSI escapes and multi-byte glyphs never break column alignment.
class Line {
public:
    explicit Line(bool color) : color_(color) {}

    Line& text(std::string_view s, std::string_view style = {}) { return raw(s, s.size(), style); }

    Line& glyph(std::string_view g, std::size_t count, std::string_view style = {}) {
        std::string s;
        for (std::size_t i = 0; i < count; ++i) s += g;
        return raw(s, count, style);
    }

    Line& right(std::string_view s, std::size_t width, std::string_view style = {}) {
        if (s.size() < width) pad(width - s.size());
        return text(s, style);
    }

    Line& pad(std::size_t n) {
        buf_.append(n, ' ');
        width_ += n;
        return *this;
    }

    Line& pad_to(std::size_t column) { return width_ < column ? pad(column - width_) : *this; }

    Line& append(const Line& other) {
        buf_ += other.buf_;
        width_ += other.width_;
        return *this;
    }

    [[nodiscard]] std::size_t        width() const noexcept { return width_; }
    [[nodiscard]] const std::string& str() const noexcept { return buf_; }

private:
    Line& raw(std::string_view s, std::size_t visible, std::string_view style) {
        if (color_ && !style.empty()) {
            buf_ += style;
            buf_ += s;
            buf_ += ansi::reset;
        } else {
            buf_ += s;
        }
        width_ += visible;
        return *this;
    }

    bool        color_;
    std::string buf_;
    std::size_t width_ = 0;
};

// ---------------------------------------------------------------------------
// Synthetic market feed
// ---------------------------------------------------------------------------

struct Print {
    std::int64_t time_ms;
    Side         side;  // aggressor side
    Price        price;
    Quantity     qty;
};

class MarketFeed {
public:
    MarketFeed(const Instrument& inst, std::uint64_t seed)
        : inst_(inst), rng_(seed), book_(1u << 14), fair_(inst.start) {
        trades_.reserve(256);
    }

    void step() {
        clock_ms_ += tick_ms_(rng_);
        ++events_;

        // Fair value follows a slow random walk; quotes are placed around it.
        if (pct_(rng_) < 4) fair_ += coin_(rng_) == 0 ? inst_.step : -inst_.step;
        fair_ = std::max(fair_, 50 * inst_.step);

        const Side side         = coin_(rng_) == 0 ? Side::Buy : Side::Sell;
        const int  cancel_share = book_.order_count() > kTargetOrders ? 45 : 24;
        const int  roll         = pct_(rng_);

        trades_.clear();
        if (roll < cancel_share) {
            if (const OrderId id = pick_live(); id != 0) book_.cancel(id);
        } else if (roll < cancel_share + 6) {
            book_.add_market(next_id_++, side, draw_qty() / 2 + 1, trades_);
        } else if (roll < cancel_share + 9) {
            const Price px = side == Side::Buy ? fair_ + inst_.step : fair_ - inst_.step;
            book_.add_ioc(next_id_++, side, px, draw_qty(), trades_);
        } else {
            const bool  marketable = pct_(rng_) < 6;
            const Price levels     = marketable ? -levels_through_(rng_) : 1 + depth_(rng_);
            const Price px         = side == Side::Buy ? fair_ - levels * inst_.step
                                                       : fair_ + levels * inst_.step;
            const OrderId id = next_id_++;
            book_.add_limit(id, side, px, draw_qty(), trades_);
            if (book_.contains(id)) live_.push_back(id);
        }

        for (std::size_t i = 0; i < trades_.size(); ++i) {
            const Trade& t = trades_[i];
            if (last_price_ != 0 && t.price != last_price_) tick_dir_ = t.price > last_price_ ? 1 : -1;
            last_price_ = t.price;
            volume_ += t.qty;
            ++trade_count_;
            // The tape shows one print per aggressor per price level, like a consolidated feed.
            if (i > 0 && trades_[i - 1].price == t.price) {
                tape_.front().qty += t.qty;
            } else {
                tape_.push_front(Print{clock_ms_, t.taker_side, t.price, t.qty});
            }
        }
        while (tape_.size() > kTapeLength) tape_.pop_back();
    }

    [[nodiscard]] const OrderBook&         book() const noexcept { return book_; }
    [[nodiscard]] const std::deque<Print>& tape() const noexcept { return tape_; }
    [[nodiscard]] std::int64_t             clock_ms() const noexcept { return clock_ms_; }
    [[nodiscard]] Price                    last_price() const noexcept { return last_price_; }
    [[nodiscard]] int                      tick_dir() const noexcept { return tick_dir_; }
    [[nodiscard]] Quantity                 volume() const noexcept { return volume_; }
    [[nodiscard]] std::uint64_t            trade_count() const noexcept { return trade_count_; }
    [[nodiscard]] std::uint64_t            events() const noexcept { return events_; }

private:
    static constexpr std::size_t kTargetOrders = 1'200;
    static constexpr std::size_t kTapeLength   = 64;

    Quantity draw_qty() {
        Quantity q = qty_(rng_);
        if (pct_(rng_) < 8) q *= 4;  // occasional block order
        return std::clamp(q, inst_.min_qty, inst_.max_qty);
    }

    OrderId pick_live() {
        while (!live_.empty()) {
            std::uniform_int_distribution<std::size_t> pick(0, live_.size() - 1);
            const std::size_t                          i  = pick(rng_);
            const OrderId                              id = live_[i];
            live_[i] = live_.back();
            live_.pop_back();
            if (book_.contains(id)) return id;
        }
        return 0;
    }

    Instrument                              inst_;
    std::mt19937_64                         rng_;
    std::uniform_int_distribution<int>      pct_{0, 99};
    std::uniform_int_distribution<int>      coin_{0, 1};
    std::uniform_int_distribution<int>      tick_ms_{0, 6};
    std::geometric_distribution<Price>      depth_{0.22};
    std::uniform_int_distribution<Price>    levels_through_{0, 2};
    std::uniform_int_distribution<Quantity> qty_{inst_.min_qty, inst_.max_qty / 4};

    OrderBook            book_;
    std::vector<Trade>   trades_;
    std::vector<OrderId> live_;
    std::deque<Print>    tape_;

    Price         fair_;
    OrderId       next_id_     = 1;
    std::int64_t  clock_ms_    = 9LL * 3'600'000 + 30LL * 60'000;  // 09:30:00.000
    Price         last_price_  = 0;
    int           tick_dir_    = 0;
    Quantity      volume_      = 0;
    std::uint64_t trade_count_ = 0;
    std::uint64_t events_      = 0;
};

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

struct Options {
    const Instrument* inst      = &kBtcUsd;
    std::size_t       depth     = 10;
    long long         frames    = 0;  // 0 = run until interrupted
    int               fps       = 8;
    int               per_frame = 150;  // feed events between frames
    int               warmup    = 6'000;
    std::uint64_t     seed      = 42;
    bool              color     = true;
};

constexpr std::size_t kPriceW = 10, kSizeW = 9, kOrdW = 4, kBarW = 13;
constexpr std::size_t kLeftW  = kPriceW + 1 + kSizeW + 1 + kOrdW + 2 + kBarW;  // 40
constexpr std::size_t kTimeW  = 12;
constexpr std::size_t kRightW = kTimeW + 3 + kPriceW + 1 + kSizeW;            // 35
constexpr std::size_t kTotalW = 1 + kLeftW + 3 + kRightW;                      // 79, fits 80 cols

std::string render(const MarketFeed& feed, const Options& opt, long long frame) {
    const Instrument& in   = *opt.inst;
    const bool        c    = opt.color;
    const auto        snap = feed.book().snapshot(opt.depth);
    std::vector<Line> out;

    // Title bar
    {
        Line l(c);
        l.text(" NANOBOOK ", ansi::title).text("  ").text(in.symbol, ansi::bold).text("  Level-2 Order Book", ansi::dim);
        std::string right = clock_text(feed.clock_ms()) + "   ";
        right += opt.frames == 0 ? std::string("LIVE")
                                 : "frame " + std::to_string(frame) + "/" + std::to_string(opt.frames);
        l.pad_to(kTotalW - right.size()).text(right, opt.frames == 0 ? ansi::bgreen : ansi::dim);
        out.push_back(l);
    }
    const auto rule = [&] {
        Line l(c);
        l.glyph("─", kTotalW, ansi::dim);
        out.push_back(l);
    };
    rule();

    // Headline stats
    {
        Line l(c);
        l.text(" Last ", ansi::dim);
        if (feed.last_price() == 0) {
            l.glyph("—", 1);
        } else {
            const auto style = feed.tick_dir() > 0 ? ansi::bgreen : feed.tick_dir() < 0 ? ansi::bred : ansi::bold;
            l.text(fixed(feed.last_price(), in.price_dp), style).text(" ");
            if (feed.tick_dir() > 0) {
                l.glyph("▲", 1, ansi::bgreen);
            } else if (feed.tick_dir() < 0) {
                l.glyph("▼", 1, ansi::bred);
            }
        }
        if (snap.best_bid() && snap.best_ask()) {
            const Price sum    = *snap.best_bid() + *snap.best_ask();
            const Price spread = *snap.spread();
            // Mid is exact: shown with one extra decimal when it falls between ticks.
            const std::string mid = sum % 2 == 0 ? fixed(sum / 2, in.price_dp) : fixed(sum * 5, in.price_dp + 1);
            const Price       bps_x100 = spread * 2'000'000 / sum;
            l.pad_to(26).text("Mid ", ansi::dim).text(mid, ansi::bold);
            l.pad_to(48).text("Spread ", ansi::dim).text(fixed(spread, in.price_dp), ansi::yellow)
                .text(" (" + fixed(bps_x100, 2) + " bps)", ansi::dim);
        }
        out.push_back(l);
    }
    {
        Line l(c);
        l.text(" Volume ", ansi::dim).text(fixed(feed.volume(), in.qty_dp) + " " + std::string(in.unit));
        l.pad_to(34).text("Trades ", ansi::dim).text(group_thousands(std::to_string(feed.trade_count())));
        l.pad_to(52).text("Resting ", ansi::dim).text(group_thousands(std::to_string(feed.book().order_count())));
        l.pad_to(67).text("Lvls ", ansi::dim)
            .text(std::to_string(feed.book().bid_levels()) + "/" + std::to_string(feed.book().ask_levels()));
        out.push_back(l);
    }
    rule();

    // Left panel: asks (worst -> best), spread marker, bids (best -> worst)
    std::vector<Line> left;
    {
        Line h(c);
        h.right("PRICE", kPriceW, ansi::dim).pad(1).right("SIZE", kSizeW, ansi::dim).pad(1)
            .right("ORD", kOrdW, ansi::dim).pad(2).text("DEPTH", ansi::dim);
        left.push_back(h.pad_to(kLeftW));
    }

    Quantity max_qty = 1;
    for (const auto& lv : snap.bids) max_qty = std::max(max_qty, lv.qty);
    for (const auto& lv : snap.asks) max_qty = std::max(max_qty, lv.qty);

    const auto level_row = [&](const LevelView& lv, bool is_bid, bool is_best) {
        const auto price_style = is_bid ? (is_best ? ansi::bgreen : ansi::green) : (is_best ? ansi::bred : ansi::red);
        const auto bar_style   = is_bid ? ansi::green : ansi::red;
        const auto bar = static_cast<std::size_t>(std::max<Quantity>(
            1, (lv.qty * static_cast<Quantity>(kBarW) + max_qty / 2) / max_qty));
        Line l(c);
        l.right(fixed(lv.price, in.price_dp), kPriceW, price_style).pad(1)
            .right(fixed(lv.qty, in.qty_dp), kSizeW, is_best ? ansi::bold : std::string_view{}).pad(1)
            .right(std::to_string(lv.orders), kOrdW, ansi::dim).pad(2)
            .glyph("█", std::min(bar, kBarW), bar_style);
        return l.pad_to(kLeftW);
    };

    for (std::size_t i = snap.asks.size(); i < opt.depth; ++i) left.push_back(Line(c).pad(kLeftW));
    for (std::size_t i = snap.asks.size(); i-- > 0;) left.push_back(level_row(snap.asks[i], false, i == 0));
    {
        std::string label = " no market ";
        if (const auto spread = snap.spread()) label = " spread " + fixed(*spread, in.price_dp) + " ";
        const std::size_t dashes = kLeftW > label.size() ? kLeftW - label.size() : 0;
        Line l(c);
        l.glyph("┄", dashes / 2, ansi::dim).text(label, ansi::yellow).glyph("┄", dashes - dashes / 2, ansi::dim);
        left.push_back(l);
    }
    for (std::size_t i = 0; i < snap.bids.size(); ++i) left.push_back(level_row(snap.bids[i], true, i == 0));
    for (std::size_t i = snap.bids.size(); i < opt.depth; ++i) left.push_back(Line(c).pad(kLeftW));

    // Right panel: trade tape, newest first
    std::vector<Line> right;
    {
        Line h(c);
        h.text("TIME", ansi::dim).pad_to(kTimeW + 3).right("PRICE", kPriceW, ansi::dim).pad(1)
            .right("SIZE", kSizeW, ansi::dim);
        right.push_back(h);
    }
    for (const Print& p : feed.tape()) {
        if (right.size() >= left.size()) break;
        const bool buy   = p.side == Side::Buy;
        const auto style = buy ? ansi::green : ansi::red;
        Line       l(c);
        l.text(clock_text(p.time_ms), ansi::dim).pad(1).glyph(buy ? "▲" : "▼", 1, style).pad(1)
            .right(fixed(p.price, in.price_dp), kPriceW, style).pad(1).right(fixed(p.qty, in.qty_dp), kSizeW);
        right.push_back(l);
    }

    for (std::size_t i = 0; i < left.size(); ++i) {
        Line row(c);
        row.pad(1).append(left[i]).text(" ").glyph("│", 1, ansi::dim).text(" ");
        if (i < right.size()) row.append(right[i]);
        out.push_back(row);
    }
    rule();
    {
        Line l(c);
        l.text(" price-time FIFO  ·  int64 ticks  ·  ", ansi::dim)
            .text(group_thousands(std::to_string(feed.events())) + " events", ansi::cyan);
        if (opt.frames == 0) l.text("  ·  Ctrl+C to quit", ansi::dim);
        out.push_back(l);
    }

    std::string frame_text;
    for (const Line& l : out) {
        frame_text += l.str();
        if (c) frame_text += "\x1b[K";  // clear any leftovers from a wider previous frame
        frame_text += '\n';
    }
    return frame_text;
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

std::atomic<bool> g_stop{false};

extern "C" void on_interrupt(int) { g_stop.store(true); }

void enable_terminal() {
#if defined(_WIN32)
    // UTF-8 glyphs and ANSI escape processing for classic Windows consoles.
    SetConsoleOutputCP(CP_UTF8);
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  mode   = 0;
    if (GetConsoleMode(handle, &mode)) SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
}

void usage() {
    std::cout << "Usage: nanobook_demo [options]\n"
                 "  --frames N       render N frames then exit (0 = live until Ctrl+C, default)\n"
                 "  --snapshot       render a single static frame (same as --frames 1)\n"
                 "  --symbol S       BTC-USD (default) or NVDA\n"
                 "  --depth N        price levels per side, 1-25 (default 10)\n"
                 "  --fps N          redraw rate in live mode (default 8)\n"
                 "  --speed N        feed events between frames (default 150)\n"
                 "  --seed N         RNG seed for the synthetic feed (default 42)\n"
                 "  --no-color       plain text output, no ANSI escapes\n";
}

bool parse(int argc, char** argv, Options& opt) {
    const auto number = [&](int& i, long long lo, long long hi, long long& outv) {
        if (i + 1 >= argc) return false;
        char*           end = nullptr;
        const long long v   = std::strtoll(argv[++i], &end, 10);
        if (end == nullptr || *end != '\0' || v < lo || v > hi) return false;
        outv = v;
        return true;
    };

    if (std::getenv("NO_COLOR") != nullptr) opt.color = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        long long              v   = 0;
        if (arg == "--frames") {
            if (!number(i, 0, 1'000'000'000, v)) return false;
            opt.frames = v;
        } else if (arg == "--snapshot") {
            opt.frames = 1;
        } else if (arg == "--depth") {
            if (!number(i, 1, 25, v)) return false;
            opt.depth = static_cast<std::size_t>(v);
        } else if (arg == "--fps") {
            if (!number(i, 1, 120, v)) return false;
            opt.fps = static_cast<int>(v);
        } else if (arg == "--speed") {
            if (!number(i, 1, 1'000'000, v)) return false;
            opt.per_frame = static_cast<int>(v);
        } else if (arg == "--seed") {
            if (!number(i, 0, 1'000'000'000'000LL, v)) return false;
            opt.seed = static_cast<std::uint64_t>(v);
        } else if (arg == "--symbol") {
            if (i + 1 >= argc) return false;
            const std::string_view s = argv[++i];
            if (s == "BTC-USD" || s == "btc") {
                opt.inst = &kBtcUsd;
            } else if (s == "NVDA" || s == "nvda") {
                opt.inst = &kNvda;
            } else {
                return false;
            }
        } else if (arg == "--no-color") {
            opt.color = false;
        } else {
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse(argc, argv, opt)) {
        usage();
        return 2;
    }
    enable_terminal();

    MarketFeed feed(*opt.inst, opt.seed);
    for (int i = 0; i < opt.warmup; ++i) feed.step();

    // Static output: one frame, no cursor control.
    if (opt.frames == 1) {
        std::cout << render(feed, opt, 1) << std::flush;
        return 0;
    }

    std::signal(SIGINT, on_interrupt);
    const bool redraw = opt.color;  // plain mode prints frames one after another
    if (redraw) std::cout << "\x1b[?25l\x1b[2J";

    const auto period = std::chrono::milliseconds(1000 / opt.fps);
    for (long long frame = 1; (opt.frames == 0 || frame <= opt.frames) && !g_stop.load(); ++frame) {
        const auto start = std::chrono::steady_clock::now();
        if (frame > 1) {
            for (int i = 0; i < opt.per_frame; ++i) feed.step();
        }
        std::cout << (redraw ? "\x1b[H" : "") << render(feed, opt, frame) << (redraw ? "\x1b[J" : "\n")
                  << std::flush;
        if (opt.frames == 0 || frame < opt.frames) std::this_thread::sleep_until(start + period);
    }

    if (redraw) std::cout << "\x1b[0m\x1b[?25h" << std::flush;
    return 0;
}
