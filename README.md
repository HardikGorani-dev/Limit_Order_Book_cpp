# C++20 Limit Order Book & Matching Engine

[![CI](https://github.com/HardikGorani-dev/Limit_Order_Book_cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/HardikGorani-dev/Limit_Order_Book_cpp/actions/workflows/ci.yml)

A price-time priority (FIFO) financial limit order book and matching engine written in standard C++20.

**Current Status:** Core STL baseline engine, 24-case typed specification suite, 10M-operation differential fuzzer against a flat-vector reference model, and per-operation latency/allocation benchmark harness are complete. Intrusive slab memory pool integration is in progress.

---

## Build, Test, and Run

### 1. Build (Release Default)
~~~bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
~~~

### 2. Run Unit Tests & Fuzz Smoke Test
`ctest` runs the 48 unit checks (24 test cases instantiated across both `Order_book` and `ReferenceBook`) followed by a deterministic 1,000,000-operation differential fuzz smoke test:
~~~bash
ctest --test-dir build --output-on-failure
~~~

### 3. Run the Differential Fuzzer Directly
Pass the operation count and an optional 64-bit replay seed:
~~~bash
./build/tests/fuzz_orderbook 10000000 42
~~~

### 4. Run the Benchmark
~~~bash
./build/lob_benchmark
~~~

---

## Architecture & API Contracts

The baseline engine (`Order_book`) maintains price levels in ordered red-black trees (`std::map` descending for bids, ascending for asks), FIFO queues per level (`std::list<Order>`), and a hash index (`std::unordered_map<uint64_t, IndexEntry>`) storing each resting order's list iterator, price, and side.

| Method | Contract & Preconditions | Complexity |
| :--- | :--- | :--- |
| `insert(Order)` | Passive insert only (does not match). Rejects `Quantity == 0` or duplicate `Order_id` (`false`). | `O(1)` hash lookup/insert + `O(log M)` level lookup + `O(1)` list append |
| `process_aggressive_order(Order, out_trade)` | Matches against opposite side in strict price-time priority up to `order.Price`. Fills execute at the resting order's price; any unfilled remainder rests at `order.Price`. Rejects `Quantity == 0` or duplicate `Order_id`. | `O(K + log M)` for `K` resting orders swept |
| `cancel(order_id)` | Removes a resting order and erases its price level if empty. Returns `false` for unknown IDs. | `O(1)` hash lookup + `O(log M)` level lookup + `O(1)` list erase |
| `reduce(order_id, decrease_by)` | Decreases resting quantity in-place without losing FIFO queue priority. Rejects `decrease_by == 0`, unknown IDs, or `decrease_by > current_qty` (`false`). Calls `cancel(order_id)` if `decrease_by == current_qty`. | `O(1)` partial reduce; `O(1)` hash + `O(log M)` level + `O(1)` erase when `decrease_by == current_qty` |
| `check_invariants()` | Checks for empty price levels, zero resting quantities, order price/side mismatches against the enclosing level, missing index entries, index entries whose price, side, or iterator disagrees with the resting order, and index-to-book order count mismatches. | `O(N)` verification |

---

## Verification & Differential Fuzzing

Correctness is enforced before optimization through four layers:

1. **Typed Unit Specification (`tests/test_orderbook.cpp`):**
   24 hand-written GoogleTest cases templated via `TYPED_TEST` over both `Order_book` and `ReferenceBook` (48 total checks). Covers exact/partial fills, multi-level sweeps on both sides, FIFO queue priority preservation under partial fills and reductions, middle-of-queue cancellations, duplicate ID rejection, and `check_invariants()` verification in `TearDown()`.
2. **Brute-Force Reference Oracle (`include/lob/ReferenceBook.hpp`):**
   A flat `std::vector` implementation that assigns a monotonic sequence number on arrival and scans linearly for best price and lowest sequence number. Contains zero secondary indices so structural bugs cannot hide.
3. **Differential Fuzzer (`tests/fuzz_orderbook.cpp`):**
   Drives both implementations with identical randomized streams (inserts, aggressive sweeps, cancels, and reduces) and compares return values and exact emitted trade sequences after every operation, dumping the last 50 operations and replay seed on divergence. Enforces non-zero coverage gates across partial fills, multi-level sweeps, duplicate rejections, rejected reduces, and non-front cancels.
   * **Fault-Injection Coverage:** Validated against seeded implementation bugs (flipped price comparisons, LIFO queue regressions, stale index entries, buyer/seller ID swaps, and wrong traded quantities).
4. **Sanitizers in CI (`.github/workflows/ci.yml`):**
   Runs on every push and pull request across GCC and Clang on Ubuntu in both `Release` and `Debug` (`-fsanitize=address,undefined -fno-sanitize-recover=all -Werror`).

---

## Baseline Benchmark Results

Workload: 500,000 operations generated around a random-walk mid-price. Every new order enters via `process_aggressive_order` (36.4% cross rate), paired with cancellations and reductions (resting depth grows from ~1.3k to 3,156 orders across the 500k-op run; 1,760 orders at the 100k-op sanity check alongside 36,574 trades and 1,012,706 shares matched).

| Operation Category | Count | p50 (ns) | p99 (ns) | p99.9 (ns) | Max (ns) | Heap Allocs / Op |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| **Matched (Aggressive)** | 94,183 | 100 | 300 | 500 | 205,400 | 0.05 |
| **Rested (Passive)** | 164,340 | 100 | 200 | 500 | 167,000 | 2.08 |
| **Cancel** | 191,377 | 100 | 200 | 300 | 199,200 | 0.00 |
| **Reduce** | 50,100 | 0 | 100 | 200 | 3,900 | 0.00 |

* **Instrumented Throughput:** `8.24 M ops/sec` (includes per-operation `steady_clock::now()` reads)

### Benchmark Environment & Measurement Limitations
* **Environment:** Windows 11, MinGW-w64 GCC 15.2.0, `-O3 -Wall -Wextra -Wpedantic -fno-omit-frame-pointer`, pinned to Core 0 via `SetThreadAffinityMask`.
* **Clock Granularity & Tail Noise:** On Windows, `std::chrono::steady_clock` queries the 10 MHz `QueryPerformanceCounter`, which has a **100 ns tick resolution**. Back-to-back clock reads have a `0 ns` median and single-operation latencies are quantized into multiples of `100 ns`. The ~200 us `Max` spikes appear across both allocating (`Matched`, `Rested`) and non-allocating (`Cancel`) paths and reflect OS scheduling/timer interrupts rather than allocator cost.
* **Heap Allocation Profile:** `Rested` orders average `2.08` heap allocations per operation (`std::list` node + `std::unordered_map` index node + occasional `std::map` level node).

---

## Engineering Roadmap

- [x] **STL Baseline Engine:** Price-time priority matching (`std::map`, `std::list`, `std::unordered_map`) with strict invariant verification.
- [x] **Verification & Differential Fuzzing:** 24-case typed GoogleTest suite, flat-vector `ReferenceBook` oracle, 10M-operation differential fuzzer, and CI with ASan/UBSan.
- [x] **Intrusive Slab Memory Pool:** Pre-allocated, cache-aligned fixed-capacity pool with debug slot poisoning to remove the list-node heap allocation (~1 of the `2.08 allocs/op` on resting orders).
- [ ] **Intrusive Pooled Order Book:** Replace `std::list` order queues with pooled intrusive doubly-linked lists, benchmarking before/after latency and allocation counts.
