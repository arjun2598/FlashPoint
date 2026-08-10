// Tests for market orders and time-in-force.
//
// What matters here:
//
//   * A market order stops at its protection price. The venue derives that from
//     the opposite touch, so a thin book cannot let one sweep to any price.
//   * IOC cancels its remainder instead of resting it.
//   * FOK either fills completely or does nothing at all. "Nothing at all" has
//     to mean no trades were emitted, not trades that were undone.
//   * filled + resting + cancelled always equals the submitted quantity.

#include "flashpoint/matching_engine.hpp"

#include "flashpoint/order.hpp"
#include "flashpoint/ostream.hpp"
#include "flashpoint/trade.hpp"

#include "flashpoint/types.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <random>
#include <utility>
#include <vector>

namespace flashpoint {
namespace {

Order limit_buy(OrderId::Rep id, Price::Rep price, Quantity::Rep quantity,
                TimeInForce tif = TimeInForce::GoodTillCancel) {
    return Order::limit(OrderId{id}, Side::Buy, Price{price}, Quantity{quantity}, tif);
}

Order limit_sell(OrderId::Rep id, Price::Rep price, Quantity::Rep quantity,
                 TimeInForce tif = TimeInForce::GoodTillCancel) {
    return Order::limit(OrderId{id}, Side::Sell, Price{price}, Quantity{quantity}, tif);
}

Order market_buy(OrderId::Rep id, Quantity::Rep quantity,
                 TimeInForce tif = TimeInForce::ImmediateOrCancel) {
    return Order::market(OrderId{id}, Side::Buy, Quantity{quantity}, tif);
}

Order market_sell(OrderId::Rep id, Quantity::Rep quantity,
                  TimeInForce tif = TimeInForce::ImmediateOrCancel) {
    return Order::market(OrderId{id}, Side::Sell, Quantity{quantity}, tif);
}

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

/// A book with three ask levels: 30 at 100, 40 at 101, 50 at 105.
void build_asks(MatchingEngine& engine) {
    ASSERT_TRUE(engine.submit(limit_sell(101, 100, 30), ignore_events).accepted());
    ASSERT_TRUE(engine.submit(limit_sell(102, 101, 40), ignore_events).accepted());
    ASSERT_TRUE(engine.submit(limit_sell(103, 105, 50), ignore_events).accepted());
}

// ---------------------------------------------------------------------------
// Market orders and protection
// ---------------------------------------------------------------------------

// With a band of 10 and a best ask of 100, the order may trade up to 105. All
// three levels are inside that, so it fills completely.
TEST(MarketOrder, TradesThroughLevelsInsideTheProtectionPrice) {
    MatchingEngine engine{EngineConfig{.market_protection_ticks = 10}};
    build_asks(engine);

    const Submitted submitted = send(engine, market_buy(1, 120));

    ASSERT_EQ(submitted.trades.size(), 3U);
    EXPECT_EQ(submitted.trades[0].price, Price{100});
    EXPECT_EQ(submitted.trades[1].price, Price{101});
    EXPECT_EQ(submitted.trades[2].price, Price{105});
    EXPECT_EQ(submitted.result.filled, Quantity{120});
    EXPECT_EQ(submitted.result.cancelled, Quantity{});
    EXPECT_TRUE(engine.book().empty());
}

// With a band of 2 the effective limit is 102, so the level at 105 is out of
// reach. The order takes 70 and cancels the other 50.
TEST(MarketOrder, StopsAtTheProtectionPriceAndCancelsTheRest) {
    MatchingEngine engine{EngineConfig{.market_protection_ticks = 2}};
    build_asks(engine);

    const Submitted submitted = send(engine, market_buy(1, 120));

    ASSERT_EQ(submitted.trades.size(), 2U);
    EXPECT_EQ(submitted.trades[0].price, Price{100});
    EXPECT_EQ(submitted.trades[1].price, Price{101});
    EXPECT_EQ(submitted.result.filled, Quantity{70});
    EXPECT_EQ(submitted.result.cancelled, Quantity{50});
    EXPECT_EQ(submitted.result.resting, Quantity{});

    // The level beyond protection is untouched.
    EXPECT_EQ(engine.book().quantity_at(Side::Sell, Price{105}), Quantity{50});
}

// Protection is measured from the touch at the moment the order arrives, not
// from each level as the sweep progresses. A band of 2 off a touch of 100 allows
// 102, and stays 102 even after the 100 level is consumed.
TEST(MarketOrder, ProtectionIsMeasuredFromTheTouchAtArrival) {
    MatchingEngine engine{EngineConfig{.market_protection_ticks = 2}};
    ASSERT_TRUE(engine.submit(limit_sell(101, 100, 10), ignore_events).accepted());
    ASSERT_TRUE(engine.submit(limit_sell(102, 103, 10), ignore_events).accepted());

    const Submitted submitted = send(engine, market_buy(1, 20));

    ASSERT_EQ(submitted.trades.size(), 1U);
    EXPECT_EQ(submitted.trades[0].price, Price{100});
    EXPECT_EQ(submitted.result.cancelled, Quantity{10});
}

TEST(MarketOrder, SellingMirrorsBuyingAgainstTheBids) {
    MatchingEngine engine{EngineConfig{.market_protection_ticks = 2}};
    ASSERT_TRUE(engine.submit(limit_buy(101, 100, 30), ignore_events).accepted());
    ASSERT_TRUE(engine.submit(limit_buy(102, 99, 30), ignore_events).accepted());
    ASSERT_TRUE(engine.submit(limit_buy(103, 95, 30), ignore_events).accepted());

    const Submitted submitted = send(engine, market_sell(1, 90));

    // Effective limit is 100 - 2 = 98, so the bid at 95 is out of reach.
    ASSERT_EQ(submitted.trades.size(), 2U);
    EXPECT_EQ(submitted.trades[0].price, Price{100});
    EXPECT_EQ(submitted.trades[1].price, Price{99});
    EXPECT_EQ(submitted.result.filled, Quantity{60});
    EXPECT_EQ(submitted.result.cancelled, Quantity{30});
}

// With nothing resting opposite there is no touch to measure protection from,
// and nothing to trade against either.
TEST(MarketOrder, IsCancelledEntirelyWhenTheOppositeSideIsEmpty) {
    MatchingEngine engine;
    ASSERT_TRUE(engine.submit(limit_buy(101, 100, 50), ignore_events).accepted());

    const Submitted submitted = send(engine, market_buy(1, 50));

    EXPECT_TRUE(submitted.trades.empty());
    EXPECT_EQ(submitted.result.cancelled, Quantity{50});
    EXPECT_EQ(submitted.result.filled, Quantity{});
    EXPECT_EQ(submitted.result.resting, Quantity{});
    // The unrelated bid is untouched.
    EXPECT_EQ(engine.book().size(), 1U);
}

// A market order has no price of its own, so it can never rest.
TEST(MarketOrder, NeverRests) {
    MatchingEngine engine{EngineConfig{.market_protection_ticks = 1}};
    ASSERT_TRUE(engine.submit(limit_sell(101, 100, 10), ignore_events).accepted());

    const Submitted submitted = send(engine, market_buy(1, 100));

    EXPECT_EQ(submitted.result.resting, Quantity{});
    EXPECT_EQ(submitted.result.cancelled, Quantity{90});
    EXPECT_FALSE(engine.book().contains(OrderId{1}));
    EXPECT_FALSE(engine.book().best_bid().has_value());
}

// Adding the band to a touch near the top of the price range must saturate.
// Signed overflow is undefined behaviour, so without the clamp this is a real
// bug that UBSan would trip on.
TEST(MarketOrder, ProtectionSaturatesAtTheEndsOfThePriceRange) {
    constexpr auto kHighest = std::numeric_limits<Price::Rep>::max();
    constexpr auto kLowest = std::numeric_limits<Price::Rep>::min();

    MatchingEngine buying{EngineConfig{.market_protection_ticks = 1000}};
    ASSERT_TRUE(buying.submit(limit_sell(101, kHighest, 10), ignore_events).accepted());
    EXPECT_EQ(send(buying, market_buy(1, 10)).result.filled, Quantity{10});

    MatchingEngine selling{EngineConfig{.market_protection_ticks = 1000}};
    ASSERT_TRUE(selling.submit(limit_buy(102, kLowest, 10), ignore_events).accepted());
    EXPECT_EQ(send(selling, market_sell(2, 10)).result.filled, Quantity{10});
}

// A band of zero means the order may only trade at the touch itself.
TEST(MarketOrder, AZeroBandAllowsOnlyTheTouchPrice) {
    MatchingEngine engine{EngineConfig{.market_protection_ticks = 0}};
    build_asks(engine);

    const Submitted submitted = send(engine, market_buy(1, 120));

    ASSERT_EQ(submitted.trades.size(), 1U);
    EXPECT_EQ(submitted.trades[0].price, Price{100});
    EXPECT_EQ(submitted.result.filled, Quantity{30});
    EXPECT_EQ(submitted.result.cancelled, Quantity{90});
}

// ---------------------------------------------------------------------------
// ImmediateOrCancel
// ---------------------------------------------------------------------------

TEST(ImmediateOrCancel, TradesWhatItCanAndCancelsTheRest) {
    MatchingEngine engine;
    ASSERT_TRUE(engine.submit(limit_sell(101, 100, 30), ignore_events).accepted());

    const Submitted submitted = send(engine, limit_buy(1, 100, 50, TimeInForce::ImmediateOrCancel));

    ASSERT_EQ(submitted.trades.size(), 1U);
    EXPECT_EQ(submitted.result.filled, Quantity{30});
    EXPECT_EQ(submitted.result.cancelled, Quantity{20});
    EXPECT_EQ(submitted.result.resting, Quantity{});
    EXPECT_FALSE(engine.book().contains(OrderId{1}));
    EXPECT_FALSE(engine.book().best_bid().has_value());
}

TEST(ImmediateOrCancel, ReportsNothingCancelledWhenItFillsCompletely) {
    MatchingEngine engine;
    ASSERT_TRUE(engine.submit(limit_sell(101, 100, 50), ignore_events).accepted());

    const Submitted submitted = send(engine, limit_buy(1, 100, 50, TimeInForce::ImmediateOrCancel));

    EXPECT_EQ(submitted.result.filled, Quantity{50});
    EXPECT_EQ(submitted.result.cancelled, Quantity{});
}

// Nothing crosses, so an IOC order leaves no trace.
TEST(ImmediateOrCancel, LeavesTheBookUnchangedWhenNothingCrosses) {
    MatchingEngine engine;
    ASSERT_TRUE(engine.submit(limit_sell(101, 101, 50), ignore_events).accepted());

    const Submitted submitted = send(engine, limit_buy(1, 100, 50, TimeInForce::ImmediateOrCancel));

    EXPECT_TRUE(submitted.trades.empty());
    EXPECT_EQ(submitted.result.cancelled, Quantity{50});
    EXPECT_EQ(engine.book().size(), 1U);
    EXPECT_FALSE(engine.book().best_bid().has_value());
}

// ---------------------------------------------------------------------------
// FillOrKill
// ---------------------------------------------------------------------------

TEST(FillOrKill, FillsCompletelyWhenEnoughLiquidityExists) {
    MatchingEngine engine;
    ASSERT_TRUE(engine.submit(limit_sell(101, 100, 30), ignore_events).accepted());
    ASSERT_TRUE(engine.submit(limit_sell(102, 101, 30), ignore_events).accepted());

    const Submitted submitted = send(engine, limit_buy(1, 101, 60, TimeInForce::FillOrKill));

    ASSERT_EQ(submitted.trades.size(), 2U);
    EXPECT_EQ(submitted.result.filled, Quantity{60});
    EXPECT_EQ(submitted.result.cancelled, Quantity{});
    EXPECT_TRUE(engine.book().empty());
}

// The important case. Falling short must emit no trades at all, because a trade
// handed to the sink cannot be withdrawn.
TEST(FillOrKill, EmitsNoTradesAndChangesNothingWhenItCannotFill) {
    MatchingEngine engine;
    ASSERT_TRUE(engine.submit(limit_sell(101, 100, 30), ignore_events).accepted());

    const Submitted submitted = send(engine, limit_buy(1, 100, 50, TimeInForce::FillOrKill));

    EXPECT_TRUE(submitted.trades.empty());
    EXPECT_EQ(submitted.result.filled, Quantity{});
    EXPECT_EQ(submitted.result.cancelled, Quantity{50});
    EXPECT_EQ(engine.book().quantity_at(Side::Sell, Price{100}), Quantity{30});
    EXPECT_EQ(engine.book().size(), 1U);
}

// Exactly enough must fill; one short must not. This pins the comparison.
TEST(FillOrKill, ExactlyEnoughFillsAndOneShortDoesNot) {
    MatchingEngine exact;
    ASSERT_TRUE(exact.submit(limit_sell(101, 100, 50), ignore_events).accepted());
    EXPECT_EQ(send(exact, limit_buy(1, 100, 50, TimeInForce::FillOrKill)).result.filled,
              Quantity{50});

    MatchingEngine short_by_one;
    ASSERT_TRUE(short_by_one.submit(limit_sell(102, 100, 49), ignore_events).accepted());
    EXPECT_TRUE(send(short_by_one, limit_buy(2, 100, 50, TimeInForce::FillOrKill)).trades.empty());
}

// Liquidity priced outside the order's limit does not count towards fillability.
TEST(FillOrKill, IgnoresLiquidityBeyondItsLimitPrice) {
    MatchingEngine engine;
    ASSERT_TRUE(engine.submit(limit_sell(101, 100, 30), ignore_events).accepted());
    ASSERT_TRUE(engine.submit(limit_sell(102, 105, 30), ignore_events).accepted());

    // 60 available in total, but only 30 at or below the limit of 100.
    const Submitted submitted = send(engine, limit_buy(1, 100, 60, TimeInForce::FillOrKill));

    EXPECT_TRUE(submitted.trades.empty());
    EXPECT_EQ(submitted.result.cancelled, Quantity{60});
    EXPECT_EQ(engine.book().size(), 2U);
}

TEST(FillOrKill, WorksOnTheSellSideToo) {
    MatchingEngine engine;
    ASSERT_TRUE(engine.submit(limit_buy(101, 100, 30), ignore_events).accepted());

    EXPECT_TRUE(send(engine, limit_sell(1, 100, 50, TimeInForce::FillOrKill)).trades.empty());
    EXPECT_EQ(send(engine, limit_sell(2, 100, 30, TimeInForce::FillOrKill)).result.filled,
              Quantity{30});
}

// A market order can be FillOrKill. Protection bounds what counts as available.
TEST(FillOrKill, AppliesToMarketOrdersWithinProtection) {
    MatchingEngine engine{EngineConfig{.market_protection_ticks = 2}};
    build_asks(engine);

    // 120 rests in total, but only 70 sits at or below the protection price
    // of 102, so a market FOK for 120 cannot fill.
    EXPECT_TRUE(send(engine, market_buy(1, 120, TimeInForce::FillOrKill)).trades.empty());

    EXPECT_EQ(send(engine, market_buy(2, 70, TimeInForce::FillOrKill)).result.filled, Quantity{70});
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

// A market order cannot rest, so GoodTillCancel has no meaning for one.
TEST(OrderValidation, MarketOrderWithGoodTillCancelIsRejected) {
    MatchingEngine engine;

    const Order bad =
        Order::market(OrderId{1}, Side::Buy, Quantity{10}, TimeInForce::GoodTillCancel);

    EXPECT_FALSE(bad.is_valid());
    EXPECT_EQ(send(engine, bad).result.status, SubmitStatus::RejectedInvalid);
    EXPECT_TRUE(engine.book().empty());
}

TEST(OrderValidation, OutOfRangeTypeOrTimeInForceIsRejected) {
    MatchingEngine engine;

    const Order bad_type{OrderId{1},
                         Side::Buy,
                         Price{100},
                         Quantity{10},
                         static_cast<OrderType>(7),
                         TimeInForce::GoodTillCancel};
    const Order bad_tif{OrderId{2},   Side::Buy,        Price{100},
                        Quantity{10}, OrderType::Limit, static_cast<TimeInForce>(9)};

    EXPECT_EQ(send(engine, bad_type).result.status, SubmitStatus::RejectedInvalid);
    EXPECT_EQ(send(engine, bad_tif).result.status, SubmitStatus::RejectedInvalid);
    EXPECT_TRUE(engine.book().empty());
}

// ---------------------------------------------------------------------------
// Conservation across mixed order types
// ---------------------------------------------------------------------------

// Randomised flow mixing limit and market orders across all three time-in-force
// values. After every submission: quantities add up, nothing rests that should
// not have, and the book is never left crossed.
TEST(MixedFlowInvariants, QuantitiesAddUpAndTheBookIsNeverCrossed) {
    constexpr int kSteps = 3000;

    std::mt19937 rng{20260610U};
    std::uniform_int_distribution<Price::Rep> price_dist{98, 102};
    std::uniform_int_distribution<Quantity::Rep> quantity_dist{1, 60};
    std::uniform_int_distribution<int> pick{0, 99};

    MatchingEngine engine{EngineConfig{.market_protection_ticks = 3}};
    Quantity total_traded{};
    Quantity total_cancelled{};

    for (int step = 0; step < kSteps; ++step) {
        const auto id = static_cast<OrderId::Rep>(step + 1);
        const Side side = pick(rng) % 2 == 0 ? Side::Buy : Side::Sell;
        const Quantity quantity{quantity_dist(rng)};

        // Mostly resting limit orders, with market and IOC/FOK mixed in, which
        // is roughly the shape of real flow.
        const int roll = pick(rng);
        Order order = limit_buy(id, price_dist(rng), quantity.value());
        if (roll < 15) {
            order = Order::market(OrderId{id}, side, quantity, TimeInForce::ImmediateOrCancel);
        } else if (roll < 25) {
            order = Order::market(OrderId{id}, side, quantity, TimeInForce::FillOrKill);
        } else if (roll < 45) {
            order = Order::limit(OrderId{id}, side, Price{price_dist(rng)}, quantity,
                                 TimeInForce::ImmediateOrCancel);
        } else if (roll < 60) {
            order = Order::limit(OrderId{id}, side, Price{price_dist(rng)}, quantity,
                                 TimeInForce::FillOrKill);
        } else {
            order = Order::limit(OrderId{id}, side, Price{price_dist(rng)}, quantity);
        }

        EventRecorder recorder;
        const SubmitResult result = engine.submit(order, recorder);
        ASSERT_TRUE(result.accepted()) << "step " << step << ' ' << order;

        ASSERT_EQ(result.filled + result.resting + result.cancelled, order.quantity())
            << "step " << step << ' ' << order;

        Quantity from_trades{};
        for (const Trade& trade : recorder.trades()) {
            from_trades += trade.quantity;
        }
        ASSERT_EQ(from_trades, result.filled) << "step " << step;

        // Only GoodTillCancel limit orders may leave anything resting.
        if (order.time_in_force() != TimeInForce::GoodTillCancel) {
            ASSERT_EQ(result.resting, Quantity{}) << "step " << step << ' ' << order;
            ASSERT_FALSE(engine.book().contains(order.id())) << "step " << step;
        }

        // FillOrKill is all or nothing.
        if (order.time_in_force() == TimeInForce::FillOrKill) {
            ASSERT_TRUE(result.filled == order.quantity() || result.filled == Quantity{})
                << "step " << step << ' ' << result;
        }

        const auto bid = engine.book().best_bid();
        const auto ask = engine.book().best_ask();
        if (bid.has_value() && ask.has_value()) {
            ASSERT_LT(*bid, *ask) << "crossed book after step " << step;
        }

        total_traded += from_trades;
        total_cancelled += result.cancelled;
    }

    // The run must actually have exercised trading and cancelling, or the
    // invariants above were checked against an idle engine.
    EXPECT_GT(total_traded, Quantity{1000});
    EXPECT_GT(total_cancelled, Quantity{1000});
}

}  // namespace
}  // namespace flashpoint
