#include <iostream>
#include <vector>
#include <chrono>
#include <algorithm>
#include <random>
#include "lob/OrderBook.hpp"

enum class OpType { Insert, Cancel };
struct BenchmarkOp {
    OpType type;
    Order order;
};

int main() {
    constexpr size_t WARMUP_OPS = 100'000;
    constexpr size_t BENCHMARK_OPS = 1'000'000;
    
    std::mt19937_64 rng(1337);
    std::uniform_int_distribution<uint64_t> price_dist(95, 105);
    std::uniform_int_distribution<uint32_t> qty_dist(1, 100);
    std::uniform_int_distribution<int> op_dist(1, 100);

    std::vector<BenchmarkOp> stream;
    stream.reserve(WARMUP_OPS + BENCHMARK_OPS);

    uint64_t next_id = 1;
    std::vector<uint64_t> live_ids;

    // Realistic distribution: 70% Limit Adds, 30% Cancels
    for (size_t i = 0; i < WARMUP_OPS + BENCHMARK_OPS; ++i) {
        int roll = op_dist(rng);
        if (roll <= 70 || live_ids.empty()) {
            Side side = (roll % 2 == 0) ? Side::buy : Side::sell;
            Order ord{next_id, price_dist(rng), qty_dist(rng), side};
            stream.push_back({OpType::Insert, ord});
            live_ids.push_back(next_id++);
        } else {
            size_t idx = rng() % live_ids.size();
            uint64_t cancel_id = live_ids[idx];
            live_ids[idx] = live_ids.back();
            live_ids.pop_back();
            stream.push_back({OpType::Cancel, Order{cancel_id, 0, 0, Side::buy}});
        }
    }

    Order_book book;
    std::vector<Trade> trades;
    trades.reserve(16);

    // Warm-up phase: prime instruction and data caches
    for (size_t i = 0; i < WARMUP_OPS; ++i) {
        const auto& op = stream[i];
        if (op.type == OpType::Insert) book.insert(op.order);
        else book.cancel(op.order.Order_id);
    }

    // Measurement phase
    std::vector<uint64_t> latencies_ns;
    latencies_ns.reserve(BENCHMARK_OPS);

    for (size_t i = WARMUP_OPS; i < WARMUP_OPS + BENCHMARK_OPS; ++i) {
        const auto& op = stream[i];
        auto start = std::chrono::steady_clock::now();

        if (op.type == OpType::Insert) {
            book.insert(op.order);
        } else {
            book.cancel(op.order.Order_id);
        }

        auto end = std::chrono::steady_clock::now();
        latencies_ns.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    }

    std::sort(latencies_ns.begin(), latencies_ns.end());

    std::cout << "--- Baseline Latency Results (" << BENCHMARK_OPS << " ops) ---\n";
    std::cout << "p50:   " << latencies_ns[BENCHMARK_OPS * 0.50] << " ns\n";
    std::cout << "p99:   " << latencies_ns[BENCHMARK_OPS * 0.99] << " ns\n";
    std::cout << "p99.9: " << latencies_ns[BENCHMARK_OPS * 0.999] << " ns\n";

    return 0;
}