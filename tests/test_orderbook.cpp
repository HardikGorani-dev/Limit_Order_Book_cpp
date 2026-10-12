#include <gtest/gtest.h>
#include <lob/OrderBook.hpp>

class OrderBookTest : public ::testing::Test {
protected:
    Order_book book;
    std::vector<Trade> trades;

    void TearDown() override {
        book.check_invariants();
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
    EXPECT_TRUE(book.reduce(1, 15));
    // Order should be fully removed. Subsequent fill should yield nothing.
    book.process_aggressive_order({2, 100, 10, Side::buy}, trades);
    EXPECT_TRUE(trades.empty());
}