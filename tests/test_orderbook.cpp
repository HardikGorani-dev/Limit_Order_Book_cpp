#include <gtest/gtest.h>
#include <lob/OrderBook.hpp>

class OrderBookTest : public ::testing::Test {
protected:
    Order_book book;
    std::vector<Trade> trades;

    void TearDown() override {
        EXPECT_EQ(book.check_invariants(), InvariantError::none);
        trades.clear();
    }
};

// --- MATCHING RULES & PRICE PRIORITY ---

TEST_F(OrderBookTest, ExactFill_TradePriceIsRestingPrice) {
    book.insert({1, 100, 50, Side::sell});
    book.process_aggressive_order({2, 100, 50, Side::buy}, trades);
    
    ASSERT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].Buyer_id, 2);
    EXPECT_EQ(trades[0].Seller_id, 1);
    EXPECT_EQ(trades[0].Price, 100); // Must be resting Price
    EXPECT_EQ(trades[0].Quantity, 50);
}

TEST_F(OrderBookTest, SellAggressor_IgnoresWorseLimits) {
    book.insert({1, 90, 50, Side::buy}); // Too low
    book.insert({2, 100, 50, Side::buy}); // Match
    
    book.process_aggressive_order({3, 95, 50, Side::sell}, trades);
    
    ASSERT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].Buyer_id, 2);
    EXPECT_EQ(trades[0].Price, 100);
}

TEST_F(OrderBookTest, PartialFill_RemainderRests) {
    book.insert({1, 100, 20, Side::sell});
    book.process_aggressive_order({2, 100, 50, Side::buy}, trades);
    
    ASSERT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].Quantity, 20);
    
    // Aggressor remainder (30) must rest. A new sell should hit it.
    trades.clear();
    book.process_aggressive_order({3, 100, 30, Side::sell}, trades);
    ASSERT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].Buyer_id, 2);
    EXPECT_EQ(trades[0].Quantity, 30);
}

TEST_F(OrderBookTest, MultiLevelSweep_StopsAtLimit) {
    book.insert({1, 100, 10, Side::sell});
    book.insert({2, 101, 10, Side::sell});
    book.insert({3, 102, 10, Side::sell});
    
    book.process_aggressive_order({4, 101, 50, Side::buy}, trades); // Only sweeps 100 and 101
    
    ASSERT_EQ(trades.size(), 2);
    EXPECT_EQ(trades[0].Price, 100);
    EXPECT_EQ(trades[1].Price, 101);
}

// --- QUEUE PRIORITY (FIFO) ---

TEST_F(OrderBookTest, FifoPriority_Maintained) {
    book.insert({1, 100, 10, Side::sell});
    book.insert({2, 100, 10, Side::sell});
    
    book.process_aggressive_order({3, 100, 10, Side::buy}, trades);
    
    ASSERT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].Seller_id, 1); // Order 1 must fill first
}

TEST_F(OrderBookTest, QueuePosition_KeptAfterPartialFill) {
    book.insert({1, 100, 20, Side::sell});
    book.insert({2, 100, 10, Side::sell});
    
    book.process_aggressive_order({3, 100, 10, Side::buy}, trades); // Partially fills Order 1
    trades.clear();
    
    book.process_aggressive_order({4, 100, 10, Side::buy}, trades); // Order 1 should still be first
    
    ASSERT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].Seller_id, 1);
}

TEST_F(OrderBookTest, QueuePosition_KeptAfterReduce) {
    book.insert({1, 100, 20, Side::sell});
    book.insert({2, 100, 10, Side::sell});
    
    book.reduce(1, 10); // Reduce Order 1
    book.process_aggressive_order({3, 100, 10, Side::buy}, trades);
    
    ASSERT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].Seller_id, 1); // Order 1 must still be first
}

TEST_F(OrderBookTest, QueuePosition_LostAfterCancelAndReadd) {
    book.insert({1, 100, 10, Side::sell});
    book.insert({2, 100, 10, Side::sell});
    
    book.cancel(1);
    book.insert({1, 100, 10, Side::sell});
    
    book.process_aggressive_order({3, 100, 10, Side::buy}, trades);
    
    ASSERT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].Seller_id, 2); // Order 2 is now first
}

// --- VALIDATION & EDGE CASES ---

TEST_F(OrderBookTest, RejectDuplicateIds) {
    EXPECT_TRUE(book.insert({1, 100, 10, Side::sell}));
    EXPECT_FALSE(book.insert({1, 101, 10, Side::buy}));
}

TEST_F(OrderBookTest, RejectZeroQuantity) {
    EXPECT_FALSE(book.insert({1, 100, 0, Side::sell}));
    EXPECT_FALSE(book.process_aggressive_order({2, 100, 0, Side::buy}, trades));
}

TEST_F(OrderBookTest, Cancel_UnknownId_Ignored) {
    book.insert({1, 100, 10, Side::sell});
    EXPECT_FALSE(book.cancel(999)); // Should return false, not crash
}

TEST_F(OrderBookTest, Cancel_RemovesEmptyPriceLevel) {
    book.insert({1, 100, 10, Side::sell});
    book.cancel(1);
    // Invariants check in TearDown will fail if the empty level is still in the map
}

TEST_F(OrderBookTest, Reduce_UnknownId_Ignored) {
    book.insert({1, 100, 10, Side::sell});
    EXPECT_FALSE(book.reduce(999, 5));
}

TEST_F(OrderBookTest, Reduce_ZeroQty_Ignored) {
    book.insert({1, 100, 10, Side::sell});
    EXPECT_FALSE(book.reduce(1, 0));
}

TEST_F(OrderBookTest, Reduce_OverQty_ActsAsCancel) {
    book.insert({1, 100, 10, Side::sell});
    
    // Over-reduce must be rejected and return false, leaving order intact
    EXPECT_FALSE(book.reduce(1, 15)); 
    
    // Validate the order is still resting and can be fully filled
    book.process_aggressive_order({2, 100, 10, Side::buy}, trades);
    ASSERT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].Quantity, 10);
}



TEST_F(OrderBookTest, QueuePosition_KeptAfterPartialFill_SellSide) {
    book.insert({1, 100, 20, Side::buy});
    book.insert({2, 100, 10, Side::buy});
    
    book.process_aggressive_order({3, 100, 10, Side::sell}, trades); // Fills 10 of Order 1
    trades.clear();
    
    book.process_aggressive_order({4, 100, 10, Side::sell}, trades); // Order 1 must still be first in line
    
    ASSERT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].Buyer_id, 1);
}

TEST_F(OrderBookTest, MultiLevelSweep_SellSide) {
    book.insert({1, 100, 10, Side::buy});
    book.insert({2, 99, 10, Side::buy});
    book.insert({3, 98, 10, Side::buy});
    
    book.process_aggressive_order({4, 99, 50, Side::sell}, trades); 
    
    ASSERT_EQ(trades.size(), 2);
    EXPECT_EQ(trades[0].Price, 100);
    EXPECT_EQ(trades[1].Price, 99); // Stops at 99 limit
}

TEST_F(OrderBookTest, StrongerLimitPrice_RemainderRestsAtLimit) {
    book.insert({1, 100, 10, Side::buy}); 
    
   book.process_aggressive_order({2, 95, 50, Side::sell}, trades);
    ASSERT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].Quantity, 10);
    EXPECT_EQ(trades[0].Price, 100);
    
    trades.clear();
    
     book.process_aggressive_order({3, 95, 40, Side::buy}, trades);
    ASSERT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].Seller_id, 2);
    EXPECT_EQ(trades[0].Price, 95);
    EXPECT_EQ(trades[0].Quantity, 40);
}

TEST_F(OrderBookTest, RejectDuplicateId_AggressiveOrder) {
    book.insert({1, 100, 10, Side::sell});
    
    EXPECT_FALSE(book.process_aggressive_order({1, 100, 10, Side::buy}, trades));
    EXPECT_TRUE(trades.empty());
}


TEST_F(OrderBookTest, Cancel_BidSide) {
    book.insert({1, 100, 10, Side::buy});
    EXPECT_TRUE(book.cancel(1));
    
    book.process_aggressive_order({2, 100, 10, Side::sell}, trades);
    EXPECT_TRUE(trades.empty()); 
}