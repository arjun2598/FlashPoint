// Tests for the matching engine.
//
// The cases that matter most:
//
//   * Execution price. A trade prints at the *maker's* price, so a buy limited
//     at 105 lifting an ask resting at 100 trades at 100. Reversing this would
//     silently overcharge every taker while every quantity still balanced.
//   * Queue priority across a partial fill. A partially filled resting order
//     keeps its place in line. Sending it to the back would be invisible in
//     aggregate depth and ruinous for anyone resting size.
//   * The limit boundary. An order must sweep levels it can afford and stop at
//     the first one it cannot, resting the remainder.
//
// The final test submits randomised flow and asserts two invariants after every
// submission: the book is never left crossed, and filled + resting always equals
// what was submitted. The second is a conservation check: it catches quantity
// being invented or lost anywhere in the matching loop.

#include "flashpoint/matching_engine.hpp"

#include "flashpoint/order.hpp"
#include "flashpoint/ostream.hpp"
#include "flashpoint/trade.hpp"

#include "flashpoint/types.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <random>
#include <utility>
#include <vector>

namespace flashpoint {
namespace {

Order buy(OrderId::Rep id, Price::Rep price, Quantity::Rep quantity) {
    return Order{OrderId{id}, Side::Buy, Price{price}, Quantity{quantity}};
}

Order sell(OrderId::Rep id, Price::Rep price, Quantity::Rep quantity) {
    return Order{OrderId{id}, Side::Sell, Price{price}, Quantity{quantity}};
}

/// Captures every trade an order produces, so a test can assert on the sequence
/// rather than only on the aggregate outcome.
using testing_support::EventRecorder;
using testing_support::ignore_events;

struct Submitted {
    SubmitResult result;
    std::vector<Trade> trades;
};

[[nodiscard]] Submitted send(MatchingEngine& engine, const Order& order) {
    EventRecorder recorder;
    const SubmitResult result = engine.submit(order, recorder);
    return Submitted{result, recorder.trades()};
}

// ---------------------------------------------------------------------------
// Resting without trading
// ---------------------------------------------------------------------------

// An order that cannot trade simply joins the
// book, and the sink is never invoked.
TEST(MatchingEngine, NonMarketableOrderRestsAndProducesNoTrades) {
    MatchingEngine engine;
    ASSERT_TRUE(send(engine, sell(1, 101, 50)).result.accepted());

    const Submitted submitted = send(engine, buy(2, 100, 30));

    EXPECT_TRUE(submitted.trades.empty());
    EXPECT_EQ(submitted.result, (SubmitResult{SubmitStatus::Accepted, Quantity{}, Quantity{30}}));
    EXPECT_EQ(engine.book().best_bid(), Price{100});
    EXPECT_EQ(engine.book().best_ask(), Price{101});
}

TEST(MatchingEngine, OrderIntoAnEmptyBookJustRests) {
    MatchingEngine engine;

    const Submitted submitted = send(engine, buy(1, 100, 30));

    EXPECT_TRUE(submitted.trades.empty());
    EXPECT_EQ(submitted.result.resting, Quantity{30});
    EXPECT_EQ(engine.book().quantity_at(Side::Buy, Price{100}), Quantity{30});
}

// ---------------------------------------------------------------------------
// Single fills
// ---------------------------------------------------------------------------

TEST(MatchingEngine, EqualSizesFillEachOtherCompletely) {
    MatchingEngine engine;
    ASSERT_TRUE(send(engine, sell(1, 100, 50)).result.accepted());

    const Submitted submitted = send(engine, buy(2, 100, 50));

    ASSERT_EQ(submitted.trades.size(), 1U);
    EXPECT_EQ(submitted.trades[0].quantity, Quantity{50});
    EXPECT_EQ(submitted.result, (SubmitResult{SubmitStatus::Accepted, Quantity{50}, Quantity{}}));
    EXPECT_TRUE(engine.book().empty());
}

// The aggressor outsizes the book: it takes everything available, and the
// leftover rests at its own limit price.
TEST(MatchingEngine, AggressorLargerThanTheBookRestsItsRemainder) {
    MatchingEngine engine;
    ASSERT_TRUE(send(engine, sell(1, 100, 30)).result.accepted());

    const Submitted submitted = send(engine, buy(2, 100, 50));

    ASSERT_EQ(submitted.trades.size(), 1U);
    EXPECT_EQ(submitted.trades[0].quantity, Quantity{30});
    EXPECT_EQ(submitted.result, (SubmitResult{SubmitStatus::Accepted, Quantity{30}, Quantity{20}}));
    EXPECT_EQ(engine.book().best_bid(), Price{100});
    EXPECT_EQ(engine.book().quantity_at(Side::Buy, Price{100}), Quantity{20});
    EXPECT_FALSE(engine.book().best_ask().has_value());
}

// The resting order outsizes the aggressor: it stays, reduced.
TEST(MatchingEngine, RestingOrderLargerThanTheAggressorStaysReduced) {
    MatchingEngine engine;
    ASSERT_TRUE(send(engine, sell(1, 100, 50)).result.accepted());

    const Submitted submitted = send(engine, buy(2, 100, 30));

    ASSERT_EQ(submitted.trades.size(), 1U);
    EXPECT_EQ(submitted.result, (SubmitResult{SubmitStatus::Accepted, Quantity{30}, Quantity{}}));
    EXPECT_EQ(engine.book().quantity_at(Side::Sell, Price{100}), Quantity{20});
    EXPECT_EQ(engine.book().remaining_of(OrderId{1}), Quantity{20});
    EXPECT_FALSE(engine.book().contains(OrderId{2}));
}

// ---------------------------------------------------------------------------
// Execution price
// ---------------------------------------------------------------------------

// The single most consequential rule in this milestone. A buyer willing to pay
// 105 that lifts an ask resting at 100 pays 100, not 105.
TEST(MatchingEngine, TradePrintsAtTheRestingPriceNotTheAggressorLimit) {
    MatchingEngine engine;
    ASSERT_TRUE(send(engine, sell(1, 100, 50)).result.accepted());

    const Submitted submitted = send(engine, buy(2, 105, 50));

    ASSERT_EQ(submitted.trades.size(), 1U);
    EXPECT_EQ(submitted.trades[0].price, Price{100});
}

// The same rule mirrored: a seller willing to accept 95 that hits a bid resting
// at 100 receives 100.
TEST(MatchingEngine, PriceImprovementAlsoAccruesToASellingAggressor) {
    MatchingEngine engine;
    ASSERT_TRUE(send(engine, buy(1, 100, 50)).result.accepted());

    const Submitted submitted = send(engine, sell(2, 95, 50));

    ASSERT_EQ(submitted.trades.size(), 1U);
    EXPECT_EQ(submitted.trades[0].price, Price{100});
}

TEST(MatchingEngine, TradeRecordsMakerTakerAndAggressorSide) {
    MatchingEngine engine;
    ASSERT_TRUE(send(engine, sell(1, 100, 50)).result.accepted());

    const Submitted submitted = send(engine, buy(2, 100, 50));

    ASSERT_EQ(submitted.trades.size(), 1U);
    EXPECT_EQ(submitted.trades[0], (Trade{.maker_id = OrderId{1},
                                          .taker_id = OrderId{2},
                                          .price = Price{100},
                                          .quantity = Quantity{50},
                                          .aggressor = Side::Buy}));
}

TEST(MatchingEngine, AggressorSideIsRecordedAsSellWhenTheSellerInitiates) {
    MatchingEngine engine;
    ASSERT_TRUE(send(engine, buy(1, 100, 50)).result.accepted());

    const Submitted submitted = send(engine, sell(2, 100, 50));

    ASSERT_EQ(submitted.trades.size(), 1U);
    EXPECT_EQ(submitted.trades[0].aggressor, Side::Sell);
    EXPECT_EQ(submitted.trades[0].maker_id, OrderId{1});
    EXPECT_EQ(submitted.trades[0].taker_id, OrderId{2});
}

// ---------------------------------------------------------------------------
// Sweeping
// ---------------------------------------------------------------------------

// The worked example: three resting sells, one buy that clears the first two
// levels and partially fills the third.
TEST(MatchingEngine, SweepsAPriceLevelInArrivalOrderThenMovesUp) {
    MatchingEngine engine;
    ASSERT_TRUE(send(engine, sell(101, 100, 30)).result.accepted());
    ASSERT_TRUE(send(engine, sell(102, 100, 50)).result.accepted());
    ASSERT_TRUE(send(engine, sell(103, 101, 40)).result.accepted());

    const Submitted submitted = send(engine, buy(1, 101, 100));

    ASSERT_EQ(submitted.trades.size(), 3U);

    // Oldest first within the level, cheapest level first.
    EXPECT_EQ(submitted.trades[0].maker_id, OrderId{101});
    EXPECT_EQ(submitted.trades[0].price, Price{100});
    EXPECT_EQ(submitted.trades[0].quantity, Quantity{30});

    EXPECT_EQ(submitted.trades[1].maker_id, OrderId{102});
    EXPECT_EQ(submitted.trades[1].price, Price{100});
    EXPECT_EQ(submitted.trades[1].quantity, Quantity{50});

    EXPECT_EQ(submitted.trades[2].maker_id, OrderId{103});
    EXPECT_EQ(submitted.trades[2].price, Price{101});
    EXPECT_EQ(submitted.trades[2].quantity, Quantity{20});

    EXPECT_EQ(submitted.result, (SubmitResult{SubmitStatus::Accepted, Quantity{100}, Quantity{}}));
    EXPECT_EQ(engine.book().remaining_of(OrderId{103}), Quantity{20});
    EXPECT_EQ(engine.book().best_ask(), Price{101});
    EXPECT_FALSE(engine.book().best_bid().has_value());
}

// A sell aggressor must take the *highest* bids first, the mirror of taking
// the cheapest asks.
TEST(MatchingEngine, SellingAggressorTakesTheHighestBidsFirst) {
    MatchingEngine engine;
    ASSERT_TRUE(send(engine, buy(1, 98, 10)).result.accepted());
    ASSERT_TRUE(send(engine, buy(2, 100, 10)).result.accepted());
    ASSERT_TRUE(send(engine, buy(3, 99, 10)).result.accepted());

    const Submitted submitted = send(engine, sell(4, 98, 30));

    ASSERT_EQ(submitted.trades.size(), 3U);
    EXPECT_EQ(submitted.trades[0].price, Price{100});
    EXPECT_EQ(submitted.trades[1].price, Price{99});
    EXPECT_EQ(submitted.trades[2].price, Price{98});
    EXPECT_TRUE(engine.book().empty());
}

// ---------------------------------------------------------------------------
// The limit boundary
// ---------------------------------------------------------------------------

// The order must take what it can afford, stop at the first level it cannot, and
// rest the rest. Getting the comparison wrong by one tick either trades at a
// price the client never agreed to, or fails to trade when it should.
TEST(MatchingEngine, StopsAtTheFirstLevelBeyondItsLimitAndRestsTheRemainder) {
    MatchingEngine engine;
    ASSERT_TRUE(send(engine, sell(1, 100, 50)).result.accepted());
    ASSERT_TRUE(send(engine, sell(2, 102, 50)).result.accepted());

    const Submitted submitted = send(engine, buy(3, 101, 100));

    ASSERT_EQ(submitted.trades.size(), 1U);
    EXPECT_EQ(submitted.trades[0].price, Price{100});
    EXPECT_EQ(submitted.result, (SubmitResult{SubmitStatus::Accepted, Quantity{50}, Quantity{50}}));

    // The untouched level and the newly rested remainder leave an uncrossed book.
    EXPECT_EQ(engine.book().best_bid(), Price{101});
    EXPECT_EQ(engine.book().best_ask(), Price{102});
}

// Equality must trade. A limit of exactly the resting price is a match, not a
// miss, on both sides.
TEST(MatchingEngine, ALimitEqualToTheRestingPriceTrades) {
    MatchingEngine engine;
    ASSERT_TRUE(send(engine, sell(1, 100, 10)).result.accepted());
    EXPECT_EQ(send(engine, buy(2, 100, 10)).trades.size(), 1U);

    ASSERT_TRUE(send(engine, buy(3, 100, 10)).result.accepted());
    EXPECT_EQ(send(engine, sell(4, 100, 10)).trades.size(), 1U);
}

// One tick short must not trade, on either side.
TEST(MatchingEngine, ALimitOneTickShortDoesNotTrade) {
    MatchingEngine engine;
    ASSERT_TRUE(send(engine, sell(1, 100, 10)).result.accepted());
    EXPECT_TRUE(send(engine, buy(2, 99, 10)).trades.empty());

    MatchingEngine other;
    ASSERT_TRUE(send(other, buy(3, 100, 10)).result.accepted());
    EXPECT_TRUE(send(other, sell(4, 101, 10)).trades.empty());
}

// ---------------------------------------------------------------------------
// Queue priority across a partial fill
// ---------------------------------------------------------------------------

// A partially filled resting order keeps its place at the front of the queue.
// If it were sent to the back, aggregate depth would look identical and only the
// order of subsequent executions would betray the bug.
TEST(MatchingEngine, PartiallyFilledRestingOrderKeepsItsQueuePosition) {
    MatchingEngine engine;
    ASSERT_TRUE(send(engine, sell(101, 100, 50)).result.accepted());
    ASSERT_TRUE(send(engine, sell(102, 100, 30)).result.accepted());

    // Takes 20 of #101's 50, leaving it at the front with 30.
    ASSERT_EQ(send(engine, buy(1, 100, 20)).trades.size(), 1U);
    EXPECT_EQ(engine.book().front_at(Side::Sell, Price{100}), OrderId{101});
    EXPECT_EQ(engine.book().remaining_of(OrderId{101}), Quantity{30});

    // The next aggressor must therefore meet #101 again, not #102.
    const Submitted submitted = send(engine, buy(2, 100, 30));

    ASSERT_EQ(submitted.trades.size(), 1U);
    EXPECT_EQ(submitted.trades[0].maker_id, OrderId{101});
    EXPECT_EQ(engine.book().front_at(Side::Sell, Price{100}), OrderId{102});
    EXPECT_EQ(engine.book().remaining_of(OrderId{102}), Quantity{30});
}

// ---------------------------------------------------------------------------
// Rejections
// ---------------------------------------------------------------------------

// The boundary check DD-012 promised. This is the only place structural validity
// is examined, and everything downstream is entitled to assume it held.
TEST(MatchingEngine, MalformedOrdersAreRejectedAndChangeNothing) {
    MatchingEngine engine;

    EXPECT_EQ(send(engine, buy(1, 100, 0)).result.status, SubmitStatus::RejectedInvalid);
    EXPECT_EQ(send(engine, buy(OrderId::kNone, 100, 10)).result.status,
              SubmitStatus::RejectedInvalid);

    const Order bad_side{OrderId{2}, static_cast<Side>(9), Price{100}, Quantity{10}};
    EXPECT_EQ(send(engine, bad_side).result.status, SubmitStatus::RejectedInvalid);

    EXPECT_TRUE(engine.book().empty());
}

TEST(MatchingEngine, RejectedOrdersReportNoFillAndNoResting) {
    MatchingEngine engine;

    const Submitted submitted = send(engine, buy(1, 100, 0));

    EXPECT_TRUE(submitted.trades.empty());
    EXPECT_EQ(submitted.result.filled, Quantity{});
    EXPECT_EQ(submitted.result.resting, Quantity{});
}

TEST(MatchingEngine, DuplicateIdIsRejectedWithoutDisturbingTheRestingOrder) {
    MatchingEngine engine;
    ASSERT_TRUE(send(engine, buy(1, 100, 50)).result.accepted());

    const Submitted submitted = send(engine, buy(1, 101, 999));

    EXPECT_EQ(submitted.result.status, SubmitStatus::RejectedDuplicateId);
    EXPECT_TRUE(submitted.trades.empty());
    EXPECT_EQ(engine.book().size(), 1U);
    EXPECT_EQ(engine.book().best_bid(), Price{100});
}

// An order that fully fills never rests, so its id is free again afterwards.
TEST(MatchingEngine, AnIdIsReusableOnceItsOrderHasFullyFilled) {
    MatchingEngine engine;
    ASSERT_TRUE(send(engine, sell(1, 100, 10)).result.accepted());
    ASSERT_EQ(send(engine, buy(2, 100, 10)).trades.size(), 1U);

    EXPECT_EQ(send(engine, buy(2, 99, 10)).result.status, SubmitStatus::Accepted);
}

// ---------------------------------------------------------------------------
// Invariants under randomised flow
// ---------------------------------------------------------------------------

// Two properties that must hold after every single submission, checked against
// thousands of orders drawn from a narrow price band so that crossing is
// frequent rather than incidental.
//
//   * The book is never left crossed. Only the engine can produce a cross, and
//     it must never leave one behind.
//   * filled + resting = submitted quantity. A conservation check: it catches
//     quantity being invented or dropped anywhere in the matching loop, which no
//     single hand-written case would notice.
//
// Fixed seed, so any failure is reproducible.
TEST(MatchingEngineInvariants, NeverLeavesTheBookCrossedAndNeverLosesQuantity) {
    constexpr int kSteps = 3000;

    std::mt19937 rng{20260609U};
    std::uniform_int_distribution<Price::Rep> price_dist{98, 102};
    std::uniform_int_distribution<Quantity::Rep> quantity_dist{1, 60};
    std::uniform_int_distribution<int> side_dist{0, 1};

    MatchingEngine engine;
    Quantity total_traded{};

    for (int step = 0; step < kSteps; ++step) {
        const auto id = static_cast<OrderId::Rep>(step + 1);
        const Side side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
        const Order order{OrderId{id}, side, Price{price_dist(rng)}, Quantity{quantity_dist(rng)}};

        EventRecorder recorder;
        const SubmitResult result = engine.submit(order, recorder);
        ASSERT_TRUE(result.accepted()) << "step " << step << ' ' << order;

        // Conservation: nothing invented, nothing lost.
        ASSERT_EQ(result.filled + result.resting, order.quantity()) << "step " << step;

        // The recorded trades must account for exactly the filled quantity.
        Quantity from_trades{};
        for (const Trade& trade : recorder.trades()) {
            from_trades += trade.quantity;
            ASSERT_EQ(trade.taker_id, order.id()) << "step " << step;
            ASSERT_EQ(trade.aggressor, order.side()) << "step " << step;
        }
        ASSERT_EQ(from_trades, result.filled) << "step " << step;
        total_traded += from_trades;

        // The engine must never leave a crossed book behind.
        const auto bid = engine.book().best_bid();
        const auto ask = engine.book().best_ask();
        if (bid.has_value() && ask.has_value()) {
            ASSERT_LT(*bid, *ask) << "crossed book after step " << step;
        }
    }

    // The band is narrow enough that trading must actually have happened, or the
    // invariants above would have been checked against an idle engine.
    EXPECT_GT(total_traded, Quantity{1000});
}

}  // namespace
}  // namespace flashpoint
