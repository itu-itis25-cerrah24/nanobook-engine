# NanoBook

[![CI](https://github.com/itu-itis25-cerrah24/nanobook-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/itu-itis25-cerrah24/nanobook-engine/actions/workflows/ci.yml)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)
![Dependencies](https://img.shields.io/badge/dependencies-none-brightgreen)
![Tests](https://img.shields.io/badge/tests-63%20passing-brightgreen)

**NanoBook** is a low-latency **Limit Order Book & Matching Engine** written in modern C++20.
It matches orders with strict **price-time (FIFO) priority**, uses **fixed-point integer prices**
with no floating point on the matching path, and processes **~14.8 million orders per second** with a
**median latency of ~87 ns** on a single core. A **lock-free SPSC ring buffer** feeds the engine from a
separate thread at **~12.6 million orders per second**. It uses only the standard library.

- **Order types:** `LIMIT`, `MARKET`, `IOC` (Immediate-or-Cancel), `FOK` (Fill-or-Kill), plus `CANCEL` and `MODIFY`
- **Self-Trade Prevention:** per-order `trader_id` with `CancelNewest` / `CancelOldest` modes
- **Concurrency:** cache-line-aligned, wait-free `SPSCQueue` built on acquire/release atomics
- **Market data:** Level-2 depth snapshots (top *N* levels), best bid/ask, spread
- **Execution reports:** structured `Trade` events with sequence numbers, taker/maker ids and aggressor side
- **Tooling:** 63 unit tests (including a 2-thread stress test, and ThreadSanitizer runs in CI), a 1M-order
  single- and multi-threaded benchmark, and a live ANSI terminal visualizer

---

## Benchmark Results

1,000,000 synthetic orders. The order stream comes from a fixed seed, so every run processes identical
input: 55% limit, 30% cancel, 7% market, 3% IOC, 1% FOK and 4% modify, sent by 500 traders. About 70% of
orders use self-trade prevention. The run produces 255,924 trades, and up to ~95k orders rest on the book at once.

### Single thread: matching engine only

| Metric | Result |
|---|---|
| **Throughput** | **14,774,584 orders/sec** (1M orders in 67.7 ms) |
| **p50 latency** | **87 ns** |
| **p95 latency** | **174 ns** |
| **p99 latency** | **254 ns** |
| **p99.9 latency** | **398 ns** |
| Mean latency | 98 ns |

Per operation type (ns):

| Type | p50 | p95 | p99 | p99.9 |
|---|---:|---:|---:|---:|
| Limit  |  81 | 140 | 196 | 358 |
| Market |  53 | 173 | 247 | 382 |
| IOC    |  50 | 169 | 244 | 380 |
| FOK    |  53 | 179 | 262 | 429 |
| Cancel | 101 | 189 | 272 | 403 |
| Modify | 155 | 264 | 348 | 497 |

### Two threads: producer → `SPSCQueue` → matching engine

A producer thread pushes the same 1M orders through a 65,536-slot lock-free ring to a consumer thread
that owns the `OrderBook`. End-to-end latency is measured from an order's scheduled send time to the end
of its matching. The producer emits on a fixed schedule, and each order is stamped with the time it was
*scheduled* to be sent, so delays caused by consumer stalls are counted, not hidden
(this avoids coordinated omission).

| Metric | Result |
|---|---|
| **Cross-thread throughput (saturated)** | **12,562,455 orders/sec** (1M orders in 79.6 ms) |

| Paced rate | p50 | p95 | p99 | p99.9 |
|---|---:|---:|---:|---:|
| 500,000 orders/sec   | 231 ns | 501 ns |  8.3 µs |  94 µs |
| 1,000,000 orders/sec | 220 ns | 418 ns | 18.4 µs | 345 µs |
| 2,000,000 orders/sec | 231 ns | 427 ns | 26.3 µs | 425 µs |

The ~130 ns gap between the single-thread median and the pipeline median is the cost of handing a
message from one core to another: the queue counter and the message slot both have to cross the cache
hierarchy. The p99 and p99.9 tails look about the same at every rate, so load isn't causing them. They
come from OS scheduling of unpinned threads on a hybrid P-core/E-core CPU, and vary from run to run
(p99 has measured 8–94 µs). A production deployment would pin both threads to isolated cores, which
portable standard C++ cannot do.

<sub>Intel Core i7-13700HX · Windows 11 · g++ 15.2 (MSYS2 UCRT64) · `-std=c++20 -O3 -march=native`.
Timestamps come from a serialized `RDTSC`, calibrated against `steady_clock`, and include ~13 ns of timer
overhead. Single-thread throughput is measured in a separate replay with no per-order timers.
Results vary by hardware. Across repeated runs, single-thread throughput was 14.6–15.1M/s with p50 83–89 ns,
pipeline throughput 12.6–13.0M/s, and pipeline p50 211–252 ns.</sub>

Reproduce with `.\build.ps1 bench` or `make bench`. Options: `nanobook_bench [count] [--single | --pipeline] [--rate N]`.

---

## Time Complexity

`L` = number of active price levels on one side (typically tens), `F` = number of fills an order generates,
`K` = levels an aggressive order crosses.

| Operation | Complexity | How |
|---|---|---|
| Best bid / best ask / spread | **O(1)** | `begin()` of a per-side ordered map |
| Lookup order by id | **O(1)** avg | `std::unordered_map<OrderId, NodeIndex>` |
| Cancel | **O(1)** | hash lookup + unlink from intrusive FIFO list (+ O(log L) only if the level becomes empty) |
| Modify: reduce qty, same price | **O(1)** | in-place update, keeps queue priority |
| Modify: reprice / increase qty | O(log L) | cancel/replace, loses priority, may trade |
| Add resting limit | O(log L) | find-or-create level, O(1) append to tail |
| Match (per fill) | **O(1)** | pop head of best level's FIFO queue |
| Market / IOC / marketable limit | O(F + K) | sweep from best price outward |
| FOK liquidity check | O(K) | read-only scan using each level's running `total_qty`, then the normal sweep |
| FOK check with STP | O(orders in K levels) | must skip or stop at the trader's own orders |
| Self-trade check | **O(1)** per maker | one `trader_id` compare inside the match loop |
| Level-2 snapshot (depth N) | O(N) | walk the first N levels of each side |
| Node allocation / release | **O(1)** | free-list slab pool, no `new`/`delete` in steady state |
| `SPSCQueue::try_push` / `try_pop` | **O(1)**, wait-free | one relaxed load, one store-release, and an acquire load only when the cached index says full/empty |

---

## Architecture

```
  Producer thread                                                      Matching thread
  (gateway / feed)                                                     (owns the book)
 ┌───────────────┐    SPSCQueue<Message>  (lock-free ring, 2^N slots)   ┌──────────────────┐
 │ try_push(msg) │──▶ ┌────┬────┬────┬────┬────┬────┬────┬────┐ ──────▶│ try_pop(msg)     │
 └───────────────┘    │    │ m4 │ m5 │ m6 │ m7 │    │    │    │        │ book.submit(...) │
                      └────┴────┴────┴────┴────┴────┴────┴────┘        └────────┬─────────┘
   tail_  (producer cache line, store-release)  ▲    head_ (consumer cache line, store-release)
                                                                                 │
                         ┌───────────────────────────────────────────────────────▼──────┐
  OrderRequest ────────▶ │                          OrderBook                           │ ──▶ Trade events
  (Limit/Market/IOC/FOK, │  Order Index  std::unordered_map                             │     (seq, taker, maker,
   Cancel, Modify,       │  ┌──────────────────────────┐                                │      side, price, qty)
   trader_id, STP mode)  │  │ OrderId ──▶ NodeIndex    │──┐  O(1) lookup                │
                         │  └──────────────────────────┘  │                             │ ──▶ Level-2 snapshot
                         └────────────────────────────────┼─────────────────────────────┘     (top-N, BBO, spread)
                                                          │
   Price Levels (per side)                                ▼
   std::map<Price, PriceLevel>             Memory Pool  (OrderPool: contiguous std::vector<OrderNode>)
                                           ┌────┬────┬────┬────┬────┬────┬────┬────┐
   ASKS  (ascending, best = begin())       │ #0 │ #1 │ #2 │ #3 │ #4 │ #5 │ #6 │ .. │   free list: 5 ─▶ 2 ─▶ ∅
    10,012 │ qty 900 │ head ─▶ #6 ─▶ #3    └────┴────┴────┴────┴────┴────┴────┴────┘
    10,011 │ qty 250 │ head ─▶ #0             ▲ nodes linked by index (prev/next), not pointers
   ───────────── spread ─────────────         │
    10,009 │ qty 400 │ head ─▶ #1 ─▶ #4 ─▶ #7 │   FIFO: oldest at head, new orders at tail
    10,008 │ qty 120 │ head ─▶ #8             │
   BIDS  (descending, best = begin())
```

- **Fixed-point prices.** `Price` is an `int64_t` count of ticks. For example, with a $0.01 tick `$67,432.50` is
  `6743250`. Price comparisons are exact integer comparisons, so rounding errors cannot occur.
- **Memory pool.** `OrderNode`s (48 bytes, checked at compile time) live in one contiguous slab. Freed
  slots go on an index-linked free list and are reused before the slab grows, so a busy book does no heap
  allocation.
- **Intrusive FIFO queues.** Each `PriceLevel` stores `head`/`tail` indices and a running `total_qty`.
  Each order stores `prev`/`next` indices and a pointer back to its level. Unlinking from anywhere in the
  queue is O(1), and both Level-2 quantities and FOK liquidity checks read the running totals instead of
  re-summing.
- **Order index.** A hash map from `OrderId` to pool slot gives O(1) cancel and modify without scanning levels.
- **Allocation-free trade reporting.** Trades are appended to a caller-owned `std::vector<Trade>`, so a hot
  loop reuses one buffer.

### Lock-free SPSC queue (`include/nanobook/spsc_queue.hpp`)

- **One writer per counter.** `head_` is written only by the consumer and `tail_` only by the producer.
  Each counter is `alignas(64)` on its own cache line, so the two threads never contend on a line (no false
  sharing). Capacity, mask and the buffer pointer sit on a third, read-only line.
- **Acquire/release handoff.** The producer writes the slot, then publishes it with
  `tail_.store(release)`. The consumer's `tail_.load(acquire)` guarantees it sees the slot contents.
  Slots are handed back to the producer the same way through `head_`.
- **Cached remote index.** Each side keeps a private copy of the other side's counter and reloads the
  shared atomic only when the queue looks full or empty. While both sides are moving, most operations
  never touch the other core's cache line.
- **Wait-free.** `try_push` and `try_pop` finish in a bounded number of steps: no locks, CAS loops or
  retries. Counters are 64-bit and increase forever, so slots are indexed with `counter & mask` and full
  and empty are never ambiguous.
- **Extras.** `front()` + `pop()` read in place without a copy, `try_emplace` constructs in place, and
  move-only payload types are supported.

### Matching rules

| Order | Behaviour |
|---|---|
| `LIMIT`  | Trades against the opposite side while prices cross. The remainder rests at its limit price. |
| `MARKET` | Trades at any price until filled or the book is empty. The remainder is **cancelled** (`NoLiquidity`). |
| `IOC`    | Trades up to its limit price. The remainder is **cancelled** and never rests. |
| `FOK`    | Checks liquidity up to its limit price first, without touching the book. If the full quantity is available it executes completely; otherwise it is **killed** (`FOKUnfillable`) with zero trades, no sequence numbers used and no change to the book. |
| `CANCEL` | Removes a resting order. Returns `false` for unknown or already-filled ids. |
| `MODIFY` | Reducing the quantity at the same price keeps queue priority. Repricing or increasing the quantity is a cancel/replace: priority is lost, the order keeps its trader and STP mode, and it can trade at once if it now crosses. |

All trades print at the **resting (maker) order's price**. Aggressive orders sweep from the best price
outward, so the book can never be left crossed. Requests with a non-positive quantity or price, or with
a duplicate id, are rejected and leave the book unchanged.

### Self-Trade Prevention

Each order can carry a `trader_id` (`uint32_t`) and an `STPMode`. A `trader_id` of `0`, the default, means
the order is anonymous and never checked, so existing callers behave exactly as before. When an incoming
order would match a resting order with the same non-zero `trader_id`, the **incoming order's** mode decides:

| `STPMode` | Effect |
|---|---|
| `None`         | Self-matching allowed. |
| `CancelNewest` | Matching stops and the incoming order's remainder is cancelled (`CancelReason::SelfTrade`). Fills before that point stand, and the remainder never rests. The resting order is untouched. |
| `CancelOldest` | The resting order is cancelled and matching continues with the next order in the queue. The cancelled ids are reported through `ExecResult::stp_cancels` and `OrderBook::stp_cancelled()`. |

FOK and STP work together: the FOK pre-check counts only liquidity the order could actually trade
against. With `CancelNewest` it stops at the trader's first own order, and with `CancelOldest` it leaves the
trader's own quantity out. A killed FOK never cancels resting orders.

---

## Live Terminal Visualizer

`nanobook_demo` feeds a synthetic market into the engine and renders the book in the terminal:
green bids, red asks, depth bars, last price with tick direction, mid, spread in bps, and a scrolling
tape of executed trades.

```
 NANOBOOK   BTC-USD  Level-2 Order Book                09:30:18.102   frame 1/1
───────────────────────────────────────────────────────────────────────────────
 Last 67,442.50 ▼         Mid 67,442.75         Spread 0.50 (0.07 bps)
 Volume 172.1984 BTC              Trades 2,099      Resting 966    Lvls 30/23
───────────────────────────────────────────────────────────────────────────────
      PRICE      SIZE  ORD  DEPTH         │ TIME                PRICE      SIZE
  67,447.50    5.8192   25  █████         │ 09:30:18.086 ▼  67,442.50    0.1654
  67,447.00    4.2577   24  ███           │ 09:30:18.074 ▼  67,442.50    0.2967
  67,446.50    7.7390   42  ██████        │ 09:30:18.065 ▲  67,443.00    0.0479
  67,446.00    8.0027   40  ██████        │ 09:30:18.001 ▼  67,442.50    0.1351
  67,445.50    9.5689   53  ███████       │ 09:30:17.994 ▼  67,442.50    0.0057
  67,445.00   12.7399   70  ██████████    │ 09:30:17.979 ▲  67,443.00    0.0948
  67,444.50   16.7127   72  █████████████ │ 09:30:17.951 ▼  67,442.50    0.1272
  67,444.00    7.3082   36  ██████        │ 09:30:17.924 ▲  67,444.00    0.2656
  67,443.50    0.6119    3  █             │ 09:30:17.924 ▲  67,443.50    0.6888
  67,443.00    0.5772    4  █             │ 09:30:17.917 ▲  67,443.50    0.0363
 ┄┄┄┄┄┄┄┄┄┄┄┄┄ spread 0.50 ┄┄┄┄┄┄┄┄┄┄┄┄┄┄ │ 09:30:17.911 ▼  67,442.50    0.0533
  67,442.50    9.9190   38  ████████      │ 09:30:17.882 ▼  67,442.50    0.0181
  67,442.00    7.2456   40  ██████        │ 09:30:17.854 ▲  67,443.50    0.0942
  67,441.50    4.9792   34  ████          │ 09:30:17.854 ▼  67,442.50    0.0975
  67,441.00    8.3815   42  ███████       │ 09:30:17.848 ▼  67,442.50    0.0696
  67,440.50    7.5236   40  ██████        │ 09:30:17.842 ▲  67,443.50    0.0142
  67,440.00    5.8870   32  █████         │ 09:30:17.838 ▲  67,443.50    0.1092
  67,439.50    4.8270   23  ████          │ 09:30:17.835 ▲  67,443.50    0.1195
  67,439.00    3.0646   18  ██            │ 09:30:17.835 ▲  67,443.00    0.4833
  67,438.50    3.2264   16  ███           │ 09:30:17.733 ▼  67,442.50    0.1796
  67,438.00    2.0698   14  ██            │ 09:30:17.732 ▲  67,443.50    0.1361
───────────────────────────────────────────────────────────────────────────────
 price-time FIFO  ·  int64 ticks  ·  6,000 events
```

| Flag | Description |
|---|---|
| `--frames N` | Render N frames and exit. `0` (default) runs live until Ctrl+C |
| `--snapshot` | One static frame, same as `--frames 1`. Good for screenshots and CI |
| `--symbol S` | `BTC-USD` (default) or `NVDA` |
| `--depth N`  | Price levels per side, 1–25 (default 10) |
| `--fps N` / `--speed N` | Redraw rate / feed events between frames |
| `--seed N`   | Seed for the synthetic feed. Output is deterministic for a given seed |
| `--no-color` | Plain text with no ANSI escapes. Also honours the `NO_COLOR` environment variable |

---

## Quick Start

Requirements: a C++20 compiler (**g++ 11+**, tested with g++ 13 on Ubuntu and g++ 15.2 on Windows).
No third-party libraries are needed.

### Windows (PowerShell)

```powershell
.\build.ps1              # build all, run tests, render a demo frame, run both benchmarks
.\build.ps1 test         # build + unit tests
.\build.ps1 bench        # build + single-thread and 2-thread SPSC benchmarks
.\build.ps1 demo         # build + live visualizer (Ctrl+C to quit)
.\build\nanobook_demo.exe --snapshot --symbol NVDA
.\build\nanobook_bench.exe --pipeline --rate 1000000
```

### Linux / macOS

```bash
./build.sh               # same targets as build.ps1: all | test | bench | demo | build
make -j                  # or use the Makefile
make test
make bench
make snapshot            # single visualizer frame
make demo                # live visualizer
```

### CMake

```bash
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake -j
ctest --test-dir build-cmake --output-on-failure
```

Every build path compiles with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
-Wold-style-cast -Werror`. CI also builds and runs the test suite under **ThreadSanitizer** and
**AddressSanitizer + UBSan**.

---

## Using the Engine

```cpp
#include "nanobook/order_book.hpp"
using namespace nanobook;

OrderBook          book;
std::vector<Trade> trades;

book.add_limit(1, Side::Sell, 10'050, 300, trades);         // ask 100.50 x 300 (tick = 0.01)
book.add_limit(2, Side::Sell, 10'060, 500, trades);         // ask 100.60 x 500
book.add_limit(3, Side::Buy,  10'040, 200, trades);         // bid 100.40 x 200

ExecResult r = book.add_ioc(4, Side::Buy, 10'060, 600, trades);
// r.status == OrderStatus::Filled, r.filled == 600
// trades: {seq 1, taker 4, maker 1, 100.50 x 300}, {seq 2, taker 4, maker 2, 100.60 x 300}

book.modify(2, 10'060, 100, trades);                         // size-down keeps priority
book.cancel(3);

// Fill-or-Kill: all 150 or nothing (only 100 rests at 100.60, so this is killed untouched)
ExecResult fok = book.add_fok(5, Side::Buy, 10'060, 150, trades);
// fok.status == OrderStatus::Cancelled, fok.cancel_reason == CancelReason::FOKUnfillable

// Self-trade prevention: trader 42 never trades with itself
const OrderOptions desk{/*trader_id=*/42, STPMode::CancelOldest};
book.add_limit(6, Side::Buy, 10'000, 50, trades, desk);
book.add_market(7, Side::Sell, 50, trades, desk);            // cancels order 6 instead of trading

Level2Snapshot l2 = book.snapshot(5);                        // top-5 levels per side
std::optional<Price> spread = book.spread();
```

Feeding the engine from another thread:

```cpp
#include "nanobook/spsc_queue.hpp"

SPSCQueue<OrderRequest> queue(1 << 16);                      // capacity rounded to a power of two

std::thread matcher([&] {                                    // the only consumer
    OrderBook book;  std::vector<Trade> trades;  OrderRequest req;
    while (running) {
        if (queue.try_pop(req)) { trades.clear(); book.submit(req, trades); }
    }
});

while (!queue.try_push(OrderRequest{/*id=*/1, Side::Buy, OrderType::FOK, 10'050, 100})) {}  // the only producer
```

---

## Project Layout

```
include/nanobook/
  types.hpp          Price/Quantity/OrderId/TraderId, Side, OrderType, STPMode, Trade, ExecResult
  order_pool.hpp     OrderNode, PriceLevel, free-list slab allocator
  order_book.hpp     OrderBook public API
  market_data.hpp    LevelView, Level2Snapshot
  spsc_queue.hpp     Lock-free single-producer / single-consumer ring buffer
src/order_book.cpp   Matching engine implementation (FOK, STP, matching, market data)
tests/               63 unit tests + a small built-in test harness
benchmarks/          1M-order single-thread and 2-thread SPSC pipeline benchmark
demo/                Live ANSI terminal visualizer (nanobook_demo)
.github/workflows/   CI: g++ build + tests + benchmark, TSan, ASan/UBSan, CMake + ctest
```

## Test Coverage

63 unit tests in three suites:

- **Core matching (34):**
  - exact, partial and multi-level fills on both sides
  - FIFO ordering within a level, and better price beating earlier time
  - market and IOC leftovers
  - cancel and modify (keeping or losing priority, reprice into a cross)
  - the empty book, aggressive orders crossing the spread, and checks that the book never ends up crossed
  - invalid quantities and prices, and duplicate ids
  - Level-2 aggregation and trade sequence numbers
- **FOK & STP (21):**
  - FOK full, multi-level and exact-boundary fills
  - partial liquidity killed with the full book state compared before and after, and no sequence numbers used
  - limit-price bound, empty book, validation, `submit` dispatch
  - STP: anonymous orders and `None` mode, different traders, `CancelNewest` before and after partial fills
  - STP: `CancelOldest` within and across levels, market and IOC orders, STP kept through a modify reprice
  - FOK combined with each STP mode
- **SPSC queue (8):**
  - cache-line alignment, power-of-two capacity
  - empty and full behaviour, FIFO order across thousands of wrap-arounds
  - zero-copy `front`/`pop`, move-only payloads
  - a 2-thread stress test sending 2M two-word messages through a 1,024-slot ring, checking order,
    payload integrity and the sum
