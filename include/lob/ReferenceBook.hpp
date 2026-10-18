#pragma once
#include "OrderBook.hpp"
#include <algorithm>
#include <vector>
#include <cstddef>
#include <cstdint>

class ReferenceBook {
private:

struct RefOrder {
    uint64_t id;
    uint64_t Price;
    uint32_t Quantity;
    Side side;
    uint64_t seq;
};
std::vector<RefOrder> active_orders;
uint64_t global_seq = 0;

public:
   bool insert(Order order){
        if( order.Quantity == 0 ){ return false;}
        for( const auto& o :active_orders){
            if(o.id == order.Order_id){return false;}
        }
        active_orders.push_back({order.Order_id,order.Price,order.Quantity,order.side, ++global_seq});
        return true;
   }
   bool cancel(uint64_t id) {
        auto it = std::remove_if(active_orders.begin(), active_orders.end(), 
            [id](const RefOrder& o) { return o.id == id; });
        if (it == active_orders.end()) return false;
        active_orders.erase(it, active_orders.end());
        return true;
    }

    bool reduce(uint64_t id, uint32_t decrease_by) {
        if (decrease_by == 0) return false;
        for (auto it = active_orders.begin(); it != active_orders.end(); ++it) {
            if (it->id == id) {
                if (decrease_by> it->Quantity){
                    return false;  // over Reduce is strictly rejected
                }
                if (decrease_by == it->Quantity) {
                    active_orders.erase(it); // Exact Reduce removes the order
                } else {
                    it->Quantity -= decrease_by;
                }
                return true;
            }
        }
        return false;
    }

    bool process_aggressive_order(Order order, std::vector<Trade>& out_trade) {
        if (order.Quantity == 0) return false;
        for (const auto& o : active_orders) {
            if (o.id == order.Order_id) return false;
        }

        while (order.Quantity > 0) {
            auto best_it = active_orders.end();
            
            // Linear scan for the absolute best Price, breaking ties by sequence number
            for (auto it = active_orders.begin(); it != active_orders.end(); ++it) {
                if (it->side == order.side) continue;
                if (order.side == Side::buy && it->Price > order.Price) continue;
                if (order.side == Side::sell && it->Price < order.Price) continue;

                if (best_it == active_orders.end()) {
                    best_it = it;
                } else {
                    bool better_price = (order.side == Side::buy) ? (it->Price < best_it->Price) : (it->Price > best_it->Price);
                    if (better_price) {
                        best_it = it;
                    } else if (it->Price == best_it->Price && it->seq < best_it->seq) {
                        best_it = it; // FIFO tie-breaker
                    }
                }
            }

            if (best_it == active_orders.end()) break; // No more matches

            uint32_t traded_qty = std::min(order.Quantity, best_it->Quantity);
            uint64_t buyer = (order.side == Side::buy) ? order.Order_id : best_it->id;
            uint64_t seller = (order.side == Side::sell) ? order.Order_id : best_it->id;

            out_trade.push_back({buyer, seller, best_it->Price, traded_qty});

            order.Quantity -= traded_qty;
            best_it->Quantity -= traded_qty;

            if (best_it->Quantity == 0) {
                active_orders.erase(best_it);
            }
        }

        if (order.Quantity > 0) {
            active_orders.push_back({order.Order_id, order.Price, order.Quantity, order.side, ++global_seq});
        }
        return true;
    }

    InvariantError check_invariants() const { return InvariantError::none; }

    size_t size() const { return active_orders.size();}

    bool has_id(uint64_t id) const {
        for (const auto& o : active_orders) {
            if (o.id == id) return true;
        }
        return false;
    }

    uint64_t id_at(size_t index) const { return active_orders[index].id; }

    // Returns true if `id` is resting and is NOT the front (oldest seq) order at its price level
    bool is_non_front(uint64_t id) const {
        const RefOrder* target = nullptr;
        for (const auto& o : active_orders) {
            if (o.id == id) {
                target = &o;
                break;
            }
        }
        if (!target) return false;

        for (const auto& o : active_orders) {
            if (o.side == target->side && o.Price == target->Price && o.seq < target->seq) {
                return true;
            }
        }
        return false;
    }
};










