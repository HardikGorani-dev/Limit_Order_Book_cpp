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

/// @brief Passively inserts a resting limit order without matching.
    /// @pre order.Quantity > 0 and order.Order_id is not currently in the book.
    /// @note Does not check for a crossed book; use process_aggressive_order for matching flow.
    /// @return true if inserted, false if rejected (duplicate ID or zero quantity).
    bool insert(Order order);

    /// @brief Cancels a resting order by ID and removes its price level if empty.
    /// @return true if found and removed, false if order_id does not exist.
    bool cancel(uint64_t order_id);

    /// @brief Reduces the quantity of a resting order in-place without losing FIFO queue priority.
    /// @pre decrease_by > 0 and decrease_by <= current resting quantity.
    /// @post If decrease_by == current resting quantity, the order is removed like a cancel.
    /// @return true if reduced/removed, false if unknown ID, decrease_by == 0, or decrease_by > current quantity.
    bool reduce(uint64_t order_id, uint32_t decrease_by);

    /// @brief Matches an incoming aggressive order in price-time (FIFO) priority up to its limit price.
    /// @pre order.Quantity > 0 and order.Order_id is not currently in the book.
    /// @post Executed fills are appended to out_trade at the resting order's price; any unfilled remainder rests at order.Price.
    /// @return true if accepted, false if rejected (duplicate ID or zero quantity).
    bool process_aggressive_order(Order order, std::vector<Trade>& out_trade);

    std::optional<uint64_t> best_bid() const;
    std::optional<uint64_t> best_ask() const;
    bool contains(uint64_t id) const;
    uint32_t remaining_qty(uint64_t id) const;
    uint64_t total_quantity(Side side,uint64_t price) const;


    InvariantError check_invariants() const;



};
