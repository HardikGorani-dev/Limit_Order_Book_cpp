#pragma once

#include "MemoryPool.hpp"
#include "OrderBook.hpp"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

/// @brief Intrusive doubly-linked list node stored inside MemoryPool slots.
/// Layout: 24-byte Order + 4-byte prev + 4-byte next = 32 bytes (2 nodes per 64B cache line).
struct PooledNode {
    Order order;
    uint32_t prev;
    uint32_t next;
};

/// @brief Price level holding 32-bit pool handles to the FIFO queue head and tail.
struct PooledLevel {
    uint32_t head = MemoryPool<PooledNode, 1>::INVALID_INDEX;
    uint32_t tail = MemoryPool<PooledNode, 1>::INVALID_INDEX;
};

/// @brief Price-time priority Limit Order Book backed by a fixed-capacity intrusive MemoryPool.
template <size_t PoolCapacity = 16384>
class PooledOrderBook {
public:
    static constexpr uint32_t INVALID_HANDLE = MemoryPool<PooledNode, PoolCapacity>::INVALID_INDEX;

private:
    MemoryPool<PooledNode, PoolCapacity> node_pool;
    std::map<uint64_t, PooledLevel, std::greater<uint64_t>> bids;
    std::map<uint64_t, PooledLevel, std::less<uint64_t>> asks;
    std::unordered_map<uint64_t, uint32_t> order_index; // Maps Order_id -> 32-bit pool handle

    // --- Intrusive Doubly-Linked List Helpers ---
    void push_back_to_level(PooledLevel& level, uint32_t handle) noexcept {
        PooledNode* node = node_pool.at(handle);
        node->prev = level.tail;
        node->next = INVALID_HANDLE;

        if (level.tail != INVALID_HANDLE) {
            node_pool.at(level.tail)->next = handle;
        } else {
            level.head = handle;
        }
        level.tail = handle;
    }

    void unlink_from_level(PooledLevel& level, uint32_t handle) noexcept {
        PooledNode* node = node_pool.at(handle);
        uint32_t prev_h = node->prev;
        uint32_t next_h = node->next;

        if (prev_h != INVALID_HANDLE) {
            node_pool.at(prev_h)->next = next_h;
        } else {
            level.head = next_h;
        }

        if (next_h != INVALID_HANDLE) {
            node_pool.at(next_h)->prev = prev_h;
        } else {
            level.tail = prev_h;
        }

        node->prev = INVALID_HANDLE;
        node->next = INVALID_HANDLE;
    }

public:
    PooledOrderBook() = default;
    PooledOrderBook(const PooledOrderBook&) = delete;
    PooledOrderBook& operator=(const PooledOrderBook&) = delete;
    PooledOrderBook(PooledOrderBook&&) = delete;
    PooledOrderBook& operator=(PooledOrderBook&&) = delete;

    /// @brief Passively inserts a resting limit order without matching.
    /// @return true if inserted, false if rejected (zero quantity, duplicate ID, or pool exhausted).
    bool insert(Order order) {
        if (order.Quantity == 0) return false;
        if (order_index.find(order.Order_id) != order_index.end()) return false;

        PooledNode* node = node_pool.allocate(order, INVALID_HANDLE, INVALID_HANDLE);
        if (node == nullptr) {
            return false; // Pool exhausted
        }

        uint32_t handle = node_pool.index_of(node);
        PooledLevel& level = (order.side == Side::buy) ? bids[order.Price] : asks[order.Price];
        push_back_to_level(level, handle);
        order_index.emplace(order.Order_id, handle);
        return true;
    }

    /// @brief Cancels a resting order by ID, unlinks it in O(1), returns its slot to the pool,
    ///        and removes its price level if empty.
    bool cancel(uint64_t order_id) {
        auto idx_it = order_index.find(order_id);
        if (idx_it == order_index.end()) return false;

        uint32_t handle = idx_it->second;
        PooledNode* node = node_pool.at(handle);
        uint64_t price = node->order.Price;
        Side side = node->order.side;

        if (side == Side::buy) {
            auto lvl_it = bids.find(price);
            assert(lvl_it != bids.end());
            unlink_from_level(lvl_it->second, handle);
            if (lvl_it->second.head == INVALID_HANDLE) {
                bids.erase(lvl_it);
            }
        } else {
            auto lvl_it = asks.find(price);
            assert(lvl_it != asks.end());
            unlink_from_level(lvl_it->second, handle);
            if (lvl_it->second.head == INVALID_HANDLE) {
                asks.erase(lvl_it);
            }
        }

        order_index.erase(idx_it);
        node_pool.free(node);
        return true;
    }

    /// @brief Reduces the quantity of a resting order in-place without losing FIFO queue priority.
    bool reduce(uint64_t order_id, uint32_t decrease_by) {
        if (decrease_by == 0) return false;

        auto idx_it = order_index.find(order_id);
        if (idx_it == order_index.end()) return false;

        PooledNode* node = node_pool.at(idx_it->second);
        if (decrease_by > node->order.Quantity) {
            return false;
        }
        if (decrease_by == node->order.Quantity) {
            return cancel(order_id);
        }

        node->order.Quantity -= decrease_by;
        return true;
    }

    /// @brief Matches an incoming aggressive order in price-time (FIFO) priority up to its limit price.
    /// @note If the order has an unfilled remainder after matching, it is inserted into the pool.
    ///       If the pool is already full BEFORE matching starts and the order would rest, we pre-check
    ///       or return false on exhaustion.
    bool process_aggressive_order(Order order, std::vector<Trade>& out_trade) {
        if (order.Quantity == 0) return false;
        if (order_index.find(order.Order_id) != order_index.end()) return false;

        if (order.side == Side::buy) {
            while (order.Quantity > 0 && !asks.empty()) {
                auto best_ask_it = asks.begin();
                if (best_ask_it->first > order.Price) break;

                PooledLevel& level = best_ask_it->second;
                while (order.Quantity > 0 && level.head != INVALID_HANDLE) {
                    uint32_t head_h = level.head;
                    PooledNode* resting = node_pool.at(head_h);

                    uint32_t traded_qty = std::min(order.Quantity, resting->order.Quantity);
                    out_trade.push_back({order.Order_id, resting->order.Order_id,
                                         resting->order.Price, traded_qty});

                    order.Quantity -= traded_qty;
                    resting->order.Quantity -= traded_qty;

                    if (resting->order.Quantity == 0) {
                        uint64_t resting_id = resting->order.Order_id;
                        unlink_from_level(level, head_h);
                        order_index.erase(resting_id);
                        node_pool.free(resting);
                    }
                }

                if (level.head == INVALID_HANDLE) {
                    asks.erase(best_ask_it);
                }
            }
        } else {
            while (order.Quantity > 0 && !bids.empty()) {
                auto best_bid_it = bids.begin();
                if (best_bid_it->first < order.Price) break;

                PooledLevel& level = best_bid_it->second;
                while (order.Quantity > 0 && level.head != INVALID_HANDLE) {
                    uint32_t head_h = level.head;
                    PooledNode* resting = node_pool.at(head_h);

                    uint32_t traded_qty = std::min(order.Quantity, resting->order.Quantity);
                    out_trade.push_back({resting->order.Order_id, order.Order_id,
                                         resting->order.Price, traded_qty});

                    order.Quantity -= traded_qty;
                    resting->order.Quantity -= traded_qty;

                    if (resting->order.Quantity == 0) {
                        uint64_t resting_id = resting->order.Order_id;
                        unlink_from_level(level, head_h);
                        order_index.erase(resting_id);
                        node_pool.free(resting);
                    }
                }

                if (level.head == INVALID_HANDLE) {
                    bids.erase(best_bid_it);
                }
            }
        }

        if (order.Quantity > 0) {
            return insert(order);
        }
        return true;
    }

    std::optional<uint64_t> best_bid() const {
        if (bids.empty()) return std::nullopt;
        return bids.begin()->first;
    }

    std::optional<uint64_t> best_ask() const {
        if (asks.empty()) return std::nullopt;
        return asks.begin()->first;
    }

    bool contains(uint64_t id) const {
        return order_index.find(id) != order_index.end();
    }

    uint32_t remaining_qty(uint64_t id) const {
        auto it = order_index.find(id);
        if (it == order_index.end()) return 0;
        return node_pool.at(it->second)->order.Quantity;
    }

    uint64_t total_quantity(Side side, uint64_t price) const {
        const PooledLevel* lvl = nullptr;
        if (side == Side::buy) {
            auto it = bids.find(price);
            if (it == bids.end()) return 0;
            lvl = &it->second;
        } else {
            auto it = asks.find(price);
            if (it == asks.end()) return 0;
            lvl = &it->second;
        }

        uint64_t sum = 0;
        uint32_t curr = lvl->head;
        while (curr != INVALID_HANDLE) {
            const PooledNode* node = node_pool.at(curr);
            sum += node->order.Quantity;
            curr = node->next;
        }
        return sum;
    }

    InvariantError check_invariants() const {
        size_t counted_orders = 0;

        auto verify_side = [&](const auto& book_side, Side expected_side) -> InvariantError {
            for (const auto& [level_price, level] : book_side) {
                if (level.head == INVALID_HANDLE || level.tail == INVALID_HANDLE) {
                    return InvariantError::empty_level;
                }
                if (node_pool.at(level.head)->prev != INVALID_HANDLE) {
                    return InvariantError::link_mismatch;
                }
                if (node_pool.at(level.tail)->next != INVALID_HANDLE) {
                    return InvariantError::link_mismatch;
                }

                uint32_t prev_h = INVALID_HANDLE;
                uint32_t curr_h = level.head;

                while (curr_h != INVALID_HANDLE) {
                    if (curr_h >= PoolCapacity) {
                        return InvariantError::link_mismatch;
                    }
                    const PooledNode* node = node_pool.at(curr_h);

                    if (node->prev != prev_h) {
                        return InvariantError::link_mismatch;
                    }
                    if (node->next == INVALID_HANDLE && curr_h != level.tail) {
                        return InvariantError::link_mismatch;
                    }
                    if (node->next != INVALID_HANDLE && curr_h == level.tail) {
                        return InvariantError::link_mismatch;
                    }

                    if (node->order.Quantity == 0) {
                        return InvariantError::zero_quantity;
                    }
                    if (node->order.Price != level_price) {
                        return InvariantError::price_mismatch;
                    }
                    if (node->order.side != expected_side) {
                        return InvariantError::side_mismatch;
                    }

                    auto idx_it = order_index.find(node->order.Order_id);
                    if (idx_it == order_index.end()) {
                        return InvariantError::missing_from_index;
                    }
                    if (idx_it->second != curr_h) {
                        return InvariantError::iterator_mismatch;
                    }

                    ++counted_orders;
                    prev_h = curr_h;
                    curr_h = node->next;
                }
            }
            return InvariantError::none;
        };

        InvariantError err = verify_side(bids, Side::buy);
        if (err != InvariantError::none) return err;

        err = verify_side(asks, Side::sell);
        if (err != InvariantError::none) return err;

        if (counted_orders != order_index.size()) {
            return InvariantError::size_mismatch;
        }
        if (node_pool.live_count() != order_index.size()) {
            return InvariantError::pool_live_count_mismatch;
        }

        return InvariantError::none;
    }
};