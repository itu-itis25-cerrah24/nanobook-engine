# NanoBook

[![CI](https://github.com/itu-itis25-cerrah24/nanobook-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/itu-itis25-cerrah24/nanobook-engine/actions/workflows/ci.yml)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)
![Dependencies](https://img.shields.io/badge/dependencies-none-brightgreen)

**NanoBook** is a low-latency **Limit Order Book & Matching Engine** written in modern C++20.
It matches orders with strict **price-time (FIFO) priority**, uses **fixed-point integer prices**
with no floating point on the matching path, and processes **~14.7 million orders per second** with a
**median latency of ~83 ns** on a single core. It uses only the standard library.

- **Order types:** `LIMIT`, `MARKET`, `IOC` (Immediate-or-Cancel), plus `CANCEL` and `MODIFY`
- **Market data:** Level-2 depth snapshots (top *N* levels), best bid/ask, spread
- **Execution reports:** structured `Trade` events with sequence numbers, taker/maker ids and aggressor side
- **Tooling:** 34 unit tests, a 1M-order latency benchmark, and a live ANSI terminal visualizer

---

## Benchmark Results

1,000,000 synthetic orders replayed on a single thread. The order stream comes from a fixed seed,
so every run processes identical input (55% limit, 30% cancel, 7% market, 4% IOC, 4% modify).
The run produces 256,049 trades, and up to ~96k orders rest on the book at once.

| Metric | Result |
|---|---|
| **Throughput** | **14,725,089 orders/sec** (1M orders in 67.9 ms) |
| **p50 latency** | **83 ns** |
| **p95 latency** | **180 ns** |
| **p99 latency** | **288 ns** |
| **p99.9 latency** | **498 ns** |
| Mean latency | 99 ns |

Per operation type (ns):

| Type | p50 | p95 | p99 | p99.9 |
|---|---:|---:|---:|---:|
| Limit  |  77 | 140 | 200 |  409 |
| Market |  45 | 168 | 264 |  461 |
| IOC    |  43 | 163 | 260 |  468 |
| Cancel |  96 | 220 | 331 |  540 |
| Modify | 149 | 290 | 401 | 1173 |

<sub>Intel Core i7-13700HX · Windows 11 · g++ 15.2 (MSYS2 UCRT64) · `-std=c++20 -O3 -march=native`.
Each order is timed with a serialized `RDTSC`, calibrated against `steady_clock`, and the figures include
~12 ns of timer overhead. Throughput is measured in a separate replay with no per-order timers.
Results vary by hardware. Across repeated runs, p50 stays at 83–89 ns and p99.9 at 445–562 ns.</sub>

Reproduce with `.\build.ps1 bench` or `make bench`. Pass an order count to change the size, for example
`./build/nanobook_bench 200000`.

---

## Time Complexity

`L` = number of active price levels on one side (typically tens), `F` = number of fills an order generates.

| Operation | Complexity | How |
|---|---|---|
| Best bid / best ask / spread | **O(1)** | `begin()` of a per-side ordered map |
| Lookup order by id | **O(1)** avg | `std::unordered_map<OrderId, NodeIndex>` |
| Cancel | **O(1)** | hash lookup + unlink from intrusive FIFO list (+ O(log L) only if the level becomes empty) |
| Modify: reduce qty, same price | **O(1)** | in-place update, keeps queue priority |
| Modify: reprice / increase qty | O(log L) | cancel/replace, loses priority, may trade |
| Add resting limit | O(log L) | find-or-create level, O(1) append to tail |
| Match (per fill) | **O(1)** | pop head of best level's FIFO queue |
| Market / IOC / marketable limit | O(F + levels consumed) | sweep from best price outward |
| Level-2 snapshot (depth N) | O(N) | walk the first N levels of each side |
| Node allocation / release | **O(1)** | free-list slab pool, no `new`/`delete` in steady state |

---

## Architecture

```
                         ┌──────────────────────────────────────────────┐
  OrderRequest ────────▶ │                  OrderBook                   │ ──────▶ Trade events
  (Limit/Market/IOC,     │                                              │         (seq, taker, maker,
   Cancel, Modify)       │  Order Index  std::unordered_map             │          side, price, qty)
                         │  ┌──────────────────────────┐                │
                         │  │ OrderId ──▶ NodeIndex    │──┐  O(1)       │ ──────▶ Level-2 snapshot
                         │  └──────────────────────────┘  │  lookup     │         (top-N, BBO, spread)
                         └────────────────────────────────┼─────────────┘
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
- **Memory pool.** `OrderNode`s live in one contiguous slab. Freed slots go on an index-linked free list
  and are reused before the slab grows, so a busy book does no heap allocation.
- **Intrusive FIFO queues.** Each `PriceLevel` stores `head`/`tail` indices and a running `total_qty`.
  Each order stores `prev`/`next` indices and a pointer back to its level. Unlinking from anywhere in the
  queue is O(1), and Level-2 quantities never need re-summing.
- **Order index.** A hash map from `OrderId` to pool slot gives O(1) cancel and modify without scanning levels.
- **Allocation-free trade reporting.** Trades are appended to a caller-owned `std::vector<Trade>`, so a hot
  loop reuses one buffer.

### Matching rules

| Order | Behaviour |
|---|---|
| `LIMIT`  | Trades against the opposite side while prices cross. The remainder rests at its limit price. |
| `MARKET` | Trades at any price until filled or the book is empty. The remainder is **cancelled**. |
| `IOC`    | Trades up to its limit price. The remainder is **cancelled** and never rests. |
| `CANCEL` | Removes a resting order. Returns `false` for unknown or already-filled ids. |
| `MODIFY` | Reducing the quantity at the same price keeps queue priority. Repricing or increasing the quantity is a cancel/replace: priority is lost, and the order can trade at once if it now crosses. |

All trades print at the **resting (maker) order's price**. Aggressive orders sweep from the best price
outward, so the book can never be left crossed. Requests with a non-positive quantity or price, or with
a duplicate id, are rejected and leave the book unchanged.

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
.\build.ps1              # build all, run tests, render a demo frame, run the benchmark
.\build.ps1 test         # build + unit tests
.\build.ps1 bench        # build + 1M-order benchmark
.\build.ps1 demo         # build + live visualizer (Ctrl+C to quit)
.\build\nanobook_demo.exe --snapshot --symbol NVDA
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
-Wold-style-cast -Werror`.

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

Level2Snapshot l2 = book.snapshot(5);                        // top-5 levels per side
std::optional<Price> spread = book.spread();
```

---

## Project Layout

```
include/nanobook/
  types.hpp          Price/Quantity/OrderId, Side, OrderType, Trade, ExecResult
  order_pool.hpp     OrderNode, PriceLevel, free-list slab allocator
  order_book.hpp     OrderBook public API
  market_data.hpp    LevelView, Level2Snapshot
src/order_book.cpp   Matching engine implementation
tests/               34 unit tests + a small built-in test harness
benchmarks/          1M-order latency / throughput benchmark
demo/                Live ANSI terminal visualizer (nanobook_demo)
.github/workflows/   CI: g++ build, tests, visualizer snapshot, benchmark, CMake + ctest
```

## Test Coverage

The 34 unit tests cover:

- exact and partial fills on both sides
- multi-level sweeps, with every trade at the maker's price
- FIFO ordering within a level, and better price beating earlier time
- market orders: full fill, leftover cancelled, empty book
- IOC: partial fill, full fill, no cross
- cancel: head, middle and tail of a queue, double cancel, already-filled orders, reuse of freed pool slots
- modify: size-down keeps priority, size-up loses it, reprice, reprice into a cross, rejections
- aggressive orders crossing the spread, and checks that the book never ends up crossed
- invalid quantities and prices, and duplicate ids
- Level-2 aggregation and truncation, and trade sequence numbers
