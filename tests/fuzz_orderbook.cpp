#include <iostream>
#include <vector>
#include <deque>
#include <random>
#include <string>
#include <cassert>
#include <cstdlib>
#include <lob/OrderBook.hpp>
#include <lob/ReferenceBook.hpp>
#include <lob/PooledOrderBook.hpp>

enum class OpType {INSERT, CANCEL, REDUCE, AGGRESSIVE};

struct OpRecord {
    OpType type;
    uint64_t id;
    uint64_t Price;
    uint32_t Quantity;
    Side side;
};

static std::string side_to_str(Side s) { return s == Side::buy ? "BUY " : "SELL"; }
static std::string op_to_str(OpType t) {
    if (t == OpType::INSERT) return "INSERT    ";
    if (t == OpType::CANCEL) return "CANCEL    ";
    if (t == OpType::REDUCE) return "REDUCE    ";
    return "AGGRESSIVE";
}

static void dump_history(const std::deque<OpRecord>& history, uint64_t seed) {
    std::cerr << "\n========== FUZZ DIVERGENCE ==========\n";
    std::cerr << "Replay seed: " << seed << "\n\n";
    std::cerr << "Last 50 Operations:\n";
    for (const auto& op : history) {
        std::cerr << op_to_str(op.type) << " | ID: " << op.id
                  << " | Px: " << op.Price << " | Qty: " << op.Quantity
                  << " | " << side_to_str(op.side) << "\n";
    }
    std::cerr << "=====================================\n";
}
int main(int argc, char** argv) {
    uint64_t num_ops = (argc > 1) ? std::stoull(argv[1]) : 10'000'000;
    uint64_t seed = (argc > 2) ? std::stoull(argv[2]) : static_cast<uint64_t>(std::random_device{}());

    std::mt19937_64 gen(seed);

    std::cout << "Starting Differential Fuzz\n";
    std::cout << "Ops: " << num_ops << " | Seed: " << seed << "\n";

    std::uniform_int_distribution<int> pct_dist(0, 99);
    std::uniform_int_distribution<uint64_t> price_dist(100, 110); // 11 Price levels
    std::uniform_int_distribution<uint32_t> qty_dist(1, 100);
    std::uniform_int_distribution<uint32_t> sweep_qty_dist(120, 350);
    std::uniform_int_distribution<int> side_dist(0, 1);

    Order_book opt_book;
    ReferenceBook ref_book;
    PooledOrderBook<4096> pool_book;
    std::deque<OpRecord> history;

    uint64_t next_id = 1;

    // Coverage Counters
    uint64_t stats_inserts = 0;
    uint64_t stats_cancels = 0;
    uint64_t stats_reduces = 0;
    uint64_t stats_aggressive = 0;
    uint64_t stats_trades = 0;
    uint64_t stats_partial_fills = 0;
    uint64_t stats_multi_level_sweeps = 0;
    uint64_t stats_duplicate_rejects = 0;
    uint64_t stats_rejected_reduces = 0;
    uint64_t stats_non_front_cancels = 0;
    uint64_t stats_zero_qty_rejects = 0;

    for (uint64_t i = 0; i < num_ops; ++i) {
        int r = pct_dist(gen);
        OpType type;
        if (r < 25) type = OpType::INSERT;
        else if (r < 65) type = OpType::AGGRESSIVE;
        else if (r < 85) type = OpType::CANCEL;
        else type = OpType::REDUCE;

        // Keep active depth bounded around ~80 orders so queues stay 5-15 deep per level
        if (type == OpType::INSERT && ref_book.size() > 100) {
            type = OpType::CANCEL;
        }

        uint64_t id = 0;
        uint64_t Price = price_dist(gen);
        uint32_t Quantity = qty_dist(gen);
        Side side = (side_dist(gen) == 0) ? Side::buy : Side::sell;

        if (type == OpType::INSERT || type == OpType::AGGRESSIVE) {
            // 90% fresh ID (accepted), 10% existing ID (triggers duplicate_rejects)
            if (ref_book.size() > 0 && pct_dist(gen) < 10) {
                std::uniform_int_distribution<size_t> idx_dist(0, ref_book.size() - 1);
                id = ref_book.id_at(idx_dist(gen));
            } else {
                id = next_id++;
            }

            // 3% zero-quantity probe, 20% large sweep probe on aggressive orders
            int q_roll = pct_dist(gen);
            if (q_roll < 3) {
                Quantity = 0;
            } else if (type == OpType::AGGRESSIVE && q_roll < 25) {
                Quantity = sweep_qty_dist(gen);
            }
        } else {
            // CANCEL or REDUCE: 85% target an existing resting order, 15% unknown ID
            if (ref_book.size() > 0 && pct_dist(gen) < 85) {
                std::uniform_int_distribution<size_t> idx_dist(0, ref_book.size() - 1);
                id = ref_book.id_at(idx_dist(gen));
            } else {
                id = next_id + 1000;
            }
            if (type == OpType::REDUCE && pct_dist(gen) < 10) {
                Quantity = 0;
            }
        }

        OpRecord op = {type, id, Price, Quantity, side};
        history.push_back(op);
        if (history.size() > 50) history.pop_front();

      bool opt_res = false, pool_res = false, ref_res = false;
     std::vector<Trade> opt_trades, pool_trades, ref_trades;

     if (type == OpType::INSERT) {
         bool had_id = ref_book.has_id(op.id);
         opt_res = opt_book.insert({op.id, op.Price, op.Quantity, op.side});
         pool_res = pool_book.insert({op.id, op.Price, op.Quantity, op.side});
         ref_res = ref_book.insert({op.id, op.Price, op.Quantity, op.side});
         if (opt_res) stats_inserts++;
         else if (op.Quantity == 0) stats_zero_qty_rejects++;
         else if (had_id) stats_duplicate_rejects++;
     } else if (type == OpType::CANCEL) {
         bool was_non_front = ref_book.is_non_front(op.id);
         opt_res = opt_book.cancel(op.id);
         pool_res = pool_book.cancel(op.id);
         ref_res = ref_book.cancel(op.id);
         if (opt_res) {
             stats_cancels++;
             if (was_non_front) stats_non_front_cancels++;
         }
     } else if (type == OpType::REDUCE) {
         opt_res = opt_book.reduce(op.id, op.Quantity);
         pool_res = pool_book.reduce(op.id, op.Quantity);
         ref_res = ref_book.reduce(op.id, op.Quantity);
         if (opt_res) stats_reduces++;
         else stats_rejected_reduces++;
     } else if (type == OpType::AGGRESSIVE) {
         bool had_id = ref_book.has_id(op.id);
         opt_res = opt_book.process_aggressive_order({op.id, op.Price, op.Quantity, op.side}, opt_trades);
         pool_res = pool_book.process_aggressive_order({op.id, op.Price, op.Quantity, op.side}, pool_trades);
         ref_res = ref_book.process_aggressive_order({op.id, op.Price, op.Quantity, op.side}, ref_trades);
         if (opt_res) {
             stats_aggressive++;
             stats_trades += opt_trades.size();
             if (!opt_trades.empty()) {
                 uint64_t total_traded = 0;
                 for (const auto& tr : opt_trades) total_traded += tr.Quantity;
                 uint64_t last_resting_id = (op.side == Side::buy)
                     ? opt_trades.back().Seller_id
                     : opt_trades.back().Buyer_id;
                 if (total_traded < op.Quantity || ref_book.has_id(last_resting_id)) {
                     stats_partial_fills++;
                 }
                 if (opt_trades.size() >= 2 && opt_trades.front().Price != opt_trades.back().Price) {
                     stats_multi_level_sweeps++;
                 }
             }
         } else if (op.Quantity == 0) {
             stats_zero_qty_rejects++;
         } else if (had_id) {
             stats_duplicate_rejects++;
         }
     }

     if (opt_res != ref_res || pool_res != ref_res) {
         std::cerr << "\n[DIVERGENCE] Return values differ on op " << i << "\n";
         dump_history(history, seed);
         std::abort();
     }

     if (type == OpType::AGGRESSIVE) {
         if (opt_trades.size() != ref_trades.size() || pool_trades.size() != ref_trades.size()) {
             std::cerr << "\n[DIVERGENCE] Trade counts differ on op " << i << "\n";
             dump_history(history, seed);
             std::abort();
         }
         for (size_t t = 0; t < ref_trades.size(); ++t) {
             if (opt_trades[t].Buyer_id != ref_trades[t].Buyer_id ||
                 opt_trades[t].Seller_id != ref_trades[t].Seller_id ||
                 opt_trades[t].Price != ref_trades[t].Price ||
                 opt_trades[t].Quantity != ref_trades[t].Quantity ||
                 pool_trades[t].Buyer_id != ref_trades[t].Buyer_id ||
                 pool_trades[t].Seller_id != ref_trades[t].Seller_id ||
                 pool_trades[t].Price != ref_trades[t].Price ||
                 pool_trades[t].Quantity != ref_trades[t].Quantity) {
                 std::cerr << "\n[DIVERGENCE] Trade properties differ on op " << i << " at index " << t << "\n";
                 dump_history(history, seed);
                 std::abort();
             }
         }
     }

     if (num_ops <= 100'000 || (i > 0 && i % 1000 == 0)) {
         if (opt_book.check_invariants() != InvariantError::none ||
             pool_book.check_invariants() != InvariantError::none) {
             std::cerr << "\n[INVARIANT FAILURE] Failed at op " << i << "\n";
             dump_history(history, seed);
             std::abort();
         }
     }
    }

    std::cout << "\n[SUCCESS] " << num_ops << " Operations Fuzzed with Zero Divergence\n";
    std::cout << "--- Coverage Stats ---\n";
    std::cout << "Inserts:            " << stats_inserts << "\n";
    std::cout << "Aggressive:         " << stats_aggressive << "\n";
    std::cout << "Cancels:            " << stats_cancels << "\n";
    std::cout << "Non-Front Cancels:  " << stats_non_front_cancels << "\n";
    std::cout << "Reduces:            " << stats_reduces << "\n";
    std::cout << "Rejected Reduces:   " << stats_rejected_reduces << "\n";
    std::cout << "Trades Executed:    " << stats_trades << "\n";
    std::cout << "Partial Fills:      " << stats_partial_fills << "\n";
    std::cout << "Multi-Level Sweeps: " << stats_multi_level_sweeps << "\n";
    std::cout << "Duplicate Rejects:  " << stats_duplicate_rejects << "\n";
    std::cout << "Zero-Qty Rejects:   " << stats_zero_qty_rejects << "\n";

    if (stats_inserts == 0 || stats_aggressive == 0 || stats_cancels == 0 ||
        stats_non_front_cancels == 0 || stats_reduces == 0 || stats_rejected_reduces == 0 ||
        stats_trades == 0 || stats_partial_fills == 0 || stats_multi_level_sweeps == 0 ||
        stats_duplicate_rejects == 0 || stats_zero_qty_rejects == 0) {
        std::cerr << "[ERROR] One or more coverage counters are zero!\n";
        return 1;
    }

    return 0;
}