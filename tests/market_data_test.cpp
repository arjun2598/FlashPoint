// Tests for the aggregated views of the book.
//
// Two things are published: top of book, and a depth snapshot.
//
// The snapshot writes into a buffer the caller owns, so the tests check the
// return count as carefully as the contents. Writing past the end of a caller's
// array, or reporting a count that does not match what was written, are the two
// failures that would matter.

#include "flashpoint/market_data.hpp"

#include "flashpoint/order.hpp"
#include "flashpoint/order_book.hpp"
#include "flashpoint/ostream.hpp"
#include "flashpoint/types.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace flashpoint {
namespace {

Order buy(OrderId::Rep id, Price::Rep price, Quantity::Rep quantity) {
    return Order::limit(OrderId{id}, Side::Buy, Price{price}, Quantity{quantity});
}

Order sell(OrderId::Rep id, Price::Rep price, Quantity::Rep quantity) {
    return Order::limit(OrderId{id}, Side::Sell, Price{price}, Quantity{quantity});
}

/// Bids: 20 at 99, 60 at 98 (across two orders), 10 at 97.
/// Asks: 30 at 100 (across two orders), 40 at 101, 50 at 105.
void build_book(OrderBook& book) {
    ASSERT_TRUE(book.add(buy(1, 99, 20)));
    ASSERT_TRUE(book.add(buy(2, 98, 25)));
    ASSERT_TRUE(book.add(buy(3, 98, 35)));
    ASSERT_TRUE(book.add(buy(4, 97, 10)));

    ASSERT_TRUE(book.add(sell(11, 100, 10)));
    ASSERT_TRUE(book.add(sell(12, 100, 20)));
    ASSERT_TRUE(book.add(sell(13, 101, 40)));
    ASSERT_TRUE(book.add(sell(14, 105, 50)));
}

// ---------------------------------------------------------------------------
// Top of book
// ---------------------------------------------------------------------------

TEST(TopOfBookView, IsEmptyOnAnEmptyBook) {
    const OrderBook book;
    const TopOfBook top = book.top_of_book();

    EXPECT_FALSE(top.has_bid());
    EXPECT_FALSE(top.has_ask());
    EXPECT_EQ(top.bid_quantity, Quantity{});
    EXPECT_EQ(top.ask_quantity, Quantity{});
    EXPECT_FALSE(top.spread().has_value());
}

TEST(TopOfBookView, ReportsTheBestPriceAndTheQuantityThere) {
    OrderBook book;
    build_book(book);

    const TopOfBook top = book.top_of_book();

    EXPECT_EQ(top.bid_price, Price{99});
    EXPECT_EQ(top.bid_quantity, Quantity{20});
    EXPECT_EQ(top.ask_price, Price{100});
    EXPECT_EQ(top.ask_quantity, Quantity{30});
    EXPECT_EQ(top.spread(), 1);
}

// One side present is a normal state, not an error.
TEST(TopOfBookView, HandlesOneSidedBooks) {
    OrderBook bids_only;
    ASSERT_TRUE(bids_only.add(buy(1, 99, 20)));
    const TopOfBook bid_side = bids_only.top_of_book();
    EXPECT_TRUE(bid_side.has_bid());
    EXPECT_FALSE(bid_side.has_ask());
    EXPECT_FALSE(bid_side.spread().has_value());

    OrderBook asks_only;
    ASSERT_TRUE(asks_only.add(sell(2, 100, 20)));
    const TopOfBook ask_side = asks_only.top_of_book();
    EXPECT_FALSE(ask_side.has_bid());
    EXPECT_TRUE(ask_side.has_ask());
}

TEST(TopOfBookView, FollowsTheBookAsOrdersLeave) {
    OrderBook book;
    build_book(book);

    ASSERT_TRUE(book.remove(OrderId{1}));  // the only order at 99

    const TopOfBook top = book.top_of_book();
    EXPECT_EQ(top.bid_price, Price{98});
    EXPECT_EQ(top.bid_quantity, Quantity{60});
    EXPECT_EQ(top.spread(), 2);
}

TEST(TopOfBookView, ReportsTheSpreadInTicksIncludingNegativePrices) {
    OrderBook book;
    ASSERT_TRUE(book.add(buy(1, -10, 5)));
    ASSERT_TRUE(book.add(sell(2, 5, 5)));

    EXPECT_EQ(book.top_of_book().spread(), 15);
}

// ---------------------------------------------------------------------------
// Level count
//
// This is the L that every non-constant operation in the book scales with, so
// it is worth being able to observe.
// ---------------------------------------------------------------------------

TEST(LevelCount, IsZeroOnAnEmptyBook) {
    const OrderBook book;

    EXPECT_EQ(book.level_count(Side::Buy), 0U);
    EXPECT_EQ(book.level_count(Side::Sell), 0U);
}

TEST(LevelCount, CountsDistinctPricesPerSide) {
    OrderBook book;
    build_book(book);

    // Four bid orders across three prices, four ask orders across three.
    EXPECT_EQ(book.level_count(Side::Buy), 3U);
    EXPECT_EQ(book.level_count(Side::Sell), 3U);
}

TEST(LevelCount, FallsWhenALevelDrainsAndNotBefore) {
    OrderBook book;
    ASSERT_TRUE(book.add(sell(1, 100, 10)));
    ASSERT_TRUE(book.add(sell(2, 100, 20)));
    ASSERT_TRUE(book.add(sell(3, 101, 30)));
    ASSERT_EQ(book.level_count(Side::Sell), 2U);

    // The level survives while another order remains at that price.
    ASSERT_TRUE(book.remove(OrderId{1}));
    EXPECT_EQ(book.level_count(Side::Sell), 2U);

    ASSERT_TRUE(book.remove(OrderId{2}));
    EXPECT_EQ(book.level_count(Side::Sell), 1U);

    ASSERT_TRUE(book.remove(OrderId{3}));
    EXPECT_EQ(book.level_count(Side::Sell), 0U);
}

TEST(LevelCount, AgreesWithTheNumberOfSnapshotRows) {
    OrderBook book;
    build_book(book);

    std::array<LevelSnapshot, 16> rows{};
    EXPECT_EQ(book.snapshot(Side::Buy, rows), book.level_count(Side::Buy));
    EXPECT_EQ(book.snapshot(Side::Sell, rows), book.level_count(Side::Sell));
}

// ---------------------------------------------------------------------------
// Depth snapshot
// ---------------------------------------------------------------------------

TEST(Snapshot, WritesNothingForAnEmptyBook) {
    const OrderBook book;
    std::array<LevelSnapshot, 4> rows{};

    EXPECT_EQ(book.snapshot(Side::Buy, rows), 0U);
    EXPECT_EQ(book.snapshot(Side::Sell, rows), 0U);
}

// Bids descend from the best price, asks ascend.
TEST(Snapshot, OrdersBidsHighestFirstAndAsksLowestFirst) {
    OrderBook book;
    build_book(book);

    std::array<LevelSnapshot, 8> rows{};

    ASSERT_EQ(book.snapshot(Side::Buy, rows), 3U);
    EXPECT_EQ(rows[0].price, Price{99});
    EXPECT_EQ(rows[1].price, Price{98});
    EXPECT_EQ(rows[2].price, Price{97});

    ASSERT_EQ(book.snapshot(Side::Sell, rows), 3U);
    EXPECT_EQ(rows[0].price, Price{100});
    EXPECT_EQ(rows[1].price, Price{101});
    EXPECT_EQ(rows[2].price, Price{105});
}

// This is what makes it level 2 rather than level 3: orders at one price become
// a single row.
TEST(Snapshot, AggregatesOrdersAtTheSamePriceIntoOneRow) {
    OrderBook book;
    build_book(book);

    std::array<LevelSnapshot, 8> rows{};
    ASSERT_EQ(book.snapshot(Side::Buy, rows), 3U);

    // 25 and 35 at price 98 become one row of 60 across two orders.
    EXPECT_EQ(rows[1], (LevelSnapshot{Price{98}, Quantity{60}, 2}));
    EXPECT_EQ(rows[0], (LevelSnapshot{Price{99}, Quantity{20}, 1}));
}

// The caller chooses the depth by sizing the buffer.
TEST(Snapshot, StopsAtTheEndOfTheCallersBuffer) {
    OrderBook book;
    build_book(book);

    std::array<LevelSnapshot, 2> rows{};
    ASSERT_EQ(book.snapshot(Side::Sell, rows), 2U);
    EXPECT_EQ(rows[0].price, Price{100});
    EXPECT_EQ(rows[1].price, Price{101});
}

// A buffer bigger than the book must not be written past the level count.
TEST(Snapshot, LeavesTheRestOfALargeBufferUntouched) {
    OrderBook book;
    ASSERT_TRUE(book.add(sell(1, 100, 10)));

    const LevelSnapshot sentinel{Price{-1}, Quantity{999}, 42};
    std::array<LevelSnapshot, 4> rows{sentinel, sentinel, sentinel, sentinel};

    ASSERT_EQ(book.snapshot(Side::Sell, rows), 1U);
    EXPECT_EQ(rows[0], (LevelSnapshot{Price{100}, Quantity{10}, 1}));
    EXPECT_EQ(rows[1], sentinel);
    EXPECT_EQ(rows[2], sentinel);
    EXPECT_EQ(rows[3], sentinel);
}

TEST(Snapshot, AZeroLengthBufferWritesNothingAndReportsZero) {
    OrderBook book;
    build_book(book);

    EXPECT_EQ(book.snapshot(Side::Buy, std::span<LevelSnapshot>{}), 0U);
}

// Each side sees only its own levels.
TEST(Snapshot, DoesNotMixTheTwoSides) {
    OrderBook book;
    ASSERT_TRUE(book.add(buy(1, 99, 20)));
    ASSERT_TRUE(book.add(sell(2, 100, 30)));

    std::array<LevelSnapshot, 4> rows{};

    ASSERT_EQ(book.snapshot(Side::Buy, rows), 1U);
    EXPECT_EQ(rows[0].price, Price{99});

    ASSERT_EQ(book.snapshot(Side::Sell, rows), 1U);
    EXPECT_EQ(rows[0].price, Price{100});
}

TEST(Snapshot, FollowsPartialFillsAndRemovals) {
    OrderBook book;
    ASSERT_TRUE(book.add(sell(1, 100, 30)));
    ASSERT_TRUE(book.add(sell(2, 100, 20)));

    std::array<LevelSnapshot, 4> rows{};

    ASSERT_EQ(book.snapshot(Side::Sell, rows), 1U);
    EXPECT_EQ(rows[0], (LevelSnapshot{Price{100}, Quantity{50}, 2}));

    ASSERT_TRUE(book.reduce(OrderId{1}, Quantity{10}));
    ASSERT_EQ(book.snapshot(Side::Sell, rows), 1U);
    EXPECT_EQ(rows[0], (LevelSnapshot{Price{100}, Quantity{40}, 2}));

    ASSERT_TRUE(book.remove(OrderId{2}));
    ASSERT_EQ(book.snapshot(Side::Sell, rows), 1U);
    EXPECT_EQ(rows[0], (LevelSnapshot{Price{100}, Quantity{20}, 1}));

    ASSERT_TRUE(book.remove(OrderId{1}));
    EXPECT_EQ(book.snapshot(Side::Sell, rows), 0U);
}

// The same buffer is meant to be reused across snapshots without allocating.
TEST(Snapshot, CanReuseOneBufferAcrossManySnapshots) {
    OrderBook book;
    build_book(book);

    std::array<LevelSnapshot, 8> rows{};
    for (int i = 0; i < 100; ++i) {
        ASSERT_EQ(book.snapshot(Side::Sell, rows), 3U);
        ASSERT_EQ(rows[0].price, Price{100});
    }
}

// A snapshot is a copy, so it does not change when the book does afterwards.
TEST(Snapshot, IsACopyRatherThanAViewIntoTheBook) {
    OrderBook book;
    ASSERT_TRUE(book.add(sell(1, 100, 30)));

    std::array<LevelSnapshot, 4> rows{};
    ASSERT_EQ(book.snapshot(Side::Sell, rows), 1U);

    ASSERT_TRUE(book.remove(OrderId{1}));

    EXPECT_EQ(rows[0], (LevelSnapshot{Price{100}, Quantity{30}, 1}));
    EXPECT_TRUE(book.empty());
}

// The snapshot must agree with the per-price queries it summarises.
TEST(Snapshot, AgreesWithTheBooksOwnLevelQueries) {
    OrderBook book;
    build_book(book);

    std::array<LevelSnapshot, 8> rows{};
    const std::size_t written = book.snapshot(Side::Sell, rows);

    for (std::size_t i = 0; i < written; ++i) {
        EXPECT_EQ(rows[i].quantity, book.quantity_at(Side::Sell, rows[i].price));
        EXPECT_EQ(rows[i].order_count, book.order_count_at(Side::Sell, rows[i].price));
    }
}

}  // namespace
}  // namespace flashpoint
