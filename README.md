# C++20 Price-Time Priority Limit Order Book

[![CI](https://github.com/HardikGorani-dev/Limit_Order_Book_cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/HardikGorani-dev/Limit_Order_Book_cpp/actions/workflows/ci.yml)

A deterministic, single-threaded C++20 Limit Order Book matching engine implementing strict **Price-Time Priority (FIFO)** execution semantics. It provides both an STL baseline (`Order_book`) and a slab-allocated intrusive implementation (`PooledOrderBook<Capacity>`) backed by a hardened `MemoryPool` with 32-bit slot handles, verified via a 75-check typed unit test suite, 6 memory-pool hardening tests, and a 10-million-operation three-way differential fuzzer against an independent reference specification.

---

## Architecture & Data Structures

### 1. Baseline Engine (`Order_book`)
* **Price Levels (`bids` / `asks`):**
  * `bids`: `std::map<uint64_t, std::list<Order>, std::greater<uint64_t>>` (highest bid at `begin()`).
  * `asks`: `std::map<uint64_t, std::list<Order>, std::less<uint64_t>>` (lowest ask at `begin()`).
* **FIFO Order Queue:** Each price level holds a `std::list<Order>`. New resting orders append to the back; aggressive orders match from the front.
* **Order Lookup Index (`order_index`):** `std::unordered_map<uint64_t, IndexEntry>`, where `IndexEntry` stores `{Side side, uint64_t Price, std::list<Order>::iterator list_iterator}` for $O(1)$ average-case queue splicing on `cancel` and in-place quantity updates on `reduce`.

### 2. Pooled Engine (`PooledOrderBook<PoolCapacity>` + `MemoryPool<T, Capacity>`)
* **Intrusive Slab Pool (`MemoryPool`):** Pre-allocates a cache-aligned contiguous slab (`std::align_val_t{std::max(alignof(T), alignof(void*))}`) of `Capacity` slots at startup. Free slots form an $O(1)$ LIFO intrusive singly-linked list (`Freenode* next` stored in the first 8 bytes of each free slot).
  * **Sanitizer & Debug Hardening:** On `free()`, bytes `[8, sizeof(T))` are wiped with `0xEF` in Debug builds and poisoned via `ASAN_POISON_MEMORY_REGION` under AddressSanitizer (`allocate()` unpoisons the slot prior to placement `new`). `free()` and `index_of()` assert pointer bounds and slot-stride alignment in Debug builds.
* **32-Byte Intrusive Order Node (`PooledNode`):** Packs `Order` (24 bytes) + `uint32_t prev` (4 bytes) + `uint32_t next` (4 bytes) into exactly **32 bytes** (2 nodes per 64-byte CPU cache line), replacing 16-byte raw pointer pairs with 8-byte pool-index handles.
* **Price Levels & Index:**
  * `bids` / `asks`: `std::map<uint64_t, PooledLevel>` where `PooledLevel` holds `{uint32_t head, uint32_t tail}` pool handles (`UINT32_MAX` as `INVALID_HANDLE`).
  * `order_index`: Retained `std::unordered_map<uint64_t, uint32_t>` mapping `Order_id -> 32-bit pool handle`. Because `PooledNode` embeds `Price`, `side`, `prev`, and `next`, the index entry shrinks from 24 bytes (`IndexEntry`) to 4 bytes (`uint32_t`), and resting order allocations drop by exactly **1.00 allocation per order** (`2.15 -> 1.15 allocs/op`).
* **Exhaustion Contract:** `insert()` returns `false` without mutating book state if `node_pool.allocate()` returns `nullptr`. In `process_aggressive_order()`, an unfilled remainder can only fail to rest (returning `false`) when zero trades occurred and the pool was already full; any trade that leaves an incoming remainder necessarily fully consumed and freed at least one resting order slot, guaranteeing that the remainder's `insert()` succeeds.

### Asymptotic Complexity ($N$ = total resting orders, $P$ = active price levels, $K$ = orders matched)

| Operation | Baseline (`Order_book`) | Pooled (`PooledOrderBook`) | Notes |
| :--- | :--- | :--- | :--- |
| **`insert` (Passive)** | $O(\log P)$ | $O(\log P)$ | Hash lookup $O(1)$ avg + tree lookup $O(\log P)$ + $O(1)$ tail append |
| **`process_aggressive_order`** | $O(K + \log P)$ | $O(K + \log P)$ | $O(1)$ per matched resting order + $O(\log P)$ per depleted level erased |
| **`cancel`** | $O(1)$ avg, $O(\log P)$ if level emptied | $O(1)$ avg, $O(\log P)$ if level emptied | Hash lookup + $O(1)$ doubly-linked list unlink; `map::erase` only when last order at price is removed |
| **`reduce`** | $O(1)$ avg | $O(1)$ avg | In-place quantity decrement via hash index; delegates to `cancel` if reduced to 0 |
| **`best_bid` / `best_ask`** | $O(1)$ | $O(1)$ | Reads `bids.begin()` / `asks.begin()` |

---

## Verification & Correctness

The engine is validated by three independent testing layers (run via `ctest` in CI across GCC and Clang in both `Release` and `Debug` with `-fsanitize=address,undefined -fno-sanitize-recover=all -Werror`):

1. **Hardened Memory Pool Unit Suite (`tests/test_pool.cpp`):** 6 dedicated tests verifying LIFO slot reuse and 32-bit handle round-tripping (`index_of` / `at`), pool exhaustion (`nullptr`) and recovery after `free()`, 64-byte over-aligned allocation under randomized heap jitter, live-object address stability, non-trivial destructor invocation on `free()`, and `EXPECT_DEATH` use-after-free detection under AddressSanitizer.
2. **Typed Behavioral Specification Suite (`tests/test_orderbook.cpp`):** 25 specification test cases instantiated via GoogleTest `TYPED_TEST_SUITE` across all three implementations (`Order_book`, `ReferenceBook`, and `PooledOrderBook<4096>`) = **75 typed checks** + 1 dedicated pool-exhaustion recovery test. Every test invokes `check_invariants()` in `TearDown()`.
3. **Three-Way Differential Fuzzer (`tests/fuzz_orderbook.cpp`):** Generates deterministic pseudo-random streams of passive inserts, aggressive orders, cancellations, and quantity reductions, applying every event simultaneously to `Order_book`, `PooledOrderBook<4096>`, and a simple flat-vector `ReferenceBook` ($O(N)$ specification oracle). Over a **10,000,000-operation** run (`seed = 42`), all three books produce identical return statuses, identical fill sequences (`3,936,578` trades, `2,045,539` partial fills, `748,004` multi-level sweeps, `1,067,787` middle/back-of-queue cancellations), and zero invariant violations.
4. **Structural Invariants (`check_invariants()`):** Validates all 10 book invariants (`empty_level`, `zero_quantity`, `price_mismatch`, `side_mismatch`, `missing_from_index`, `index_data_mismatch`, `iterator_mismatch`, `size_mismatch`, `link_mismatch` bounded by `live_count()` to prevent cycle hangs, and `pool_live_count_mismatch` verifying `node_pool.live_count() == order_index.size()`). Evaluated against 28 seeded baseline bugs and 30 seeded pooled-book bugs, the combined test and fuzz suites achieve a **100% kill rate**.

---

## Benchmark & Latency Profile (Baseline vs. Pooled)

### Test Environment & Methodology
* **Hardware / OS:** Windows 11 (x86-64), pinned to Core 0 via `SetThreadAffinityMask`.
* **Compiler & Flags:** MinGW-w64 GCC 15.2.0, `-O3 -DNDEBUG -std=c++20 -fno-omit-frame-pointer`.
* **Timing Instrumentation:** Hardware Time Stamp Counter (`__rdtsc` serialized with compiler signal fences), calibrated at startup against a 200 ms `std::chrono::steady_clock` window (`3.893 GHz`). Back-to-back `__rdtsc` timer floor is `39 cycles` (`10.0 ns` p50, `8.3 ns` mean), corresponding to the CPU's ~10 ns reference clock increment.
* **Workload (`Seed = 1337`, `500,000` operations):** Bounded random-walk mid-price (`10,000 ± 1` per step) with 52% aggressive limit orders (`42.9%` cross rate executing `217,277` trades and `5,497,507` shares), 38% cancellations, and 10% quantity reductions. Cancellations and reductions sample from a sliding 2,000-ID window (producing `20.4%` cancel hits / `79.6%` cancel misses after aggressive fills, and `15.1%` reduce hits / `84.9%` reduce misses), which are reported separately below.
* **Resting Book Depth Trajectory (100k..500k ops):** `809 -> 1,096 -> 1,855 -> 1,715 -> 2,335` resting orders (identical across both engines).
* **Protocol:** 1 full warm-up run followed by 3 measured runs of 500,000 operations per book; the table reports the **median throughput run** for each implementation.

### 1. Baseline `Order_book` (`std::map` + `std::list` + `std::unordered_map`)

| Operation Category | Count | p50 (ns) | p99 (ns) | p99.9 (ns) | Max (ns) | Heap Allocs / Op |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| **Matched** (Aggressive Cross) | 111,845 | 60 | 260 | 391 | 143,683 | 0.12 |
| **Rested** (Passive Insert) | 148,672 | 100 | 220 | 441 | 139,695 | 2.15 |
| **Cancel (Hit)** (Unlink + Erase) | 38,621 | 80 | 160 | 391 | 124,497 | 0.00 |
| **Cancel (Miss)** (Unknown ID) | 150,989 | 10 | 50 | 80 | 347,060 | 0.00 |
| **Reduce (Hit)** (In-Place / Cancel) | 7,545 | 30 | 90 | 130 | 351 | 0.00 |
| **Reduce (Miss)** (Unknown / Over-Qty) | 42,328 | 20 | 50 | 90 | 43,733 | 0.00 |

* **Baseline Instrumented Throughput (Median of 3 Runs):** `12.42 M ops/sec`

### 2. Pooled `PooledOrderBook<16384>` (Intrusive 32-Byte Slab Pool + 32-Bit Handles)

| Operation Category | Count | p50 (ns) | p99 (ns) | p99.9 (ns) | Max (ns) | Heap Allocs / Op |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| **Matched** (Aggressive Cross) | 111,845 | 50 | 220 | 341 | 123,905 | 0.08 |
| **Rested** (Passive Insert) | 148,672 | 80 | 190 | 381 | 246,308 | 1.15 |
| **Cancel (Hit)** (Unlink + Pool Free) | 38,621 | 70 | 140 | 311 | 16,982 | 0.00 |
| **Cancel (Miss)** (Unknown ID) | 150,989 | 10 | 40 | 70 | 143,172 | 0.00 |
| **Reduce (Hit)** (In-Place / Cancel) | 7,545 | 20 | 100 | 140 | 102,886 | 0.00 |
| **Reduce (Miss)** (Unknown / Over-Qty) | 42,328 | 10 | 50 | 70 | 882 | 0.00 |

* **Pooled Instrumented Throughput (Median of 3 Runs):** `14.71 M ops/sec` (**+18.4%** throughput speedup over STL baseline)

### Key Takeaways & Known Limitations
* **Heap Allocation Elimination on Order Nodes:** Replacing `std::list<Order>` with `MemoryPool<PooledNode, 16384>` drops heap allocations on resting orders by **46.5%** (`2.15 -> 1.15 allocs/op`—removing the `1.00` list-node allocation per resting order). The remaining `1.15 allocs/op` come from the retained `std::unordered_map` hash node (`1.00 alloc/op`) plus occasional `std::map` price-level creations and hash bucket rehashes (`~0.15 allocs/op`).
* **Latency Reductions Across Critical Paths:** `PooledOrderBook` improves `Rested` latency by **20.0% at p50** (`100 ns -> 80 ns`) and **13.6% at p99** (`220 ns -> 190 ns`), `Matched` latency by **16.7% at p50** (`60 ns -> 50 ns`) and **15.4% at p99** (`260 ns -> 220 ns`), and `Cancel (Hit)` latency by **12.5% at p50** (`80 ns -> 70 ns`) and **20.5% at p99.9** (`391 ns -> 311 ns`).
* **Measurement Limitations:**
  1. **Hardware TSC Resolution Step:** On this CPU, `__rdtsc` advances in `39-cycle (~10.0 ns)` steps, so `p50` and `p99` figures are quantized to ~10 ns increments.
  2. **OS Scheduler Jitter:** `Max (ns)` tail outliers (`100–347 µs`) reflect non-isolated Windows OS interrupts and context switches on Core 0 rather than matching logic.

---

## Build & Run Instructions

### Prerequisites
* CMake `>= 3.16`
* C++20 compiler (GCC `>= 11`, Clang `>= 14`, or MSVC)

### Build, Test, Fuzz, and Benchmark
```bash
# 1. Configure Release build
cmake -B build -DCMAKE_BUILD_TYPE=Release

# 2. Build all targets
cmake --build build -j

# 3. Run the 83 CTest checks (75 typed tests + 1 exhaustion test + 6 pool tests + 1M fuzz smoke test)
ctest --test-dir build --output-on-failure

# 4. Run the 10-million-operation 3-way differential fuzzer
./build/tests/fuzz_orderbook 10000000 42

# 5. Run the calibrated __rdtsc Baseline vs. Pooled benchmark
./build/lob_benchmark

---

## Engineering Roadmap

- [x] **Core Price-Time Priority Matching Engine:** Passive insert, aggressive matching, $O(1)$ cancel, and in-place reduce.
- [x] **Differential Fuzzing & Typed Test Suite:** 75 typed GoogleTest checks + 10M-op 3-way fuzzer against `ReferenceBook`.
- [x] **Intrusive Slab Memory Pool (`MemoryPool`):** Pre-allocated, cache-aligned fixed-capacity pool with ASan/debug slot poisoning and 6 unit tests.
- [x] **Pooled Order Book (`PooledOrderBook`):** 32-byte intrusive order nodes with 32-bit pool handles cutting resting allocations from 2.15 to 1.15 allocs/op and increasing throughput from 12.42M to 14.71M ops/sec.
- [ ] **Open-Addressing Flat Hash Index & Flat Price Ladder:** Replace `std::unordered_map` and `std::map` to eliminate the remaining 1.15 allocs/op on resting orders.