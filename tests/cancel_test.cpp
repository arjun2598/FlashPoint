// Tests for cancelling orders.
//
// The book's removal paths were already covered at Milestone 4, including all
// four unlink cases. What is new here is the engine protocol, so these tests
// concentrate on:
//
//   * The reported quantity is what was still resting, not the original size.
//   * A cancel that misses changes nothing and says so.
//   * Cancelling out of the middle of a queue leaves the rest in order.
//
// The last test mixes submits and cancels at random and checks the same
// invariants the engine tests use, plus that every successful cancel reports
// exactly what the book held.

#include "flashpoint/matching_engine.hpp"

#include "flashpoint/order.hpp"
#include "flashpoint/ostream.hpp"
#include "flashpoint/trade.hpp"
#include "flashpoint/types.hpp"

#include <gtest/gtest.h>

#include <random>
#include <vector>

namespace flashpoint {
namespace {

void ignore_trades(const Trade&) {}

[[nodiscard]] SubmitResult rest_buy(MatchingEngine& engine, OrderId::Rep id, Price::Rep price,
                                    Quantity::Rep quantity) {
    return engine.submit(Order::limit(OrderId{id}, Side::Buy, Price{price}, Quantity{quantity}),
                         ignore_trades);
}

[[nodiscard]] SubmitResult rest_sell(MatchingEngine& engine, OrderId::Rep id, Price::Rep price,
                                     Quantity::Rep quantity) {
    return engine.submit(Order::limit(OrderId{id}, Side::Sell, Price{price}, Quantity{quantity}),
                         ignore_trades);
}

// ---------------------------------------------------------------------------
// Cancelling a resting order
// ---------------------------------------------------------------------------

TEST(Cancel, RemovesTheOrderAndReportsItsQuantity) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_buy(engine, 1, 100, 50).accepted());

    const CancelResult result = engine.cancel(OrderId{1});

    EXPECT_EQ(result, (CancelResult{CancelStatus::Cancelled, Quantity{50}}));
    EXPECT_TRUE(engine.book().empty());
    EXPECT_FALSE(engine.book().contains(OrderId{1}));
    EXPECT_FALSE(engine.book().best_bid().has_value());
}

// The reported quantity is what was left, not what was originally sent. An
// order that traded before the cancel arrived only had the remainder to pull.
TEST(Cancel, ReportsTheRemainingQuantityAfterAPartialFill) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_sell(engine, 1, 100, 50).accepted());

    // Takes 30 of the 50.
    ASSERT_TRUE(rest_buy(engine, 2, 100, 30).accepted());
    ASSERT_EQ(engine.book().remaining_of(OrderId{1}), Quantity{20});

    const CancelResult result = engine.cancel(OrderId{1});

    EXPECT_EQ(result.status, CancelStatus::Cancelled);
    EXPECT_EQ(result.cancelled, Quantity{20});
    EXPECT_TRUE(engine.book().empty());
}

TEST(Cancel, LeavesTheOppositeSideAlone) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_buy(engine, 1, 100, 50).accepted());
    ASSERT_TRUE(rest_sell(engine, 2, 105, 40).accepted());

    ASSERT_TRUE(engine.cancel(OrderId{1}).succeeded());

    EXPECT_FALSE(engine.book().best_bid().has_value());
    EXPECT_EQ(engine.book().best_ask(), Price{105});
    EXPECT_EQ(engine.book().size(), 1U);
}

TEST(Cancel, DrainingTheBestLevelPromotesTheNextOne) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_buy(engine, 1, 102, 10).accepted());
    ASSERT_TRUE(rest_buy(engine, 2, 101, 10).accepted());

    ASSERT_TRUE(engine.cancel(OrderId{1}).succeeded());

    EXPECT_EQ(engine.book().best_bid(), Price{101});
    EXPECT_EQ(engine.book().quantity_at(Side::Buy, Price{102}), Quantity{});
}

// ---------------------------------------------------------------------------
// Queue priority
// ---------------------------------------------------------------------------

// Pulling an order from the middle of a queue must leave the others in arrival
// order, and must not change who fills next.
TEST(Cancel, RemovingAMiddleOrderKeepsTheRestInOrder) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_sell(engine, 101, 100, 10).accepted());
    ASSERT_TRUE(rest_sell(engine, 102, 100, 20).accepted());
    ASSERT_TRUE(rest_sell(engine, 103, 100, 30).accepted());

    ASSERT_TRUE(engine.cancel(OrderId{102}).succeeded());

    EXPECT_EQ(engine.book().front_at(Side::Sell, Price{100}), OrderId{101});
    EXPECT_EQ(engine.book().quantity_at(Side::Sell, Price{100}), Quantity{40});
    EXPECT_EQ(engine.book().order_count_at(Side::Sell, Price{100}), 2U);

    // #103 is next once #101 goes.
    ASSERT_TRUE(engine.cancel(OrderId{101}).succeeded());
    EXPECT_EQ(engine.book().front_at(Side::Sell, Price{100}), OrderId{103});
}

TEST(Cancel, RemovingTheFrontOrderPromotesTheNextInLine) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_sell(engine, 101, 100, 10).accepted());
    ASSERT_TRUE(rest_sell(engine, 102, 100, 20).accepted());

    ASSERT_TRUE(engine.cancel(OrderId{101}).succeeded());

    EXPECT_EQ(engine.book().front_at(Side::Sell, Price{100}), OrderId{102});
}

// ---------------------------------------------------------------------------
// Cancels that miss
// ---------------------------------------------------------------------------

TEST(Cancel, UnknownIdReportsUnknownOrderAndChangesNothing) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_buy(engine, 1, 100, 50).accepted());

    const CancelResult result = engine.cancel(OrderId{99});

    EXPECT_EQ(result, (CancelResult{CancelStatus::UnknownOrder, Quantity{}}));
    EXPECT_EQ(engine.book().size(), 1U);
    EXPECT_EQ(engine.book().quantity_at(Side::Buy, Price{100}), Quantity{50});
}

TEST(Cancel, CancellingAnEmptyBookReportsUnknownOrder) {
    MatchingEngine engine;

    EXPECT_EQ(engine.cancel(OrderId{1}).status, CancelStatus::UnknownOrder);
}

TEST(Cancel, ASecondCancelOfTheSameOrderReportsUnknownOrder) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_buy(engine, 1, 100, 50).accepted());

    ASSERT_TRUE(engine.cancel(OrderId{1}).succeeded());

    const CancelResult second = engine.cancel(OrderId{1});
    EXPECT_EQ(second.status, CancelStatus::UnknownOrder);
    EXPECT_EQ(second.cancelled, Quantity{});
}

// An order that filled completely is no longer resting, so a cancel for it
// misses. The engine cannot report "already filled" without keeping a record of
// every id it has ever seen (DD-029).
TEST(Cancel, AFullyFilledOrderIsIndistinguishableFromOneThatNeverExisted) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_sell(engine, 1, 100, 50).accepted());
    ASSERT_TRUE(rest_buy(engine, 2, 100, 50).accepted());
    ASSERT_TRUE(engine.book().empty());

    EXPECT_EQ(engine.cancel(OrderId{1}).status, CancelStatus::UnknownOrder);
    EXPECT_EQ(engine.cancel(OrderId{2}).status, CancelStatus::UnknownOrder);
    EXPECT_EQ(engine.cancel(OrderId{12345}).status, CancelStatus::UnknownOrder);
}

// A cancel naming the reserved "no order" value is malformed rather than a
// miss, so it gets its own status. This mirrors the validity check submit does.
TEST(Cancel, TheReservedOrderIdIsRejectedAsMalformed) {
    MatchingEngine engine;

    const CancelResult result = engine.cancel(OrderId{OrderId::kNone});

    EXPECT_EQ(result, (CancelResult{CancelStatus::RejectedInvalidId, Quantity{}}));
}

// ---------------------------------------------------------------------------
// After cancelling
// ---------------------------------------------------------------------------

// Cancelling frees the id, since the duplicate check only looks at what is
// resting.
TEST(Cancel, FreesTheIdForReuse) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_buy(engine, 1, 100, 50).accepted());
    ASSERT_TRUE(engine.cancel(OrderId{1}).succeeded());

    EXPECT_EQ(rest_buy(engine, 1, 99, 20).status, SubmitStatus::Accepted);
    EXPECT_EQ(engine.book().best_bid(), Price{99});
}

// A cancelled order is not liquidity. An aggressor arriving afterwards must not
// trade against it.
TEST(Cancel, RemovesTheOrderFromWhatAnAggressorCanTrade) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_sell(engine, 1, 100, 50).accepted());
    ASSERT_TRUE(engine.cancel(OrderId{1}).succeeded());

    EXPECT_EQ(engine.book().quantity_available(Side::Buy, Price{200}), Quantity{});

    std::vector<Trade> trades;
    const SubmitResult result =
        engine.submit(Order::limit(OrderId{2}, Side::Buy, Price{100}, Quantity{50}),
                      [&trades](const Trade& trade) { trades.push_back(trade); });

    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(result.resting, Quantity{50});
}

// ---------------------------------------------------------------------------
// Invariants under mixed submit and cancel flow
// ---------------------------------------------------------------------------

// Randomised flow that both adds and pulls liquidity. After every operation:
// a successful cancel reported exactly what the book held, a failed one changed
// nothing, and the book is never left crossed.
TEST(CancelInvariants, ReportedQuantityMatchesTheBookAndNothingIsLeftCrossed) {
    constexpr int kSteps = 3000;

    std::mt19937 rng{20260611U};
    std::uniform_int_distribution<Price::Rep> price_dist{98, 102};
    std::uniform_int_distribution<Quantity::Rep> quantity_dist{1, 60};
    std::uniform_int_distribution<int> roll{0, 99};

    MatchingEngine engine;
    std::vector<OrderId> resting;
    OrderId::Rep next_id = 1;
    Quantity total_cancelled{};
    int successful_cancels = 0;

    for (int step = 0; step < kSteps; ++step) {
        const bool cancelling = !resting.empty() && roll(rng) < 40;

        if (cancelling) {
            std::uniform_int_distribution<std::size_t> pick{0, resting.size() - 1};
            const std::size_t slot = pick(rng);
            const OrderId id = resting[slot];
            resting[slot] = resting.back();
            resting.pop_back();

            // What the book holds right now is what the cancel must report. The
            // order may have been partially filled since it was submitted.
            const auto expected = engine.book().remaining_of(id);

            const CancelResult result = engine.cancel(id);

            if (expected.has_value()) {
                ASSERT_EQ(result.status, CancelStatus::Cancelled) << "step " << step;
                ASSERT_EQ(result.cancelled, *expected) << "step " << step;
                ASSERT_FALSE(engine.book().contains(id)) << "step " << step;
                total_cancelled += result.cancelled;
                ++successful_cancels;
            } else {
                // Fully filled since it was submitted, so the cancel misses.
                ASSERT_EQ(result.status, CancelStatus::UnknownOrder) << "step " << step;
                ASSERT_EQ(result.cancelled, Quantity{}) << "step " << step;
            }
        } else {
            const auto id = OrderId{next_id++};
            const Side side = roll(rng) % 2 == 0 ? Side::Buy : Side::Sell;
            const Order order =
                Order::limit(id, side, Price{price_dist(rng)}, Quantity{quantity_dist(rng)});

            const SubmitResult result = engine.submit(order, ignore_trades);
            ASSERT_TRUE(result.accepted()) << "step " << step;
            ASSERT_EQ(result.filled + result.resting + result.cancelled, order.quantity())
                << "step " << step;

            if (result.resting > Quantity{}) {
                resting.push_back(id);
            }
        }

        const auto bid = engine.book().best_bid();
        const auto ask = engine.book().best_ask();
        if (bid.has_value() && ask.has_value()) {
            ASSERT_LT(*bid, *ask) << "crossed book after step " << step;
        }
    }

    // The run must actually have cancelled things, or the checks above ran on
    // an engine that never exercised the path.
    EXPECT_GT(successful_cancels, 100);
    EXPECT_GT(total_cancelled, Quantity{1000});
}

}  // namespace
}  // namespace flashpoint
