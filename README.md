# C++20 Price-Time Priority Limit Order Book

[![CI](https://github.com/HardikGorani-dev/Limit_Order_Book_cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/HardikGorani-dev/Limit_Order_Book_cpp/actions/workflows/ci.yml)

A deterministic, single-threaded C++20 Limit Order Book matching engine implementing strict **Price-Time Priority (FIFO)** execution semantics. It provides both an STL baseline (`Order_book`) and a slab-allocated intrusive implementation (`PooledOrderBook<Capacity>`) backed by a hardened `MemoryPool` with 32-bit slot handles, verified via **82 CTest checks** (72 typed specification checks across 3 book implementations, 3 non-typed edge-case tests, 6 memory-pool hardening tests, and a 1M-op fuzz smoke test) plus a **10-million-operation** three-way differential fuzzer against an independent reference oracle.

---

## Architecture & Data Structures

### 1. Baseline Engine (`Order_book`)
* **Price Levels (`bids` / `asks`):**
  * `bids`: `std::map<uint64_t, std::list<Order>, std::greater<uint64_t>>` (highest bid at `begin()`).
  * `asks`: `std::map<uint64_t, std::list<Order>, std::less<uint64_t>>` (lowest ask at `begin()`).
* **FIFO Order Queue:** Each price level holds a `std::list<Order>`. New resting orders append to the back; aggressive orders match from the front.
* **Order Lookup Index (`order_index`):** `std::unordered_map<uint64_t, IndexEntry>`, where `IndexEntry` stores `{Side side, uint64_t Price, std::list<Order>::iterator list_iterator}` for $O(1)$ average-case handle lookup followed by $O(\log P)$ price-level lookup and $O(1)$ list splicing on `cancel`, and $O(1)$ average-case in-place quantity updates on partial `reduce`.

### 2. Pooled Engine (`PooledOrderBook<PoolCapacity>` + `MemoryPool<T, Capacity>`)
* **Intrusive Slab Pool (`MemoryPool`):** Pre-allocates a cache-aligned contiguous slab (`std::align_val_t{std::max(alignof(T), alignof(void*))}`) of `Capacity` slots at startup. Free slots form an $O(1)$ LIFO intrusive singly-linked list (`Freenode* next` stored in the first 8 bytes of each free slot).
  * **Sanitizer & Debug Hardening:** On `free()`, bytes `[8, sizeof(T))` are wiped with `0xEF` in Debug builds and poisoned via `ASAN_POISON_MEMORY_REGION` under AddressSanitizer (`allocate()` unpoisons the slot prior to placement `new`). `free()` and `index_of()` assert pointer bounds and slot-stride alignment in Debug builds.
* **32-Byte Intrusive Order Node (`PooledNode`):** Packs `Order` (24 bytes) + `uint32_t prev` (4 bytes) + `uint32_t next` (4 bytes) into exactly **32 bytes** (2 nodes per 64-byte CPU cache line), replacing 16-byte raw pointer pairs with 8-byte pool-index handles.
* **Price Levels & Index:**
  * `bids` / `asks`: `std::map<uint64_t, PooledLevel>` where `PooledLevel` holds `{uint32_t head, uint32_t tail}` pool handles (`UINT32_MAX` as `INVALID_HANDLE`).
  * `order_index`: Retained `std::unordered_map<uint64_t, uint32_t>` mapping `Order_id -> 32-bit pool handle`. Because `PooledNode` embeds `Price`, `side`, `prev`, and `next`, the index entry shrinks from 24 bytes (`IndexEntry`) to 4 bytes (`uint32_t`), and resting order allocations drop deterministically by **1.00 allocation per order** (`2.15 -> 1.15 allocs/op`, a **46.5% reduction**).
* **Exhaustion Contract:** `insert()` returns `false` without mutating book state if `node_pool.allocate()` returns `nullptr`. In `process_aggressive_order()`, an unfilled remainder can only fail to rest (returning `false`) when zero trades occurred and the pool was already full; any trade that leaves an incoming remainder necessarily fully consumed and freed at least one resting order slot, guaranteeing that the remainder's `insert()` succeeds.

### Asymptotic Complexity ($N$ = total resting orders, $P$ = active price levels, $K$ = orders matched)

| Operation | Baseline (`Order_book`) | Pooled (`PooledOrderBook`) | Notes |
| :--- | :--- | :--- | :--- |
| **`insert` (Passive)** | $O(\log P)$ | $O(\log P)$ | Hash lookup $O(1)$ avg + tree lookup $O(\log P)$ + $O(1)$ tail append |
| **`process_aggressive_order`** | $O(K + \log P)$ | $O(K + \log P)$ | $O(1)$ per matched resting order + $O(\log P)$ per depleted level erased |
| **`cancel`** | $O(\log P)$ | $O(\log P)$ | $O(1)$ avg hash lookup + $O(\log P)$ `std::map::find(price)` + $O(1)$ doubly-linked node unlink (and level erase if empty) |
| **`reduce` (Partial)** | $O(1)$ avg | $O(1)$ avg | In-place quantity decrement via hash index; delegates to $O(\log P)$ `cancel` if reduced to 0 |
| **`best_bid` / `best_ask`** | $O(1)$ | $O(1)$ | Reads `bids.begin()` / `asks.begin()` |

---

## Verification & Correctness

The engine is validated by **82 CTest checks** run in CI across GCC and Clang in both `Release` and `Debug` with `-fsanitize=address,undefined -fno-sanitize-recover=all -Werror`:

1. **Hardened Memory Pool Unit Suite (`tests/test_pool.cpp` — 6 tests):** Verifies LIFO slot reuse and 32-bit handle round-tripping (`index_of` / `at`), pool exhaustion (`nullptr`) and recovery after `free()`, 64-byte over-aligned allocation under randomized heap jitter, live-object address stability, non-trivial destructor invocation on `free()`, and `EXPECT_DEATH` use-after-free detection under AddressSanitizer.
2. **Typed & Non-Typed Order Book Suite (`tests/test_orderbook.cpp` — 75 tests):** 24 specification test cases instantiated via GoogleTest `TYPED_TEST_SUITE` across all three implementations (`Order_book`, `ReferenceBook`, and `PooledOrderBook<4096>`) = **72 typed checks**, plus **3 non-typed tests** covering bid-side empty-level removal without subsequent matching (`Order_book` and `PooledOrderBook`) and pool exhaustion recovery across passive and aggressive orders (`PooledOrderBook<3>`). Every typed test invokes `check_invariants()` in `TearDown()`.
3. **Three-Way Differential Fuzzer (`tests/fuzz_orderbook.cpp` — 1M CTest smoke test + 10M CLI verification):** Generates deterministic pseudo-random streams of passive inserts, aggressive orders, cancellations, and quantity reductions, applying every event simultaneously to `Order_book`, `PooledOrderBook<4096>`, and a flat-vector `ReferenceBook` ($O(N)$ specification oracle). Over a **10,000,000-operation** run (`seed = 42`), all three books produce identical return statuses, identical fill sequences (`3,936,578` trades, `2,045,539` partial fills, `748,004` multi-level sweeps, `1,067,787` middle/back-of-queue cancellations), and zero invariant violations.
4. **Structural Invariants (`check_invariants()`):** Validates all 10 book invariants (`empty_level`, `zero_quantity`, `price_mismatch`, `side_mismatch`, `missing_from_index`, `index_data_mismatch`, `iterator_mismatch`, `size_mismatch`, `link_mismatch` bounded by `live_count()` to prevent cycle hangs, and `pool_live_count_mismatch` verifying `node_pool.live_count() == order_index.size()`). All seeded bugs tested during development across both the baseline and pooled implementations were caught by the combined unit and differential fuzz suites.

---

## Benchmark & Allocation Profile (Baseline vs. Pooled)

### Test Environment & Methodology
* **Hardware / OS:** AMD Ryzen 5 5600G (6C/12T, 3.9 GHz base / 4.4 GHz boost, 3 MB L2, 16 MB L3 cache), Windows 11 (x86-64), pinned to Core 0 via `SetThreadAffinityMask`.
* **Compiler & Flags:** MinGW-w64 GCC 15.2.0, `-O3 -DNDEBUG -std=c++20 -fno-omit-frame-pointer`.
* **Timing Instrumentation:** Hardware Time Stamp Counter (`__rdtsc`), compiler-fenced via `std::atomic_signal_fence(std::memory_order_seq_cst)` (no `lfence`/`rdtscp` hardware serialization; invariant TSC assumed), calibrated at startup against a 200 ms `std::chrono::steady_clock` window (`3.893 GHz`). Back-to-back `__rdtsc` timer floor is `39 cycles` (`10.0 ns` p50, `9.3 ns` mean), corresponding to the platform's ~10 ns reference clock tick.
* **Workload (`Seed = 1337`, `500,000` operations):** Bounded random-walk mid-price (`10,000 ± 1` per step) with 52% aggressive limit orders (`42.9%` cross rate executing `217,277` trades and `5,497,507` shares), 38% cancellations, and 10% quantity reductions. Cancellations and reductions sample from a sliding 2,000-ID window (producing `20.4%` cancel hits / `79.6%` cancel misses after aggressive fills, and `15.1%` reduce hits / `84.9%` reduce misses), reported separately below.
* **Resting Book Depth Trajectory (100k..500k ops):** `809 -> 1,096 -> 1,855 -> 1,715 -> 2,335` resting orders (verified identical across both engines).
* **Interleaved Trial Protocol:** After 1 full warm-up pass per book (`500,000` ops each), the benchmark alternates `Baseline <-> PooledOrderBook<16384>` across 3 interleaved trials so thermal and OS state affect both books equally:
  * **Trial 1:** Baseline `12.19 M ops/sec` (`Rested p50=100ns, p99=220ns`) `<->` Pooled `14.88 M ops/sec` (`Rested p50=70ns, p99=180ns`)
  * **Trial 2:** Baseline `12.24 M ops/sec` (`Rested p50=100ns, p99=220ns`) `<->` Pooled `15.12 M ops/sec` (`Rested p50=70ns, p99=180ns`)
  * **Trial 3:** Baseline `12.15 M ops/sec` (`Rested p50=100ns, p99=230ns`) `<->` Pooled `15.10 M ops/sec` (`Rested p50=70ns, p99=180ns`)

### 1. Baseline `Order_book` (`std::map` + `std::list` + `std::unordered_map`) — Median Run

| Operation Category | Count | p50 (ns) | p99 (ns) | p99.9 (ns) | Max (ns) | Heap Allocs / Op |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| **Matched** (Aggressive Cross) | 111,845 | 60 | 260 | 391 | 140,587 | 0.12 |
| **Rested** (Passive Insert) | 148,672 | 100 | 220 | 461 | 142,711 | 2.15 |
| **Cancel (Hit)** (Unlink + Erase) | 38,621 | 80 | 160 | 351 | 203,346 | 0.00 |
| **Cancel (Miss)** (Unknown ID) | 150,989 | 10 | 50 | 80 | 141,428 | 0.00 |
| **Reduce (Hit)** (In-Place / Cancel) | 7,545 | 30 | 90 | 150 | 35,988 | 0.00 |
| **Reduce (Miss)** (Unknown / Over-Qty) | 42,328 | 20 | 50 | 80 | 58,110 | 0.00 |

* **Baseline Instrumented Throughput:** `12.19 M ops/sec` median (`3-run range: 12.15 – 12.24 M ops/sec`)

### 2. Pooled `PooledOrderBook<16384>` (Intrusive 32-Byte Slab Pool + 32-Bit Handles) — Median Run

| Operation Category | Count | p50 (ns) | p99 (ns) | p99.9 (ns) | Max (ns) | Heap Allocs / Op |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| **Matched** (Aggressive Cross) | 111,845 | 50 | 210 | 321 | 38,453 | 0.08 |
| **Rested** (Passive Insert) | 148,672 | 70 | 180 | 341 | 152,840 | 1.15 |
| **Cancel (Hit)** (Unlink + Pool Free) | 38,621 | 70 | 140 | 351 | 132,231 | 0.00 |
| **Cancel (Miss)** (Unknown ID) | 150,989 | 10 | 40 | 60 | 76,195 | 0.00 |
| **Reduce (Hit)** (In-Place / Cancel) | 7,545 | 20 | 100 | 150 | 551 | 0.00 |
| **Reduce (Miss)** (Unknown / Over-Qty) | 42,328 | 10 | 50 | 70 | 581 | 0.00 |

* **Pooled Instrumented Throughput:** `15.10 M ops/sec` median (`3-run range: 14.88 – 15.12 M ops/sec`)

### Key Takeaways & Measurement Limitations
* **Deterministic 46.5% Heap Allocation Reduction on Resting Orders:** Replacing `std::list<Order>` with `MemoryPool<PooledNode, 16384>` drops heap allocations on resting orders from **`2.15` to `1.15 allocs/op`** across all platforms—eliminating the `1.00` list-node allocation per resting order. The remaining `1.15 allocs/op` come from the retained `std::unordered_map` hash node (`1.00 alloc/op`) plus occasional `std::map` price-level creations and hash bucket rehashes (`~0.15 allocs/op`).
* **Throughput & Latency Across Environments:** On the bare-metal Ryzen 5 5600G testbed above, interleaved throughput rises from `12.15–12.24 M ops/sec` (Baseline) to `14.88–15.12 M ops/sec` (Pooled), with `Rested` latency improving from `100 ns` to `70 ns` (`p50`) and `220 ns` to `180 ns` (`p99`). On virtualized / shared-CPU runners, run-to-run variance is wider and the throughput gain narrows (`~6–8%`), as `std::map` and `std::unordered_map` traversal costs still dominate total runtime.
* **Measurement Limitations:**
  1. **Hardware TSC Quantization (`~10.0 ns` step) & Unserialized `rdtsc`:** On this CPU, `__rdtsc` advances in `39-cycle (~10.0 ns)` ticks and is fenced only against compiler reordering (`std::atomic_signal_fence`). Consequently, single-tick differences (`10 ns`) are at the resolution limit of the timer, and fast reject paths (`Cancel (Miss)` and `Reduce (Miss)` at `10–20 ns` `p50`) sit at the `10.0 ns` timer floor.
  2. **Sample Counts & OS Jitter at Tail Percentiles:** `p99.9` figures on smaller categories (`Cancel (Hit)` with `38,621` samples, `Reduce (Hit)` with `7,545` samples) and `Max (ns)` outliers (`38–203 µs`) reflect non-isolated Windows OS scheduler interrupts on Core 0 rather than algorithmic complexity.

---

## Build & Run Instructions

### Prerequisites
* CMake `>= 3.20`
* C++20 compiler supporting parenthesized aggregate initialization (GCC `>= 11`, Clang `>= 16`, or MSVC 2022)

### Build, Test, Fuzz, and Benchmark
```bash
# 1. Configure Release build
cmake -B build -DCMAKE_BUILD_TYPE=Release

# 2. Build all targets
cmake --build build -j

# 3. Run all 82 CTest checks (72 typed + 3 non-typed + 6 pool + 1M fuzz smoke test)
ctest --test-dir build --output-on-failure

# 4. Run the 10-million-operation 3-way differential fuzzer
./build/tests/fuzz_orderbook 10000000 42

# 5. Run the interleaved Baseline vs. Pooled benchmark
./build/lob_benchmark
```

---

## Engineering Roadmap

- [x] **Core Price-Time Priority Matching Engine:** Passive insert, aggressive matching, $O(\log P)$ cancel with $O(1)$ queue unlink, and $O(1)$ average-case in-place partial reduce.
- [x] **Differential Fuzzing & Typed Test Suite:** 82 CTest checks (72 typed checks across 3 implementations, 3 non-typed edge-case tests, 6 pool tests, 1M fuzz smoke test) + 10M-op 3-way fuzzer against `ReferenceBook`.
- [x] **Intrusive Slab Memory Pool (`MemoryPool`):** Pre-allocated, cache-aligned fixed-capacity pool with ASan/debug slot poisoning and 6 unit tests.
- [x] **Pooled Order Book (`PooledOrderBook`):** 32-byte intrusive order nodes with 32-bit pool handles cutting resting allocations by 46.5% (`2.15 -> 1.15 allocs/op`).
- [ ] **Open-Addressing Flat Hash Index & Intrusive Price Levels:** Replace `std::unordered_map` and `std::map` lookup on cancel to eliminate the remaining `1.15 allocs/op` on resting orders and achieve true $O(1)$ cancellation.