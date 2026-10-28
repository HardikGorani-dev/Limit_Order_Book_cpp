#include <gtest/gtest.h>
#include <lob/OrderBook.hpp>
#include <lob/ReferenceBook.hpp>
#include <lob/PooledOrderBook.hpp>

template<typename T>
class OrderBookTest : public ::testing::Test {
protected:
    T book;
    std::vector<Trade> trades;

    void TearDown() override {
        EXPECT_EQ(this->book.check_invariants(), InvariantError::none);
        this->trades.clear();
    }
};

using BookTypes = ::testing::Types<Order_book, ReferenceBook, PooledOrderBook<4096>>;
TYPED_TEST_SUITE(OrderBookTest,BookTypes);

// --- MATCHING RULES & PRICE PRIORITY ---

TYPED_TEST(OrderBookTest, ExactFill_TradePriceIsRestingPrice) {
    this->book.insert({1, 100, 50, Side::sell});
    this->book.process_aggressive_order({2, 100, 50, Side::buy}, this->trades);
    
    ASSERT_EQ(this->trades.size(), 1);
    EXPECT_EQ(this->trades[0].Buyer_id, 2);
    EXPECT_EQ(this->trades[0].Seller_id, 1);
    EXPECT_EQ(this->trades[0].Price, 100); // Must be resting Price
    EXPECT_EQ(this->trades[0].Quantity, 50);
}

TYPED_TEST(OrderBookTest, SellAggressor_IgnoresWorseLimits) {
    this->book.insert({1, 90, 50, Side::buy}); // Too low
    this->book.insert({2, 100, 50, Side::buy}); // Match
    
    this->book.process_aggressive_order({3, 95, 50, Side::sell}, this->trades);
    
    ASSERT_EQ(this->trades.size(), 1);
    EXPECT_EQ(this->trades[0].Buyer_id, 2);
    EXPECT_EQ(this->trades[0].Price, 100);
}

TYPED_TEST(OrderBookTest, PartialFill_RemainderRests) {
    this->book.insert({1, 100, 20, Side::sell});
    this->book.process_aggressive_order({2, 100, 50, Side::buy}, this->trades);
    
    ASSERT_EQ(this->trades.size(), 1);
    EXPECT_EQ(this->trades[0].Quantity, 20);
    
    // Aggressor remainder (30) must rest. A new sell should hit it.
    this->trades.clear();
    this->book.process_aggressive_order({3, 100, 30, Side::sell}, this->trades);
    ASSERT_EQ(this->trades.size(), 1);
    EXPECT_EQ(this->trades[0].Buyer_id, 2);
    EXPECT_EQ(this->trades[0].Quantity, 30);
}

TYPED_TEST(OrderBookTest, MultiLevelSweep_StopsAtLimit) {
    this->book.insert({1, 100, 10, Side::sell});
    this->book.insert({2, 101, 10, Side::sell});
    this->book.insert({3, 102, 10, Side::sell});
    
    this->book.process_aggressive_order({4, 101, 50, Side::buy}, this->trades); // Only sweeps 100 and 101
    
    ASSERT_EQ(this->trades.size(), 2);
    EXPECT_EQ(this->trades[0].Price, 100);
    EXPECT_EQ(this->trades[1].Price, 101);
}

// --- QUEUE PRIORITY (FIFO) ---

TYPED_TEST(OrderBookTest, FifoPriority_Maintained) {
    this->book.insert({1, 100, 10, Side::sell});
    this->book.insert({2, 100, 10, Side::sell});
    
    this->book.process_aggressive_order({3, 100, 10, Side::buy}, this->trades);
    
    ASSERT_EQ(this->trades.size(), 1);
    EXPECT_EQ(this->trades[0].Seller_id, 1); // Order 1 must fill first
}

TYPED_TEST(OrderBookTest, QueuePosition_KeptAfterPartialFill) {
    this->book.insert({1, 100, 20, Side::sell});
    this->book.insert({2, 100, 10, Side::sell});
    
    this->book.process_aggressive_order({3, 100, 10, Side::buy}, this->trades); // Partially fills Order 1
    this->trades.clear();
    
    this->book.process_aggressive_order({4, 100, 10, Side::buy}, this->trades); // Order 1 should still be first
    
    ASSERT_EQ(this->trades.size(), 1);
    EXPECT_EQ(this->trades[0].Seller_id, 1);
}

TYPED_TEST(OrderBookTest, QueuePosition_KeptAfterReduce) {
    this->book.insert({1, 100, 20, Side::sell});
    this->book.insert({2, 100, 10, Side::sell});
    
    this->book.reduce(1, 10); // Reduce Order 1
    this->book.process_aggressive_order({3, 100, 10, Side::buy}, this->trades);
    
    ASSERT_EQ(this->trades.size(), 1);
    EXPECT_EQ(this->trades[0].Seller_id, 1); // Order 1 must still be first
}

TYPED_TEST(OrderBookTest, QueuePosition_LostAfterCancelAndReadd) {
    this->book.insert({1, 100, 10, Side::sell});
    this->book.insert({2, 100, 10, Side::sell});
    
    this->book.cancel(1);
    this->book.insert({1, 100, 10, Side::sell});
    
    this->book.process_aggressive_order({3, 100, 10, Side::buy}, this->trades);
    
    ASSERT_EQ(this->trades.size(), 1);
    EXPECT_EQ(this->trades[0].Seller_id, 2); // Order 2 is now first
}

// --- VALIDATION & EDGE CASES ---

TYPED_TEST(OrderBookTest, RejectDuplicateIds) {
    EXPECT_TRUE(this->book.insert({1, 100, 10, Side::sell}));
    EXPECT_FALSE(this->book.insert({1, 101, 10, Side::buy}));
}

TYPED_TEST(OrderBookTest, RejectZeroQuantity) {
    EXPECT_FALSE(this->book.insert({1, 100, 0, Side::sell}));
    EXPECT_FALSE(this->book.process_aggressive_order({2, 100, 0, Side::buy}, this->trades));
}

TYPED_TEST(OrderBookTest, Cancel_UnknownId_Ignored) {
    this->book.insert({1, 100, 10, Side::sell});
    EXPECT_FALSE(this->book.cancel(999)); // Should return false, not crash
}

TYPED_TEST(OrderBookTest, Cancel_RemovesEmptyPriceLevel) {
    this->book.insert({1, 100, 10, Side::sell});
    this->book.cancel(1);
    // Invariants check in TearDown will fail if the empty level is still in the map
}

TYPED_TEST(OrderBookTest, Reduce_UnknownId_Ignored) {
    this->book.insert({1, 100, 10, Side::sell});
    EXPECT_FALSE(this->book.reduce(999, 5));
}

TYPED_TEST(OrderBookTest, Reduce_ZeroQty_Ignored) {
    this->book.insert({1, 100, 10, Side::sell});
    EXPECT_FALSE(this->book.reduce(1, 0));
}

TYPED_TEST(OrderBookTest, Reduce_OverQty_Rejected) {
    this->book.insert({1, 100, 10, Side::sell});
    
    // Over-reduce must be rejected and return false, leaving order intact
    EXPECT_FALSE(this->book.reduce(1, 15)); 
    
    // Validate the order is still resting and can be fully filled
    this->book.process_aggressive_order({2, 100, 10, Side::buy}, this->trades);
    ASSERT_EQ(this->trades.size(), 1);
    EXPECT_EQ(this->trades[0].Quantity, 10);
}



TYPED_TEST(OrderBookTest, QueuePosition_KeptAfterPartialFill_SellSide) {
    this->book.insert({1, 100, 20, Side::buy});
    this->book.insert({2, 100, 10, Side::buy});
    
    this->book.process_aggressive_order({3, 100, 10, Side::sell}, this->trades); // Fills 10 of Order 1
    this->trades.clear();
    
    this->book.process_aggressive_order({4, 100, 10, Side::sell}, this->trades); // Order 1 must still be first in line
    
    ASSERT_EQ(this->trades.size(), 1);
    EXPECT_EQ(this->trades[0].Buyer_id, 1);
}

TYPED_TEST(OrderBookTest, MultiLevelSweep_SellSide) {
    this->book.insert({1, 100, 10, Side::buy});
    this->book.insert({2, 99, 10, Side::buy});
    this->book.insert({3, 98, 10, Side::buy});
    
    this->book.process_aggressive_order({4, 99, 50, Side::sell}, this->trades); 
    
    ASSERT_EQ(this->trades.size(), 2);
    EXPECT_EQ(this->trades[0].Price, 100);
    EXPECT_EQ(this->trades[1].Price, 99); // Stops at 99 limit
}

TYPED_TEST(OrderBookTest, StrongerLimitPrice_RemainderRestsAtLimit) {
    this->book.insert({1, 100, 10, Side::buy}); 
    
   this->book.process_aggressive_order({2, 95, 50, Side::sell}, this->trades);
    ASSERT_EQ(this->trades.size(), 1);
    EXPECT_EQ(this->trades[0].Quantity, 10);
    EXPECT_EQ(this->trades[0].Price, 100);
    
    this->trades.clear();
    
     this->book.process_aggressive_order({3, 95, 40, Side::buy}, this->trades);
    ASSERT_EQ(this->trades.size(), 1);
    EXPECT_EQ(this->trades[0].Seller_id, 2);
    EXPECT_EQ(this->trades[0].Price, 95);
    EXPECT_EQ(this->trades[0].Quantity, 40);
}

TYPED_TEST(OrderBookTest, RejectDuplicateId_AggressiveOrder) {
    this->book.insert({1, 100, 10, Side::sell});
    
    EXPECT_FALSE(this->book.process_aggressive_order({1, 100, 10, Side::buy}, this->trades));
    EXPECT_TRUE(this->trades.empty());
}


TYPED_TEST(OrderBookTest, Cancel_BidSide) {
    this->book.insert({1, 100, 10, Side::buy});
    EXPECT_TRUE(this->book.cancel(1));
    
    this->book.process_aggressive_order({2, 100, 10, Side::sell}, this->trades);
    EXPECT_TRUE(this->trades.empty()); 
}

TYPED_TEST(OrderBookTest, FifoPriority_SellAggressor) {
    this->book.insert({1, 100, 10, Side::buy});
    this->book.insert({2, 100, 15, Side::buy});
    
    this->book.process_aggressive_order({3, 100, 15, Side::sell}, this->trades);
    
    ASSERT_EQ(this->trades.size(), 2);
    // Matches first-in (Order 1) fully, then takes 5 from Order 2
    EXPECT_EQ(this->trades[0].Buyer_id, 1);
    EXPECT_EQ(this->trades[0].Quantity, 10);
    EXPECT_EQ(this->trades[1].Buyer_id, 2);
    EXPECT_EQ(this->trades[1].Quantity, 5);
}

TYPED_TEST(OrderBookTest, Reduce_ExactQuantity_RemovesOrder) {
    this->book.insert({1, 100, 10, Side::sell});
    
    EXPECT_TRUE(this->book.reduce(1, 10)); 
    
    this->book.process_aggressive_order({2, 100, 10, Side::buy}, this->trades);
    EXPECT_TRUE(this->trades.empty()); 
}

TYPED_TEST(OrderBookTest, Reduce_Partial_UpdatesQuantity) {
    this->book.insert({1, 100, 20, Side::sell});
    
    EXPECT_TRUE(this->book.reduce(1, 5)); // Decrease BY 5, leaving 15
    
    this->book.process_aggressive_order({2, 100, 20, Side::buy}, this->trades);
    
    ASSERT_EQ(this->trades.size(), 1);
    EXPECT_EQ(this->trades[0].Quantity, 15); // The trade must be for exactly the remaining 15
}

TYPED_TEST(OrderBookTest, Cancel_MiddleOrder) {
    this->book.insert({1, 100, 10, Side::sell});
    this->book.insert({2, 100, 10, Side::sell}); // The target
    this->book.insert({3, 100, 10, Side::sell});
    
    EXPECT_TRUE(this->book.cancel(2)); 
    
    this->book.process_aggressive_order({4, 100, 20, Side::buy}, this->trades);
    
    ASSERT_EQ(this->trades.size(), 2);
    EXPECT_EQ(this->trades[0].Seller_id, 1);
    EXPECT_EQ(this->trades[1].Seller_id, 3);
}

TEST(OrderBookTestNonTyped, Cancel_RemovesEmptyBidPriceLevel) {
    Order_book book;
    EXPECT_TRUE(book.insert({1, 100, 10, Side::buy}));
    EXPECT_TRUE(book.insert({2, 99, 20, Side::buy}));
    ASSERT_EQ(book.best_bid(), 100ULL);

    EXPECT_TRUE(book.cancel(1));
    EXPECT_EQ(book.best_bid(), 99ULL);
    EXPECT_EQ(book.total_quantity(Side::buy, 100), 0ULL);

    EXPECT_TRUE(book.cancel(2));
    EXPECT_EQ(book.best_bid(), std::nullopt);
}

TEST(PooledOrderBookTestNonTyped, Cancel_RemovesEmptyBidPriceLevel) {
    PooledOrderBook<4096> book;
    EXPECT_TRUE(book.insert({1, 100, 10, Side::buy}));
    EXPECT_TRUE(book.insert({2, 99, 20, Side::buy}));
    ASSERT_EQ(book.best_bid(), 100ULL);

    EXPECT_TRUE(book.cancel(1));
    EXPECT_EQ(book.best_bid(), 99ULL);
    EXPECT_EQ(book.total_quantity(Side::buy, 100), 0ULL);

    EXPECT_TRUE(book.cancel(2));
    EXPECT_EQ(book.best_bid(), std::nullopt);
}
TEST(PooledOrderBookExhaustionTest, ReturnsFalseWhenPoolExhaustedAndRecoversAfterCancelOrTrade) {
    PooledOrderBook<3> book;
    EXPECT_TRUE(book.insert({1, 100, 10, Side::buy}));
    EXPECT_TRUE(book.insert({2, 100, 20, Side::buy}));
    EXPECT_TRUE(book.insert({3, 105, 15, Side::sell}));

    // Pool of capacity 3 is full: passive insert must return false
    EXPECT_FALSE(book.insert({4, 100, 15, Side::buy}));

    // Non-crossing aggressive order on a full pool (0 trades) cannot rest -> returns false
    std::vector<Trade> trades;
    EXPECT_FALSE(book.process_aggressive_order({5, 101, 25, Side::buy}, trades));
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.check_invariants(), InvariantError::none);

    // Crossing aggressive order on a full pool that consumes order #3 (qty 15 at 105)
    // frees slot #3 during the trade, so its 10-share remainder rests at 105
    EXPECT_TRUE(book.process_aggressive_order({6, 105, 25, Side::buy}, trades));
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].Quantity, 15u);
    EXPECT_TRUE(book.contains(6));
    EXPECT_EQ(book.remaining_qty(6), 10u);
    EXPECT_EQ(book.check_invariants(), InvariantError::none);

    // Cancel order #2 to free a slot, then verify passive insert succeeds
    EXPECT_TRUE(book.cancel(2));
    EXPECT_TRUE(book.insert({7, 99, 15, Side::buy}));
    EXPECT_EQ(book.check_invariants(), InvariantError::none);
}