// NanoBook latency / throughput benchmark.
//
// 1. Generates a deterministic stream of synthetic orders (limit / market / IOC /
//    FOK / cancel / modify, with trader ids and self-trade prevention) by driving
//    a shadow book, so cancels and modifies target orders that are actually live
//    and prices track the evolving mid.
// 2. Single thread: replays the stream on a fresh book, timing every operation
//    with the CPU timestamp counter -> per-order matching latency; then replays it
//    again without per-op timers -> sustained throughput.
// 3. Two threads: a producer thread pushes the stream through a lock-free
//    SPSCQueue to a consumer thread that owns the OrderBook -> cross-thread
//    throughput (saturated) and end-to-end latency (paced, enqueue -> matched).
//
// Usage: nanobook_bench [order_count] [--single | --pipeline] [--rate N]

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "nanobook/order_book.hpp"
#include "nanobook/spsc_queue.hpp"

#if defined(_MSC_VER)
#include <intrin.h>
#define NB_HAVE_TSC 1
#elif defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#define NB_HAVE_TSC 1
#else
#define NB_HAVE_TSC 0
#endif

using namespace nanobook;

namespace {

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

inline std::uint64_t now_ticks() noexcept {
#if NB_HAVE_TSC
    _mm_lfence();
    const std::uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
#else
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
#endif
}

inline void cpu_relax() noexcept {
#if NB_HAVE_TSC
    _mm_pause();
#endif
}

// Nanoseconds per timer tick.
double calibrate_ticks() {
#if NB_HAVE_TSC
    using clock         = std::chrono::steady_clock;
    const auto          wall0 = clock::now();
    const std::uint64_t tsc0  = now_ticks();
    while (clock::now() - wall0 < std::chrono::milliseconds(250)) {
    }
    const std::uint64_t tsc1  = now_ticks();
    const auto          wall1 = clock::now();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(wall1 - wall0).count();
    return static_cast<double>(ns) / static_cast<double>(tsc1 - tsc0);
#else
    return 1.0;
#endif
}

// ---------------------------------------------------------------------------
// Workload
// ---------------------------------------------------------------------------

enum class OpKind : std::uint8_t { Limit, Market, IOC, FOK, Cancel, Modify };
constexpr std::size_t kOpKinds = 6;
constexpr std::array<std::string_view, kOpKinds> kOpNames{"Limit", "Market", "IOC", "FOK", "Cancel", "Modify"};

struct Op {
    OpKind   kind   = OpKind::Limit;
    Side     side   = Side::Buy;
    STPMode  stp    = STPMode::None;
    TraderId trader = kNoTrader;
    OrderId  id     = 0;
    Price    price  = 0;
    Quantity qty    = 0;
};

struct Mix {
    int limit  = 55;  // percentages, must sum to 100
    int cancel = 30;
    int market = 7;
    int ioc    = 3;
    int fok    = 1;
    int modify = 4;
};

inline std::uint64_t apply(OrderBook& book, const Op& op, std::vector<Trade>& trades) {
    trades.clear();
    const OrderOptions opts{op.trader, op.stp};
    switch (op.kind) {
        case OpKind::Limit:  return static_cast<std::uint64_t>(book.add_limit(op.id, op.side, op.price, op.qty, trades, opts).filled);
        case OpKind::Market: return static_cast<std::uint64_t>(book.add_market(op.id, op.side, op.qty, trades, opts).filled);
        case OpKind::IOC:    return static_cast<std::uint64_t>(book.add_ioc(op.id, op.side, op.price, op.qty, trades, opts).filled);
        case OpKind::FOK:    return static_cast<std::uint64_t>(book.add_fok(op.id, op.side, op.price, op.qty, trades, opts).filled);
        case OpKind::Cancel: return book.cancel(op.id) ? 1u : 0u;
        case OpKind::Modify: return static_cast<std::uint64_t>(book.modify(op.id, op.price, op.qty, trades).resting);
    }
    return 0;
}

std::vector<Op> generate(std::size_t count, std::uint64_t seed, const Mix& mix) {
    std::mt19937_64                         rng(seed);
    std::uniform_int_distribution<int>      pct(0, 99);
    std::uniform_int_distribution<int>      coin(0, 1);
    std::geometric_distribution<Price>      depth(0.18);  // passive distance from touch, mean ~4.5 ticks
    std::uniform_int_distribution<Price>    aggress(0, 3);
    std::uniform_int_distribution<Quantity> limit_qty(1, 500);
    std::uniform_int_distribution<Quantity> taker_qty(1, 300);
    std::uniform_int_distribution<Price>    nudge(-2, 2);
    std::uniform_int_distribution<TraderId> trader(1, 500);

    // ~70% of flow carries STP: mostly CancelNewest, some CancelOldest.
    auto stp_mode = [&]() {
        const int r = pct(rng);
        return r < 55 ? STPMode::CancelNewest : r < 70 ? STPMode::CancelOldest : STPMode::None;
    };

    OrderBook          shadow(count);
    std::vector<Trade> trades;
    trades.reserve(4096);
    std::vector<OrderId> live;  // candidate ids for cancel / modify (lazily pruned)
    live.reserve(count);

    std::vector<Op> ops;
    ops.reserve(count);

    Price   mid     = 1'000'000;  // 100.0000 with a 0.0001 tick
    OrderId next_id = 1;

    auto pick_live = [&]() -> OrderId {
        while (!live.empty()) {
            std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
            const std::size_t                          i  = pick(rng);
            const OrderId                              id = live[i];
            if (shadow.contains(id)) return id;
            live[i] = live.back();
            live.pop_back();
        }
        return 0;
    };

    const int cut_limit  = mix.limit;
    const int cut_cancel = cut_limit + mix.cancel;
    const int cut_market = cut_cancel + mix.market;
    const int cut_ioc    = cut_market + mix.ioc;
    const int cut_fok    = cut_ioc + mix.fok;

    while (ops.size() < count) {
        const int  roll = pct(rng);
        const Side side = coin(rng) == 0 ? Side::Buy : Side::Sell;
        Op         op{};
        op.side   = side;
        op.trader = trader(rng);
        op.stp    = stp_mode();

        if (roll >= cut_limit && roll < cut_cancel) {
            const OrderId id = pick_live();
            if (id == 0) continue;
            op.kind = OpKind::Cancel;
            op.id   = id;
        } else if (roll >= cut_cancel && roll < cut_market) {
            op.kind = OpKind::Market;
            op.id   = next_id++;
            op.qty  = taker_qty(rng);
        } else if (roll >= cut_market && roll < cut_fok) {
            op.kind  = roll < cut_ioc ? OpKind::IOC : OpKind::FOK;
            op.id    = next_id++;
            op.price = side == Side::Buy ? mid + aggress(rng) : mid - aggress(rng);
            op.qty   = taker_qty(rng);
        } else if (roll >= cut_fok) {
            const OrderId id = pick_live();
            if (id == 0) continue;
            const auto view = shadow.find(id);
            op.kind  = OpKind::Modify;
            op.side  = view->side;
            op.id    = id;
            op.price = std::max<Price>(1, view->price + nudge(rng));
            op.qty   = limit_qty(rng);
        } else {
            // Limit: ~90% passive around the touch, ~10% marketable.
            const bool  marketable = pct(rng) < 10;
            const Price offset     = marketable ? -aggress(rng) : 1 + depth(rng);
            op.kind  = OpKind::Limit;
            op.id    = next_id++;
            op.price = side == Side::Buy ? mid - offset : mid + offset;
            op.qty   = limit_qty(rng);
        }

        apply(shadow, op, trades);
        if (op.kind == OpKind::Limit && shadow.contains(op.id)) live.push_back(op.id);
        ops.push_back(op);

        // Track the book's mid; drift randomly when one side is empty.
        const auto bid = shadow.best_bid();
        const auto ask = shadow.best_ask();
        if (bid && ask) {
            mid = (*bid + *ask) / 2;
        } else {
            mid += nudge(rng);
        }
    }
    return ops;
}

// ---------------------------------------------------------------------------
// Reporting helpers
// ---------------------------------------------------------------------------

double percentile(const std::vector<std::uint64_t>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    const auto rank = static_cast<std::size_t>(p / 100.0 * static_cast<double>(sorted.size()));
    return static_cast<double>(sorted[std::min(rank, sorted.size() - 1)]);
}

std::string fmt_int(std::uint64_t v) {
    std::string s = std::to_string(v);
    for (auto i = static_cast<std::ptrdiff_t>(s.size()) - 3; i > 0; i -= 3) {
        s.insert(static_cast<std::size_t>(i), ",");
    }
    return s;
}

struct Stats {
    double p50, p95, p99, p999, max, mean;
};

Stats summarize(std::vector<std::uint64_t>& samples, double ns_per_tick) {
    std::sort(samples.begin(), samples.end());
    double sum = 0;
    for (const auto s : samples) sum += static_cast<double>(s);
    const double n = samples.empty() ? 1.0 : static_cast<double>(samples.size());
    return Stats{percentile(samples, 50.0) * ns_per_tick,  percentile(samples, 95.0) * ns_per_tick,
                 percentile(samples, 99.0) * ns_per_tick,  percentile(samples, 99.9) * ns_per_tick,
                 (samples.empty() ? 0.0 : static_cast<double>(samples.back())) * ns_per_tick,
                 sum / n * ns_per_tick};
}

void print_header(std::string_view label) {
    std::cout << "  " << std::left << std::setw(10) << label << std::right << std::setw(9) << "p50"
              << std::setw(9) << "p95" << std::setw(9) << "p99" << std::setw(10) << "p99.9"
              << std::setw(10) << "mean" << std::setw(11) << "max" << '\n';
}

void print_row(std::string_view name, const Stats& s) {
    std::cout << "  " << std::left << std::setw(10) << name << std::right << std::fixed
              << std::setprecision(0) << std::setw(9) << s.p50 << std::setw(9) << s.p95
              << std::setw(9) << s.p99 << std::setw(10) << s.p999 << std::setw(10) << s.mean
              << std::setw(11) << s.max << '\n';
}

// ---------------------------------------------------------------------------
// Single-threaded benchmark
// ---------------------------------------------------------------------------

void run_single(const std::vector<Op>& ops, double ns_per_tick, std::uint64_t& sink) {
    std::vector<Trade> trades;
    trades.reserve(4096);

    std::array<std::size_t, kOpKinds> kind_counts{};
    for (const auto& op : ops) ++kind_counts[static_cast<std::size_t>(op.kind)];

    // Timer overhead (empty timed region).
    std::vector<std::uint64_t> overhead(100'000);
    for (auto& o : overhead) {
        const auto t0 = now_ticks();
        const auto t1 = now_ticks();
        o             = t1 - t0;
    }
    std::sort(overhead.begin(), overhead.end());
    const double timer_overhead_ns = percentile(overhead, 50.0) * ns_per_tick;

    // ---- Latency pass ------------------------------------------------------
    std::vector<std::uint64_t>                       all(ops.size());
    std::array<std::vector<std::uint64_t>, kOpKinds> per_kind;
    for (std::size_t k = 0; k < kOpKinds; ++k) per_kind[k].reserve(kind_counts[k]);

    std::size_t trade_count  = 0;
    std::size_t stp_cancels  = 0;
    std::size_t peak_orders  = 0;
    std::size_t final_orders = 0;
    std::size_t final_levels = 0;
    {
        OrderBook book(ops.size());
        for (std::size_t i = 0; i < ops.size(); ++i) {
            const auto t0 = now_ticks();
            sink += apply(book, ops[i], trades);
            const auto t1 = now_ticks();
            all[i]        = t1 - t0;
            per_kind[static_cast<std::size_t>(ops[i].kind)].push_back(t1 - t0);
            trade_count += trades.size();
            stp_cancels += book.stp_cancelled().size();
            if ((i & 1023u) == 0) peak_orders = std::max(peak_orders, book.order_count());
        }
        final_orders = book.order_count();
        final_levels = book.bid_levels() + book.ask_levels();
    }

    // ---- Throughput pass ---------------------------------------------------
    double elapsed_s = 0;
    {
        OrderBook  book(ops.size());
        const auto start = std::chrono::steady_clock::now();
        for (const auto& op : ops) sink += apply(book, op, trades);
        const auto stop = std::chrono::steady_clock::now();
        elapsed_s       = std::chrono::duration<double>(stop - start).count();
    }

    std::cout << "\nWorkload\n--------\n";
    for (std::size_t k = 0; k < kOpKinds; ++k) {
        std::cout << "  " << std::left << std::setw(8) << kOpNames[k] << std::right
                  << std::setw(10) << fmt_int(kind_counts[k]) << "  (" << std::fixed
                  << std::setprecision(1)
                  << 100.0 * static_cast<double>(kind_counts[k]) / static_cast<double>(ops.size())
                  << "%)\n";
    }
    std::cout << "  Trades generated : " << fmt_int(trade_count) << '\n'
              << "  STP cancels      : " << fmt_int(stp_cancels) << " resting orders\n"
              << "  Peak resting     : " << fmt_int(peak_orders) << " orders\n"
              << "  Final book       : " << fmt_int(final_orders) << " orders across "
              << fmt_int(final_levels) << " price levels\n";

    const double throughput = static_cast<double>(ops.size()) / elapsed_s;
    std::cout << "\n[1] Single thread: throughput (untimed replay)\n"
                 "-----------------------------------------------\n"
              << "  " << fmt_int(ops.size()) << " orders in " << std::setprecision(2)
              << elapsed_s * 1e3 << " ms  ->  " << fmt_int(static_cast<std::uint64_t>(throughput))
              << " orders/sec\n";

    std::cout << "\n[1] Single thread: latency per order (ns, includes ~" << std::setprecision(1)
              << timer_overhead_ns << " ns timer overhead)\n"
              << "--------------------------------------------------------------------\n";
    print_header("Type");
    print_row("ALL", summarize(all, ns_per_tick));
    for (std::size_t k = 0; k < kOpKinds; ++k) {
        if (!per_kind[k].empty()) print_row(kOpNames[k], summarize(per_kind[k], ns_per_tick));
    }
}

// ---------------------------------------------------------------------------
// Two-thread SPSC pipeline benchmark
// ---------------------------------------------------------------------------

struct Message {
    Op            op{};
    std::uint64_t stamp = 0;  // producer timestamp (TSC ticks)
};

struct PipelineRun {
    std::uint64_t              elapsed_ticks = 0;
    std::vector<std::uint64_t> latency;  // enqueue -> matched, in ticks
    std::size_t                trades    = 0;
};

constexpr std::size_t kQueueCapacity = 1u << 16;

// interval_ticks == 0: producer pushes as fast as the queue accepts (saturated).
// interval_ticks  > 0: producer emits on a fixed schedule; each message is
//                      stamped with its *scheduled* send time so queueing delay
//                      caused by consumer stalls is measured, not hidden
//                      (avoids coordinated omission).
PipelineRun run_pipeline(const std::vector<Op>& ops, std::uint64_t interval_ticks,
                         std::uint64_t& sink) {
    SPSCQueue<Message> queue(kQueueCapacity);
    PipelineRun        run;
    const bool         measure = interval_ticks != 0;  // per-message stamps only when paced
    if (measure) run.latency.resize(ops.size());

    std::atomic<bool> consumer_ready{false};
    std::uint64_t     end_tick       = 0;
    std::uint64_t     consumer_sink  = 0;

    std::thread consumer([&] {
        OrderBook          book(ops.size());
        std::vector<Trade> trades;
        trades.reserve(4096);
        Message msg;
        consumer_ready.store(true, std::memory_order_release);

        // Dedicated matching thread: hot busy-poll (no pause) for the lowest wake-up latency.
        for (std::size_t i = 0; i < ops.size();) {
            if (!queue.try_pop(msg)) continue;
            consumer_sink += apply(book, msg.op, trades);
            if (measure) run.latency[i] = now_ticks() - msg.stamp;
            run.trades += trades.size();
            ++i;
        }
        end_tick = now_ticks();
    });

    while (!consumer_ready.load(std::memory_order_acquire)) cpu_relax();

    const std::uint64_t start_tick = now_ticks();
    std::uint64_t       scheduled  = start_tick;
    for (const Op& op : ops) {
        Message msg{op, 0};
        if (measure) {
            while (now_ticks() < scheduled) cpu_relax();
            msg.stamp = scheduled;
            scheduled += interval_ticks;
        }
        // Back off when the ring is full: re-reading the consumer's index on every
        // spin would bounce its cache line between cores and slow the consumer.
        while (!queue.try_push(msg)) {
            for (int k = 0; k < 32; ++k) cpu_relax();
        }
    }
    consumer.join();

    sink += consumer_sink;
    run.elapsed_ticks = end_tick - start_tick;
    return run;
}

void run_two_thread(const std::vector<Op>& ops, double ns_per_tick, const std::vector<double>& rates,
                    std::uint64_t& sink) {
    std::cout << "\n[2] Two threads: producer -> SPSCQueue<" << fmt_int(kQueueCapacity)
              << "> -> matching engine\n"
                 "-----------------------------------------------------------------\n";

    // Warm-up run (thread start-up, page faults in the queue and book).
    run_pipeline(ops, 0, sink);

    PipelineRun  sat     = run_pipeline(ops, 0, sink);
    const double sat_sec = static_cast<double>(sat.elapsed_ticks) * ns_per_tick * 1e-9;
    std::cout << "  Saturated: " << fmt_int(ops.size()) << " orders in " << std::fixed
              << std::setprecision(2) << sat_sec * 1e3 << " ms  ->  "
              << fmt_int(static_cast<std::uint64_t>(static_cast<double>(ops.size()) / sat_sec))
              << " orders/sec across threads (" << fmt_int(sat.trades) << " trades)\n\n";

    // Latency is only meaningful below saturation: the producer emits on a fixed
    // schedule and each order is stamped with its scheduled send time.
    std::cout << "  End-to-end latency, scheduled enqueue -> matched (ns, paced producer):\n";
    print_header("Rate/sec");
    for (const double rate : rates) {
        const auto  interval = static_cast<std::uint64_t>(1e9 / rate / ns_per_tick + 0.5);
        PipelineRun paced    = run_pipeline(ops, interval, sink);
        print_row(fmt_int(static_cast<std::uint64_t>(rate)), summarize(paced.latency, ns_per_tick));
    }
    std::cout << "  (includes ~13 ns timer overhead; tails reflect OS scheduling of unpinned threads)\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t order_count   = 1'000'000;
    bool        want_single   = true;
    bool        want_pipeline = true;
    std::vector<double> rates{500'000.0, 1'000'000.0, 2'000'000.0};

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--single") {
            want_pipeline = false;
        } else if (arg == "--pipeline") {
            want_single = false;
        } else if (arg == "--rate" && i + 1 < argc) {
            rates = {std::max(1'000.0, std::strtod(argv[++i], nullptr))};
        } else if (!arg.empty() && arg.front() != '-') {
            order_count = static_cast<std::size_t>(std::strtoull(argv[i], nullptr, 10));
        } else {
            std::cerr << "usage: nanobook_bench [order_count] [--single | --pipeline] [--rate N]\n";
            return 2;
        }
    }
    order_count = std::max<std::size_t>(order_count, 1'000);

    constexpr std::uint64_t kSeed = 0x5EED'B00C;
    const Mix               mix{};

    std::cout << "NanoBook benchmark\n==================\n";
    std::cout << "Generating " << fmt_int(order_count) << " synthetic orders (seed 0x" << std::hex
              << kSeed << std::dec << ")...\n";
    const std::vector<Op> ops = generate(order_count, kSeed, mix);

    const double ns_per_tick = calibrate_ticks();
#if NB_HAVE_TSC
    std::cout << "Timer: RDTSC, " << std::fixed << std::setprecision(3) << 1.0 / ns_per_tick
              << " GHz   Hardware threads: " << std::thread::hardware_concurrency() << '\n';
#else
    std::cout << "Timer: std::chrono::steady_clock\n";
#endif

    std::uint64_t sink = 0;

    // Warm-up: caches, branch predictors, allocator.
    {
        std::vector<Trade> trades;
        trades.reserve(4096);
        OrderBook warm(order_count);
        for (const auto& op : ops) sink += apply(warm, op, trades);
    }

    if (want_single) run_single(ops, ns_per_tick, sink);
    if (want_pipeline) run_two_thread(ops, ns_per_tick, rates, sink);

    std::cout << "\n(checksum " << sink << ")\n";
    return 0;
}
