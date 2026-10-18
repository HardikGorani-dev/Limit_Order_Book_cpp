#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <new>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#endif

#include <lob/OrderBook.hpp>
#include <lob/ReferenceBook.hpp>

// --- Global Heap Allocation Counter ---
static uint64_t g_alloc_count = 0;

void* operator new(std::size_t size) {
    ++g_alloc_count;
    if (void* p = std::malloc(size)) return p;
    throw std::bad_alloc();
}

void operator delete(void* p) noexcept {
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}

static void pin_current_thread() {
#ifdef _WIN32
    SetThreadAffinityMask(GetCurrentThread(), 1ULL << 0);
    std::cout << "Thread Affinity: Pinned to Core 0 (Windows SetThreadAffinityMask)\n";
#elif defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
    if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) == 0) {
        std::cout << "Thread Affinity: Pinned to Core 0 (Linux sched_setaffinity)\n";
    }
#endif
}
struct TimerStats {
    uint64_t p50_ns;
    double mean_ns;
    uint64_t min_nonzero_ns;
};

static TimerStats measure_timer_floor() {
    constexpr size_t N = 100'000;
    std::vector<uint64_t> deltas(N);
    uint64_t sum = 0;
    uint64_t min_nz = UINT64_MAX;
    for (size_t i = 0; i < N; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        auto t1 = std::chrono::steady_clock::now();
        uint64_t d = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        deltas[i] = d;
        sum += d;
        if (d > 0 && d < min_nz) min_nz = d;
    }
    std::sort(deltas.begin(), deltas.end());
    if (min_nz == UINT64_MAX) min_nz = 0;
    return {deltas[N / 2], static_cast<double>(sum) / static_cast<double>(N), min_nz};
}

enum class BenchOpType : uint8_t { AGGRESSIVE_NEW, CANCEL, REDUCE };

struct BenchEvent {
    BenchOpType type;
    Order order;
    uint32_t reduce_qty;
};

static std::vector<BenchEvent> generate_stream(size_t num_ops, uint64_t seed) {
    std::mt19937_64 gen(seed);
    std::uniform_int_distribution<int> pct(0, 99);
    std::uniform_int_distribution<int> step_dist(-1, 1);
    std::uniform_int_distribution<uint64_t> passive_offset(1, 15);
    std::uniform_int_distribution<uint64_t> cross_offset(0, 5);
    std::uniform_int_distribution<uint32_t> qty_dist(10, 100);
    std::uniform_int_distribution<uint32_t> reduce_dist(1, 9);

    std::vector<BenchEvent> stream;
    stream.reserve(num_ops);

    std::vector<uint64_t> active_ids;
    active_ids.reserve(4096);

    uint64_t mid = 10'000;
    uint64_t next_id = 1;

    for (size_t i = 0; i < num_ops; ++i) {
        mid = static_cast<uint64_t>(
            std::clamp<int64_t>(static_cast<int64_t>(mid) + step_dist(gen), 2'000, 50'000));

        int r = pct(gen);
        // Keep new resting orders (~42%) and cancels (~43%) balanced so book depth stays bounded
        if (active_ids.size() < 200 || (r < 55 && active_ids.size() < 2500)) {
            Side side = (pct(gen) < 50) ? Side::buy : Side::sell;
            bool crosses = (pct(gen) < 25); // Stated ~25% crossing target
            uint64_t price = 0;
            if (side == Side::buy) {
                price = crosses ? (mid + cross_offset(gen)) : (mid - passive_offset(gen));
            } else {
                price = crosses ? (mid - cross_offset(gen)) : (mid + passive_offset(gen));
            }
            uint64_t id = next_id++;
            if (!crosses) {
                active_ids.push_back(id);
            }
            stream.push_back({BenchOpType::AGGRESSIVE_NEW, {id, price, qty_dist(gen), side}, 0});
        } else if (r < 90 && !active_ids.empty()) {
            std::uniform_int_distribution<size_t> idx_dist(0, active_ids.size() - 1);
            size_t idx = idx_dist(gen);
            uint64_t target_id = active_ids[idx];
            active_ids[idx] = active_ids.back();
            active_ids.pop_back();
            stream.push_back({BenchOpType::CANCEL, {target_id, 0, 0, Side::buy}, 0});
        } else if (!active_ids.empty()) {
            std::uniform_int_distribution<size_t> idx_dist(0, active_ids.size() - 1);
            uint64_t target_id = active_ids[idx_dist(gen)];
            stream.push_back({BenchOpType::REDUCE, {target_id, 0, 0, Side::buy}, reduce_dist(gen)});
        }
    }
    return stream;
}

static void verify_stream_prefix(const std::vector<BenchEvent>& stream, size_t prefix_len) {
    Order_book opt_book;
    ReferenceBook ref_book;
    std::vector<Trade> opt_trades, ref_trades;
    opt_trades.reserve(64);
    ref_trades.reserve(64);

    uint64_t opt_total_trades = 0, ref_total_trades = 0;
    uint64_t opt_total_qty = 0, ref_total_qty = 0;

    size_t limit = std::min(prefix_len, stream.size());
    for (size_t i = 0; i < limit; ++i) {
        const auto& ev = stream[i];
        if (ev.type == BenchOpType::AGGRESSIVE_NEW) {
            opt_trades.clear();
            ref_trades.clear();
            opt_book.process_aggressive_order(ev.order, opt_trades);
            ref_book.process_aggressive_order(ev.order, ref_trades);
            opt_total_trades += opt_trades.size();
            ref_total_trades += ref_trades.size();
            for (const auto& t : opt_trades) opt_total_qty += t.Quantity;
            for (const auto& t : ref_trades) ref_total_qty += t.Quantity;
        } else if (ev.type == BenchOpType::CANCEL) {
            opt_book.cancel(ev.order.Order_id);
            ref_book.cancel(ev.order.Order_id);
        } else {
            opt_book.reduce(ev.order.Order_id, ev.reduce_qty);
            ref_book.reduce(ev.order.Order_id, ev.reduce_qty);
        }
    }

    if (opt_total_trades != ref_total_trades || opt_total_qty != ref_total_qty) {
        std::cerr << "[ERROR] Stream sanity check failed against ReferenceBook!\n";
        std::abort();
    }
    std::cout << "Sanity Check (First " << limit << " ops vs ReferenceBook): PASSED ("
              << opt_total_trades << " trades, " << opt_total_qty << " shares matched, "
              << "resting depth = " << ref_book.size() << ")\n";
}

static void print_stats(const std::string& label, std::vector<uint64_t>& samples, uint64_t allocs) {
    if (samples.empty()) return;
    std::sort(samples.begin(), samples.end());
    size_t n = samples.size();
    uint64_t p50 = samples[n * 50 / 100];
    uint64_t p99 = samples[n * 99 / 100];
    uint64_t p999 = samples[n * 999 / 1000];
    uint64_t max_v = samples.back();
    double allocs_per_op = static_cast<double>(allocs) / static_cast<double>(n);

    std::cout << std::left << std::setw(14) << label
              << std::right << std::setw(10) << n
              << std::setw(10) << p50
              << std::setw(10) << p99
              << std::setw(12) << p999
              << std::setw(12) << max_v
              << std::setw(12) << std::fixed << std::setprecision(2) << allocs_per_op << "\n";
}

int main() {
    pin_current_thread();

    TimerStats tf = measure_timer_floor();
    std::cout << "Timer Floor (steady_clock::now): p50 = " << tf.p50_ns
              << " ns | mean = " << std::fixed << std::setprecision(1) << tf.mean_ns
              << " ns | min non-zero tick = " << tf.min_nonzero_ns << " ns\n";

    constexpr size_t NUM_OPS = 500'000;
    constexpr uint64_t SEED = 1337;

    std::cout << "Generating " << NUM_OPS << " realistic random-walk operations...\n";
    auto stream = generate_stream(NUM_OPS, SEED);

    verify_stream_prefix(stream, 100'000);

    std::vector<uint64_t> lat_matched, lat_rested, lat_cancel, lat_reduce;
    lat_matched.reserve(NUM_OPS);
    lat_rested.reserve(NUM_OPS);
    lat_cancel.reserve(NUM_OPS);
    lat_reduce.reserve(NUM_OPS);

    uint64_t alloc_matched = 0, alloc_rested = 0, alloc_cancel = 0, alloc_reduce = 0;

    Order_book book;
    std::vector<Trade> trades;
    trades.reserve(256);

    // Track exact resting quantities outside timed regions for true final depth
    std::vector<uint32_t> resting_qty(NUM_OPS + 2, 0);
    size_t exact_depth = 0;
    uint64_t total_new_orders = 0;
    uint64_t matched_orders = 0;

    auto bench_start = std::chrono::steady_clock::now();

    for (const auto& ev : stream) {
        if (ev.type == BenchOpType::AGGRESSIVE_NEW) {
            trades.clear();
            uint64_t a0 = g_alloc_count;
            auto t0 = std::chrono::steady_clock::now();
            book.process_aggressive_order(ev.order, trades);
            auto t1 = std::chrono::steady_clock::now();
            uint64_t a1 = g_alloc_count;

            uint64_t ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

            ++total_new_orders;
            if (!trades.empty()) {
                ++matched_orders;
                lat_matched.push_back(ns);
                alloc_matched += (a1 - a0);
            } else {
                lat_rested.push_back(ns);
                alloc_rested += (a1 - a0);
            }

            uint32_t rem = ev.order.Quantity;
            for (const auto& tr : trades) {
                rem -= tr.Quantity;
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
            auto t0 = std::chrono::steady_clock::now();
            bool ok = book.cancel(ev.order.Order_id);
            auto t1 = std::chrono::steady_clock::now();
            uint64_t a1 = g_alloc_count;

            if (ok && resting_qty[ev.order.Order_id] > 0) {
                resting_qty[ev.order.Order_id] = 0;
                --exact_depth;
            }
            uint64_t ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            lat_cancel.push_back(ns);
            alloc_cancel += (a1 - a0);
        } else {
            uint64_t a0 = g_alloc_count;
            auto t0 = std::chrono::steady_clock::now();
            bool ok = book.reduce(ev.order.Order_id, ev.reduce_qty);
            auto t1 = std::chrono::steady_clock::now();
            uint64_t a1 = g_alloc_count;

            if (ok && resting_qty[ev.order.Order_id] > 0) {
                if (resting_qty[ev.order.Order_id] > ev.reduce_qty) {
                    resting_qty[ev.order.Order_id] -= ev.reduce_qty;
                } else {
                    resting_qty[ev.order.Order_id] = 0;
                    --exact_depth;
                }
            }
            uint64_t ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            lat_reduce.push_back(ns);
            alloc_reduce += (a1 - a0);
        }
    }

    auto bench_end = std::chrono::steady_clock::now();
    double elapsed_s = std::chrono::duration<double>(bench_end - bench_start).count();
    double throughput = static_cast<double>(NUM_OPS) / elapsed_s;
    double match_rate = 100.0 * static_cast<double>(matched_orders) / static_cast<double>(total_new_orders);

    std::cout << "\n--- Benchmark Results (" << NUM_OPS << " Ops) ---\n";
    std::cout << std::left << std::setw(14) << "Op Category"
              << std::right << std::setw(10) << "Count"
              << std::setw(10) << "p50(ns)"
              << std::setw(10) << "p99(ns)"
              << std::setw(12) << "p99.9(ns)"
              << std::setw(12) << "Max(ns)"
              << std::setw(12) << "Allocs/Op" << "\n";
    std::cout << std::string(80, '-') << "\n";

    print_stats("Matched", lat_matched, alloc_matched);
    print_stats("Rested", lat_rested, alloc_rested);
    print_stats("Cancel", lat_cancel, alloc_cancel);
    print_stats("Reduce", lat_reduce, alloc_reduce);

    std::cout << std::string(80, '-') << "\n";
    std::cout << "Throughput:       " << std::fixed << std::setprecision(2) << (throughput / 1e6) << " M ops/sec\n";
    std::cout << "Actual Cross %:   " << std::fixed << std::setprecision(1) << match_rate << "% of new orders matched\n";
    std::cout << "Final Book Depth: " << exact_depth << " resting orders\n";
    std::cout << "Invariant Check:  " << (book.check_invariants() == InvariantError::none ? "PASSED" : "FAILED") << "\n";

    return 0;
}