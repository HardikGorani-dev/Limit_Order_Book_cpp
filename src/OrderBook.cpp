#include <lob/OrderBook.hpp>
#include <algorithm>
#include <iterator>
#include <cassert>

bool Order_book::insert(Order order){
    if(order.Quantity == 0){ return false;}
    if(order_index.find(order.Order_id) != order_index.end()){ return false;}
    
    if(order.side == Side::buy){
        std::list<Order>& queue = bids[order.Price];
        queue.push_back(order);
        order_index[order.Order_id] = {Side::buy, order.Price, std::prev(queue.end())};

    }else {
        std::list<Order>& queue = asks[order.Price];
        queue.push_back(order);
        order_index[order.Order_id] = {Side::sell, order.Price, std::prev(queue.end())};
    }
    return true;
}

bool Order_book::cancel(uint64_t order_id) {
    auto it = order_index.find(order_id);
    if(it == order_index.end()) {return false;}

    IndexEntry entry = it->second;
    if (entry.side == Side::buy) {
        auto level_it = bids.find(entry.Price);
        assert(level_it != bids.end() && "Level missing in index"); 
        
        level_it->second.erase(entry.list_iterator);
        if(level_it->second.empty()) bids.erase(level_it);
    } else {
        auto level_it = asks.find(entry.Price);
        assert(level_it != asks.end() && "Level missing in index"); 
        
        level_it->second.erase(entry.list_iterator);
        if(level_it->second.empty()) asks.erase(level_it);
    }
    order_index.erase(it);
    return true;
}

bool Order_book::reduce(uint64_t order_id, uint32_t qty_to_reduce) {
    if(qty_to_reduce == 0 ){ return false;}

    auto it = order_index.find(order_id);
    if (it == order_index.end()) return false;

    IndexEntry entry = it->second;
    Order& order = *(entry.list_iterator);

    if(qty_to_reduce > order.Quantity){
        return false;

    }else if(qty_to_reduce == order.Quantity){
        return cancel(order_id);

    }else{
        order.Quantity -= qty_to_reduce;
        return true;
    }

}



bool Order_book::process_aggressive_order(Order order, std::vector<Trade>& out_trade) {
    if(order.Quantity == 0) return false;
    if(order_index.find(order.Order_id) != order_index.end()) return false;

    if (order.side == Side::buy) {
        while (!asks.empty() && order.Quantity > 0 && asks.begin()->first <= order.Price) {
            auto best_ask = asks.begin();
            std::list<Order>& sellers = best_ask->second;
            Order& best_seller = sellers.front();
            
            uint32_t traded_qty = std::min(order.Quantity, best_seller.Quantity);

            out_trade.push_back({order.Order_id, best_seller.Order_id, best_ask->first, traded_qty});
            order.Quantity -= traded_qty;
            best_seller.Quantity -= traded_qty;

            if (best_seller.Quantity == 0) {
                order_index.erase(best_seller.Order_id); 
                sellers.pop_front();
            }
            if (sellers.empty()) asks.erase(best_ask);
        }
    } 
    else if (order.side == Side::sell) {
        while (!bids.empty() && bids.begin()->first >= order.Price && order.Quantity > 0) {
            auto best_bid = bids.begin();
            std::list<Order>& buyers = best_bid->second;
            Order& best_buyer = buyers.front();

            uint32_t traded_qty = std::min(order.Quantity, best_buyer.Quantity);

            out_trade.push_back({best_buyer.Order_id, order.Order_id, best_bid->first, traded_qty});
            order.Quantity -= traded_qty;
            best_buyer.Quantity -= traded_qty;

            if (best_buyer.Quantity == 0) {
                order_index.erase(best_buyer.Order_id); 
                buyers.pop_front();
            }
            if (buyers.empty()) bids.erase(best_bid);
        }
    }

   if (order.Quantity > 0) {
        bool success = insert(order); 
        assert(success && "Leftover insert must not fail after validation"); 
        (void)success; 
    
    } 
    return true;
}


std::optional<uint64_t> Order_book::best_bid() const{
        return bids.empty() ? std::nullopt : std::make_optional(bids.begin()->first);
}
std::optional<uint64_t> Order_book::best_ask() const{
        return asks.empty() ? std::nullopt : std::make_optional(asks.begin()->first);
}

bool Order_book::contains(uint64_t id) const{
    return order_index.find(id) != order_index.end();
}

uint32_t Order_book::remaining_qty(uint64_t id) const {
    auto it = order_index.find(id);
    return it != order_index.end() ? it->second.list_iterator->Quantity : 0;
}

uint64_t Order_book::total_quantity(Side side, uint64_t price) const {
    uint64_t total = 0;
    if (side == Side::buy) {
        auto it = bids.find(price);
        if (it != bids.end()) {
            for (const auto& o : it->second) total += o.Quantity;
        }
    } else {
        auto it = asks.find(price);
        if (it != asks.end()) {
            for (const auto& o : it->second) total += o.Quantity;
        }
    }
    return total;
}

InvariantError Order_book::check_invariants() const {
    size_t order_count = 0;

    auto check_level = [&](const auto& levels_map, Side expected_side) {
        for (const auto& [price, level] : levels_map) {
            if (level.empty()) return InvariantError::empty_level; 
            for (const auto& order : level) {
                if (order.Quantity == 0) return InvariantError::zero_quantity; 
                if (order.Price != price) return InvariantError::price_mismatch;
                if (order.side != expected_side) return InvariantError::side_mismatch;

                auto idx_it = order_index.find(order.Order_id);
                if (idx_it == order_index.end()) return InvariantError::missing_from_index; 

                const IndexEntry& entry = idx_it->second;
                if (entry.Price != price || entry.side != expected_side) return InvariantError::index_data_mismatch;
                if (&*(entry.list_iterator) != &order) return InvariantError::iterator_mismatch;

                order_count++;
            }
        }
        return InvariantError::none; 
    };

    if (auto err = check_level(bids, Side::buy);err != InvariantError::none) return err;
    if (auto err = check_level(asks, Side::sell);err != InvariantError::none) return err;
    if (order_index.size() != order_count) return InvariantError::size_mismatch;

    return InvariantError::none;
}