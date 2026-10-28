#include <lob/OrderBook.hpp>
#include <lob/PooledOrderBook.hpp>
#include <lob/ReferenceBook.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <new>
#include <random>
#include <string>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__i386__) || defined(__x86_64__)
#include <x86intrin.h>
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

// ============================================================================
// Global Heap Allocation Counter (Hooks standard + aligned new/delete)
// ============================================================================
static uint64_t g_alloc_count = 0;

void* operator new(std::size_t size) {
    ++g_alloc_count;
    void* p = std::malloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}

void* operator new[](std::size_t size) {
    ++g_alloc_count;
    void* p = std::malloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}

void* operator new(std::size_t size, std::align_val_t al) {
    ++g_alloc_count;
#ifdef _WIN32
    void* p = _aligned_malloc(size, static_cast<std::size_t>(al));
#else
    std::size_t align = static_cast<std::size_t>(al);
    std::size_t rounded = (size + align - 1) & ~(align - 1);
    void* p = std::aligned_alloc(align, rounded);
#endif
    if (!p) throw std::bad_alloc();
    return p;
}

void* operator new[](std::size_t size, std::align_val_t al) {
    return ::operator new(size, al);
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

void operator delete(void* p, std::align_val_t) noexcept {
#ifdef _WIN32
    _aligned_free(p);
#else
    std::free(p);
#endif
}

void operator delete[](void* p, std::align_val_t al) noexcept {
    ::operator delete(p, al);
}

void operator delete(void* p, std::size_t, std::align_val_t al) noexcept {
    ::operator delete(p, al);
}

void operator delete[](void* p, std::size_t, std::align_val_t al) noexcept {
    ::operator delete(p, al);
}

// ============================================================================
// Thread Pinning & Calibrated Hardware TSC Timer
// ============================================================================
static void pin_current_thread() {
#ifdef _WIN32
    DWORD_PTR mask = 1;
    if (SetThreadAffinityMask(GetCurrentThread(), mask) != 0) {
        std::cout << "Thread Affinity: Pinned to Core 0 (Windows SetThreadAffinityMask)\n";
    } else {
        std::cout << "Thread Affinity: Failed to pin thread\n";
    }
#elif defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0) {
        std::cout << "Thread Affinity: Pinned to Core 0 (Linux pthread_setaffinity_np)\n";
    } else {
        std::cout << "Thread Affinity: Failed to pin thread\n";
    }
#else
    std::cout << "Thread Affinity: Unsupported platform\n";
#endif
}

static inline uint64_t read_tsc() noexcept {
#if defined(__i386__) || defined(__x86_64__) || defined(_MSC_VER)
    std::atomic_signal_fence(std::memory_order_seq_cst);
    uint64_t c = __rdtsc();
    std::atomic_signal_fence(std::memory_order_seq_cst);
    return c;
#else
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
#endif
}

static double calibrate_tsc_ghz() {
    // Warm up loop
    volatile uint64_t sink = 0;
    for (int i = 0; i < 10000; ++i) sink += read_tsc();

    auto t0 = std::chrono::steady_clock::now();
    uint64_t c0 = read_tsc();

    // Busy-spin for 200 ms against steady_clock
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0)
               .count() < 200) {
        sink += read_tsc();
    }

    uint64_t c1 = read_tsc();
    auto t1 = std::chrono::steady_clock::now();
    (void)sink;

    double elapsed_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    return static_cast<double>(c1 - c0) / elapsed_ns;
}

struct TimerFloor {
    uint64_t p50_cycles;
    double p50_ns;
    double mean_ns;
};

static TimerFloor measure_tsc_floor(double tsc_ghz) {
    constexpr size_t N = 100'000;
    std::vector<uint64_t> deltas(N);
    uint64_t sum = 0;
    for (size_t i = 0; i < N; ++i) {
        uint64_t c0 = read_tsc();
        uint64_t c1 = read_tsc();
        uint64_t d = (c1 >= c0) ? (c1 - c0) : 0;
        deltas[i] = d;
        sum += d;
    }
    std::sort(deltas.begin(), deltas.end());
    uint64_t p50_c = deltas[N / 2];
    double mean_c = static_cast<double>(sum) / static_cast<double>(N);
    return {p50_c, static_cast<double>(p50_c) / tsc_ghz, mean_c / tsc_ghz};
}

// ============================================================================
// Workload Generator (Identical Deterministic 500k Stream)
// ============================================================================
enum class BenchOpType : uint8_t {
    AGGRESSIVE_NEW,
    CANCEL,
    REDUCE
};

struct BenchEvent {
    BenchOpType type;
    Order order;
    uint32_t reduce_qty;
};

static std::vector<BenchEvent> generate_stream(size_t num_ops, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<BenchEvent> stream;
    stream.reserve(num_ops);

    int64_t mid_price = 10'000;
    uint64_t next_order_id = 1;
    std::vector<uint64_t> recent_ids;
    recent_ids.reserve(num_ops);

    std::uniform_int_distribution<int> op_dist(1, 100);
    std::uniform_int_distribution<int> walk_dist(-1, 1);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<uint32_t> qty_dist(1, 100);
    std::uniform_int_distribution<uint32_t> red_dist(1, 50);
    std::uniform_int_distribution<int> offset_dist(-5, 8);

    for (size_t i = 0; i < num_ops; ++i) {
        mid_price += walk_dist(rng);
        if (mid_price < 500) mid_price = 500;

        int roll = op_dist(rng);
        if (recent_ids.empty() || roll <= 52) {
            Side side = (side_dist(rng) == 0) ? Side::buy : Side::sell;
            int offset = offset_dist(rng);
            uint64_t price = (side == Side::buy)
                ? static_cast<uint64_t>(mid_price - offset)
                : static_cast<uint64_t>(mid_price + offset);
            uint32_t qty = qty_dist(rng);
            uint64_t id = next_order_id++;
            recent_ids.push_back(id);
            stream.push_back({BenchOpType::AGGRESSIVE_NEW, {id, price, qty, side}, 0});
        } else if (roll <= 90) {
            size_t window = std::min<size_t>(recent_ids.size(), 2000);
            std::uniform_int_distribution<size_t> pick(recent_ids.size() - window, recent_ids.size() - 1);
            size_t idx = pick(rng);
            uint64_t target_id = recent_ids[idx];
            recent_ids[idx] = recent_ids.back();
            recent_ids.pop_back();
            stream.push_back({BenchOpType::CANCEL, {target_id, 0, 0, Side::buy}, 0});
        } else {
            size_t window = std::min<size_t>(recent_ids.size(), 2000);
            std::uniform_int_distribution<size_t> pick(recent_ids.size() - window, recent_ids.size() - 1);
            uint64_t target_id = recent_ids[pick(rng)];
            stream.push_back({BenchOpType::REDUCE, {target_id, 0, 0, Side::buy}, red_dist(rng)});
        }
    }
    return stream;
}

static void verify_stream_prefix(const std::vector<BenchEvent>& stream, size_t check_ops) {
    Order_book opt_book;
    PooledOrderBook<16384> pool_book;
    ReferenceBook ref_book;

    std::vector<Trade> t_opt, t_pool, t_ref;
    uint64_t total_trades = 0;
    uint64_t total_shares = 0;

    size_t limit = std::min(check_ops, stream.size());
    for (size_t i = 0; i < limit; ++i) {
        const auto& ev = stream[i];
        if (ev.type == BenchOpType::AGGRESSIVE_NEW) {
            t_opt.clear();
            t_pool.clear();
            t_ref.clear();
            bool r1 = opt_book.process_aggressive_order(ev.order, t_opt);
            bool r2 = pool_book.process_aggressive_order(ev.order, t_pool);
            bool r3 = ref_book.process_aggressive_order(ev.order, t_ref);
            if (r1 != r3 || r2 != r3 || t_opt.size() != t_ref.size() || t_pool.size() != t_ref.size()) {
                std::cerr << "Sanity check failed at op " << i << "\n";
                std::abort();
            }
            total_trades += t_opt.size();
            for (size_t k = 0; k < t_ref.size(); ++k) {
                if (t_opt[k].Price != t_ref[k].Price || t_opt[k].Quantity != t_ref[k].Quantity ||
                    t_pool[k].Price != t_ref[k].Price || t_pool[k].Quantity != t_ref[k].Quantity) {
                    std::cerr << "Trade mismatch at op " << i << "\n";
                    std::abort();
                }
                total_shares += t_ref[k].Quantity;
            }
        } else if (ev.type == BenchOpType::CANCEL) {
            bool r1 = opt_book.cancel(ev.order.Order_id);
            bool r2 = pool_book.cancel(ev.order.Order_id);
            bool r3 = ref_book.cancel(ev.order.Order_id);
            if (r1 != r3 || r2 != r3) {
                std::cerr << "Cancel mismatch at op " << i << "\n";
                std::abort();
            }
        } else {
            bool r1 = opt_book.reduce(ev.order.Order_id, ev.reduce_qty);
            bool r2 = pool_book.reduce(ev.order.Order_id, ev.reduce_qty);
            bool r3 = ref_book.reduce(ev.order.Order_id, ev.reduce_qty);
            if (r1 != r3 || r2 != r3) {
                std::cerr << "Reduce mismatch at op " << i << "\n";
                std::abort();
            }
        }
    }

    std::cout << "3-Way Sanity Check (First " << limit << " ops vs ReferenceBook): PASSED ("
              << total_trades << " trades, " << total_shares
              << " shares matched, resting depth = " << ref_book.resting_order_count() << ")\n";
}

// ============================================================================
// Templated Benchmark Runner (Warm-up + 3 Runs, Median Selected)
// ============================================================================
struct CategoryStats {
    size_t count = 0;
    uint64_t p50_ns = 0;
    uint64_t p99_ns = 0;
    uint64_t p999_ns = 0;
    uint64_t max_ns = 0;
    double allocs_per_op = 0.0;
};

struct BookRunResult {
    CategoryStats matched;
    CategoryStats rested;
    CategoryStats cancel_hit;
    CategoryStats cancel_miss;
    CategoryStats reduce_hit;
    CategoryStats reduce_miss;

    double throughput_mops = 0.0;
    double cross_pct = 0.0;
    uint64_t total_trades = 0;
    uint64_t total_shares = 0;
    std::vector<size_t> depth_trajectory; // Checkpoints at 100k, 200k, 300k, 400k, 500k
    bool invariants_ok = false;
};

static CategoryStats compute_category(std::vector<uint64_t>& cycles, uint64_t total_allocs, double tsc_ghz) {
    CategoryStats s{};
    s.count = cycles.size();
    if (s.count == 0) return s;

    std::sort(cycles.begin(), cycles.end());
    auto to_ns = [tsc_ghz](uint64_t c) -> uint64_t {
        return static_cast<uint64_t>(static_cast<double>(c) / tsc_ghz + 0.5);
    };

    s.p50_ns = to_ns(cycles[s.count * 50 / 100]);
    s.p99_ns = to_ns(cycles[s.count * 99 / 100]);
    s.p999_ns = to_ns(cycles[s.count * 999 / 1000]);
    s.max_ns = to_ns(cycles.back());
    s.allocs_per_op = static_cast<double>(total_allocs) / static_cast<double>(s.count);
    return s;
}

template <typename BookType>
static BookRunResult execute_single_run(const std::vector<BenchEvent>& stream, double tsc_ghz) {
    const size_t num_ops = stream.size();

    std::vector<uint64_t> cyc_matched, cyc_rested, cyc_cancel_hit, cyc_cancel_miss, cyc_reduce_hit, cyc_reduce_miss;
    cyc_matched.reserve(num_ops / 4);
    cyc_rested.reserve(num_ops / 3);
    cyc_cancel_hit.reserve(num_ops / 4);
    cyc_cancel_miss.reserve(num_ops / 3);
    cyc_reduce_hit.reserve(num_ops / 10);
    cyc_reduce_miss.reserve(num_ops / 10);

    uint64_t alloc_matched = 0, alloc_rested = 0;
    uint64_t alloc_cancel_hit = 0, alloc_cancel_miss = 0;
    uint64_t alloc_reduce_hit = 0, alloc_reduce_miss = 0;

    auto book_ptr = std::make_unique<BookType>();
    BookType& book = *book_ptr;

    std::vector<Trade> trades;
    trades.reserve(256);

    // Track exact resting depth outside timed intervals
    std::vector<uint32_t> resting_qty(num_ops + 2, 0);
    size_t exact_depth = 0;
    std::vector<size_t> trajectory;
    trajectory.reserve(5);

    uint64_t total_new = 0, matched_new = 0;
    uint64_t total_trades = 0, total_shares = 0;

    auto wall_start = std::chrono::steady_clock::now();

    for (size_t i = 0; i < num_ops; ++i) {
        const auto& ev = stream[i];
        if (ev.type == BenchOpType::AGGRESSIVE_NEW) {
            trades.clear();
            uint64_t a0 = g_alloc_count;
            uint64_t c0 = read_tsc();
            bool ok = book.process_aggressive_order(ev.order, trades);
            uint64_t c1 = read_tsc();
            uint64_t a1 = g_alloc_count;

            if (!ok) {
                std::cerr << "Fatal: process_aggressive_order returned false (pool exhausted?) at op " << i << "\n";
                std::abort();
            }

            uint64_t dc = (c1 >= c0) ? (c1 - c0) : 0;
            ++total_new;

            if (!trades.empty()) {
                ++matched_new;
                cyc_matched.push_back(dc);
                alloc_matched += (a1 - a0);
            } else {
                cyc_rested.push_back(dc);
                alloc_rested += (a1 - a0);
            }

            uint32_t rem = ev.order.Quantity;
            total_trades += trades.size();
            for (const auto& tr : trades) {
                rem -= tr.Quantity;
                total_shares += tr.Quantity;
                uint64_t passive_id = (ev.order.side == Side::buy) ? tr.Seller_id : tr.Buyer_id;
                if (resting_qty[passive_id] > tr.Quantity) {
                    resting_qty[passive_id] -= tr.Quantity;
                } else if (resting_qty[passive_id] > 0) {
                    resting_qty[passive_id] = 0;
                    --exact_depth;
                }
            }
            if (rem > 0) {
                resting_qty[ev.order.Order_id] = rem;
                ++exact_depth;
            }
        } else if (ev.type == BenchOpType::CANCEL) {
            uint64_t a0 = g_alloc_count;
            uint64_t c0 = read_tsc();
            bool hit = book.cancel(ev.order.Order_id);
            uint64_t c1 = read_tsc();
            uint64_t a1 = g_alloc_count;

            uint64_t dc = (c1 >= c0) ? (c1 - c0) : 0;
            if (hit) {
                cyc_cancel_hit.push_back(dc);
                alloc_cancel_hit += (a1 - a0);
                if (resting_qty[ev.order.Order_id] > 0) {
                    resting_qty[ev.order.Order_id] = 0;
                    --exact_depth;
                }
            } else {
                cyc_cancel_miss.push_back(dc);
                alloc_cancel_miss += (a1 - a0);
            }
        } else {
            uint64_t a0 = g_alloc_count;
            uint64_t c0 = read_tsc();
            bool hit = book.reduce(ev.order.Order_id, ev.reduce_qty);
            uint64_t c1 = read_tsc();
            uint64_t a1 = g_alloc_count;

            uint64_t dc = (c1 >= c0) ? (c1 - c0) : 0;
            if (hit) {
                cyc_reduce_hit.push_back(dc);
                alloc_reduce_hit += (a1 - a0);
                if (resting_qty[ev.order.Order_id] > 0) {
                    if (resting_qty[ev.order.Order_id] > ev.reduce_qty) {
                        resting_qty[ev.order.Order_id] -= ev.reduce_qty;
                    } else {
                        resting_qty[ev.order.Order_id] = 0;
                        --exact_depth;
                    }
                }
            } else {
                cyc_reduce_miss.push_back(dc);
                alloc_reduce_miss += (a1 - a0);
            }
        }

        if ((i + 1) % 100'000 == 0) {
            trajectory.push_back(exact_depth);
        }
    }

    auto wall_end = std::chrono::steady_clock::now();
    double elapsed_s = std::chrono::duration<double>(wall_end - wall_start).count();

    BookRunResult res{};
    res.matched = compute_category(cyc_matched, alloc_matched, tsc_ghz);
    res.rested = compute_category(cyc_rested, alloc_rested, tsc_ghz);
    res.cancel_hit = compute_category(cyc_cancel_hit, alloc_cancel_hit, tsc_ghz);
    res.cancel_miss = compute_category(cyc_cancel_miss, alloc_cancel_miss, tsc_ghz);
    res.reduce_hit = compute_category(cyc_reduce_hit, alloc_reduce_hit, tsc_ghz);
    res.reduce_miss = compute_category(cyc_reduce_miss, alloc_reduce_miss, tsc_ghz);

    res.throughput_mops = (static_cast<double>(num_ops) / elapsed_s) / 1e6;
    res.cross_pct = 100.0 * static_cast<double>(matched_new) / static_cast<double>(total_new);
    res.total_trades = total_trades;
    res.total_shares = total_shares;
    res.depth_trajectory = std::move(trajectory);
    res.invariants_ok = (book.check_invariants() == InvariantError::none);
    return res;
}

template <typename BookType>
static BookRunResult benchmark_book_median(const std::string& label, const std::vector<BenchEvent>& stream, double tsc_ghz) {
    std::cout << "Running " << label << " (1 warm-up + 3 measured runs)...\n";
    // Warm-up run
    (void)execute_single_run<BookType>(stream, tsc_ghz);

    std::vector<BookRunResult> runs;
    runs.reserve(3);
    for (int r = 0; r < 3; ++r) {
        runs.push_back(execute_single_run<BookType>(stream, tsc_ghz));
        std::cout << "  Run " << (r + 1) << ": " << std::fixed << std::setprecision(2)
                  << runs.back().throughput_mops << " M ops/sec (Rested p50="
                  << runs.back().rested.p50_ns << "ns, p99=" << runs.back().rested.p99_ns << "ns)\n";
    }

    // Sort by throughput and select the median run (index 1)
    std::sort(runs.begin(), runs.end(), [](const BookRunResult& a, const BookRunResult& b) {
        return a.throughput_mops < b.throughput_mops;
    });
    return runs[1];
}

static void print_row(const std::string& name, const CategoryStats& s) {
    std::cout << std::left << std::setw(16) << name
              << std::right << std::setw(9) << s.count
              << std::setw(10) << s.p50_ns
              << std::setw(10) << s.p99_ns
              << std::setw(12) << s.p999_ns
              << std::setw(12) << s.max_ns
              << std::setw(12) << std::fixed << std::setprecision(2) << s.allocs_per_op << "\n";
}

static void print_book_table(const std::string& title, const BookRunResult& r) {
    std::cout << "\n=== " << title << " (Median of 3 Runs, 500000 Ops) ===\n";
    std::cout << std::left << std::setw(16) << "Op Category"
              << std::right << std::setw(9) << "Count"
              << std::setw(10) << "p50(ns)"
              << std::setw(10) << "p99(ns)"
              << std::setw(12) << "p99.9(ns)"
              << std::setw(12) << "Max(ns)"
              << std::setw(12) << "Allocs/Op" << "\n";
    std::cout << std::string(81, '-') << "\n";

    print_row("Matched", r.matched);
    print_row("Rested", r.rested);
    print_row("Cancel (Hit)", r.cancel_hit);
    print_row("Cancel (Miss)", r.cancel_miss);
    print_row("Reduce (Hit)", r.reduce_hit);
    print_row("Reduce (Miss)", r.reduce_miss);

    std::cout << std::string(81, '-') << "\n";
    std::cout << "Instrumented Throughput: " << std::fixed << std::setprecision(2) << r.throughput_mops << " M ops/sec\n";
    std::cout << "Actual Cross %:          " << std::fixed << std::setprecision(1) << r.cross_pct << "% of new orders matched\n";
    std::cout << "Trades / Shares Matched: " << r.total_trades << " trades / " << r.total_shares << " shares\n";
    std::cout << "Depth Trajectory (100k..500k): ";
    for (size_t i = 0; i < r.depth_trajectory.size(); ++i) {
        std::cout << r.depth_trajectory[i] << (i + 1 < r.depth_trajectory.size() ? " -> " : "\n");
    }
    std::cout << "Invariant Check:         " << (r.invariants_ok ? "PASSED" : "FAILED") << "\n";
}

int main() {
    pin_current_thread();

    double tsc_ghz = calibrate_tsc_ghz();
    TimerFloor tf = measure_tsc_floor(tsc_ghz);
    std::cout << "Calibrated TSC Frequency: " << std::fixed << std::setprecision(3) << tsc_ghz << " GHz\n";
    std::cout << "Timer Floor (back-to-back __rdtsc): p50 = " << tf.p50_cycles << " cycles ("
              << std::fixed << std::setprecision(1) << tf.p50_ns << " ns) | mean = " << tf.mean_ns << " ns\n";

    constexpr size_t NUM_OPS = 500'000;
    constexpr uint64_t SEED = 1337;

    std::cout << "Generating " << NUM_OPS << " deterministic random-walk operations (Seed=" << SEED << ")...\n";
    auto stream = generate_stream(NUM_OPS, SEED);

    verify_stream_prefix(stream, 100'000);
    std::cout << "\n";

    BookRunResult base_res = benchmark_book_median<Order_book>("Baseline Order_book (STL std::list)", stream, tsc_ghz);
    BookRunResult pool_res = benchmark_book_median<PooledOrderBook<16384>>("PooledOrderBook<16384> (Intrusive Slab Pool)", stream, tsc_ghz);

    if (base_res.total_trades != pool_res.total_trades ||
        base_res.total_shares != pool_res.total_shares ||
        base_res.depth_trajectory != pool_res.depth_trajectory) {
        std::cerr << "ERROR: Baseline and PooledOrderBook diverged on benchmark stream!\n";
        return 1;
    }

    print_book_table("Baseline Order_book (STL std::list)", base_res);
    print_book_table("PooledOrderBook<16384> (Intrusive Slab Pool)", pool_res);

    std::cout << "\n=== Exact Cross-Book Equivalence Check (500000 Ops) ===\n";
    std::cout << "Trades Executed: " << base_res.total_trades << " (Baseline) == " << pool_res.total_trades << " (Pooled)\n";
    std::cout << "Shares Matched:  " << base_res.total_shares << " (Baseline) == " << pool_res.total_shares << " (Pooled)\n";
    std::cout << "Rested Allocs:   " << std::fixed << std::setprecision(2) << base_res.rested.allocs_per_op
              << " allocs/op (Baseline) -> " << pool_res.rested.allocs_per_op << " allocs/op (Pooled)\n";

    return 0;
}