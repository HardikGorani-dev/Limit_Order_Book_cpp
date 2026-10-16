#include <iostream>
#include <vector>
#include <deque>
#include <random>
#include <string>
#include <cassert>
#include <lob/OrderBook.hpp>
#include <lob/ReferenceBook.hpp>

enum class OpType {INSERT, CANCEL, REDUCE, AGGRESSIVE};

struct OpRecord {
    OpType type;
    uint64_t id;
    uint64_t Price;
    uint32_t Quantity;
    Side side;
};

std::string side_to_str(Side s){ return s == Side::buy ? "BUY" : "SELL";}
std::string op_to_str(OpType t) {
    if (t == OpType::INSERT) return "INSERT    ";
    if (t == OpType::CANCEL) return "CANCEL    ";
    if (t == OpType::REDUCE) return "REDUCE    ";
    return "AGGRESSIVE";
}

void dump_history(const std::deque<OpRecord>& history, uint64_t seed) {
    std::cerr << "\n========== FUZZ DIVERGENCE ==========\n";
    std::cerr << "Seed: " << seed << "\n\n";
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
    
    std::random_device rd;
    uint64_t seed = rd();
    std::mt19937_64 gen(seed);
    
    std::cout << "Starting Differential Fuzz\n";
    std::cout << "Ops: " << num_ops << " | Seed: " << seed << "\n";

    std::uniform_int_distribution<int> op_dist(0, 99);
    std::uniform_int_distribution<uint64_t> id_dist(1, 48); // 48 unique IDs
    std::uniform_int_distribution<uint64_t> price_dist(100, 110); // 11 Price levels
    std::uniform_int_distribution<uint32_t> qty_dist(0, 100); // Includes 0 for edge cases
    std::uniform_int_distribution<int> side_dist(0, 1);

    Order_book opt_book;
    ReferenceBook ref_book;
    std::deque<OpRecord> history;

    // Coverage Counters
    uint64_t stats_inserts = 0, stats_cancels = 0, stats_reduces = 0;
    uint64_t stats_aggressive = 0, stats_trades = 0, stats_zero_qty_rejects = 0;

    for (uint64_t i = 0; i < num_ops; ++i) {
        int r = op_dist(gen);
        OpType type;
        if (r < 40) type = OpType::INSERT;
        else if (r < 80) type = OpType::AGGRESSIVE;
        else if (r < 90) type = OpType::CANCEL;
        else type = OpType::REDUCE;

        OpRecord op = {type, id_dist(gen), price_dist(gen), qty_dist(gen), 
                       side_dist(gen) == 0 ? Side::buy : Side::sell};

        history.push_back(op);
        if (history.size() > 50) history.pop_front();

        bool opt_res = false, ref_res = false;
        std::vector<Trade> opt_trades, ref_trades;

        if (type == OpType::INSERT) {
            opt_res = opt_book.insert({op.id, op.Price, op.Quantity, op.side});
            ref_res = ref_book.insert({op.id, op.Price, op.Quantity, op.side});
            if (opt_res) stats_inserts++;
            if (op.Quantity == 0 && !opt_res) stats_zero_qty_rejects++;
        } 
        else if (type == OpType::CANCEL) {
            opt_res = opt_book.cancel(op.id);
            ref_res = ref_book.cancel(op.id);
            if (opt_res) stats_cancels++;
        } 
        else if (type == OpType::REDUCE) {
            opt_res = opt_book.reduce(op.id, op.Quantity);
            ref_res = ref_book.reduce(op.id, op.Quantity);
            if (opt_res) stats_reduces++;
        } 
        else if (type == OpType::AGGRESSIVE) {
            opt_res = opt_book.process_aggressive_order({op.id, op.Price, op.Quantity, op.side}, opt_trades);
            ref_res = ref_book.process_aggressive_order({op.id, op.Price, op.Quantity, op.side}, ref_trades);
            if (opt_res) stats_aggressive++;
            stats_trades += opt_trades.size();
        }

        // --- DIFFERENTIAL CHECKS ---
        if (opt_res != ref_res) {
            std::cerr << "\n[DIVERGENCE] Return values differ on op " << i << "\n";
            dump_history(history, seed);
            std::abort();
        }

        if (type == OpType::AGGRESSIVE) {
            if (opt_trades.size() != ref_trades.size()) {
                std::cerr << "\n[DIVERGENCE] Trade counts differ on op " << i << "\n";
                dump_history(history, seed);
                std::abort();
            }
            for (size_t t = 0; t < opt_trades.size(); ++t) {
                if (opt_trades[t].Buyer_id != ref_trades[t].Buyer_id ||
                    opt_trades[t].Seller_id != ref_trades[t].Seller_id ||
                    opt_trades[t].Price != ref_trades[t].Price ||
                    opt_trades[t].Quantity != ref_trades[t].Quantity) {
                    std::cerr << "\n[DIVERGENCE] Trade properties differ on op " << i << " at trade index " << t << "\n";
                    dump_history(history, seed);
                    std::abort();
                }
            }
        }

        // Invariant check periodicity
        if (i > 0 && i % 1000 == 0) {
            if (opt_book.check_invariants() != InvariantError::none) {
                std::cerr << "\n[INVARIANT FAILURE] Failed at op " << i << "\n";
                dump_history(history, seed);
                std::abort();
            }
        }
    }

    std::cout << "\n[SUCCESS] 10M Operations Fuzzed with Zero Divergence\n";
    std::cout << "--- Coverage Stats ---\n";
    std::cout << "Inserts: " << stats_inserts << "\n";
    std::cout << "Cancels: " << stats_cancels << "\n";
    std::cout << "Reduces: " << stats_reduces << "\n";
    std::cout << "Aggressive: " << stats_aggressive << "\n";
    std::cout << "Trades Executed: " << stats_trades << "\n";
    std::cout << "Zero-Qty Rejects: " << stats_zero_qty_rejects << "\n";

    return 0;
}