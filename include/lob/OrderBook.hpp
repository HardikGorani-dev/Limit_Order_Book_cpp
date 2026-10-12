#pragma once

#include <cstdint>
#include <map>
#include <list>
#include <unordered_map>
#include <vector>
#include <optional>
#include <functional>

enum class Side {
    buy,sell
};

struct Order{
    uint64_t Order_id;
    uint64_t Price;
    uint32_t Quantity;
    Side side;
};

struct Trade{
    uint64_t Buyer_id;
    uint64_t Seller_id;
    uint64_t Price;
    uint32_t Quantity;
};

struct IndexEntry{
    Side side;
    uint64_t Price;
    std::list<Order>::iterator list_iterator;

};
enum class InvariantError {
    none = 0,
    empty_level,
    zero_quantity,
    price_mismatch,
    side_mismatch,
    missing_from_index,
    index_data_mismatch,
    iterator_mismatch,
    size_mismatch
};

class Order_book{
    private:
    std::map<uint64_t, std::list<Order>, std::greater<uint64_t>> bids; 
    std::map<uint64_t, std::list<Order>, std::less<uint64_t>> asks;
    
    std::unordered_map<uint64_t,IndexEntry> order_index;
    public:
    Order_book() = default;
    Order_book(const Order_book&) = delete;
    Order_book& operator=(const Order_book&) = delete;

    bool insert(Order order);
    bool cancel(uint64_t order_id);
    bool reduce(uint64_t order_id, uint32_t qty_to_reduce);

    bool process_aggressive_order(Order order, std::vector<Trade>& out_trades);

    std::optional<uint64_t> best_bid() const;
    std::optional<uint64_t> best_ask() const;
    bool contains(uint64_t id) const;
    uint32_t remaining_qty(uint64_t id) const;
    uint64_t total_quantity(Side side,uint64_t price) const;


    InvariantError check_invariants() const;



};
