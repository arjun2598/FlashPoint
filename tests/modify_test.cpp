// Tests for modifying resting orders.
//
// Queue priority is the whole point of this milestone:
//
//   * Reducing quantity at the same price keeps the order's place in line.
//   * Increasing it, or changing the price, sends the order to the back.
//
// Aggregate depth looks the same either way, so a bug here is invisible unless
// the tests check who fills next. Every priority case below does.
//
// The other thing worth pinning is that a modify which reprices an order across
// the spread trades, exactly as a fresh order at that price would.

#include "flashpoint/matching_engine.hpp"

#include "flashpoint/order.hpp"
#include "flashpoint/order_book.hpp"
#include "flashpoint/ostream.hpp"
#include "flashpoint/trade.hpp"

#include "flashpoint/types.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <random>
#include <utility>
#include <vector>

namespace flashpoint {
namespace {

using testing_support::EventRecorder;
using testing_support::ignore_events;

[[nodiscard]] SubmitResult rest_buy(MatchingEngine& engine, OrderId::Rep id, Price::Rep price,
                                    Quantity::Rep quantity) {
    return engine.submit(Order::limit(OrderId{id}, Side::Buy, Price{price}, Quantity{quantity}),
                         ignore_events);
}

[[nodiscard]] SubmitResult rest_sell(MatchingEngine& engine, OrderId::Rep id, Price::Rep price,
                                     Quantity::Rep quantity) {
    return engine.submit(Order::limit(OrderId{id}, Side::Sell, Price{price}, Quantity{quantity}),
                         ignore_events);
}

struct Amended {
    ModifyResult result;
    std::vector<Trade> trades;
};

[[nodiscard]] Amended amend(MatchingEngine& engine, OrderId::Rep id, Price::Rep price,
                            Quantity::Rep quantity) {
    EventRecorder recorder;
    const ModifyResult result =
        engine.modify(OrderId{id}, Price{price}, Quantity{quantity}, recorder);
    return Amended{result, recorder.trades()};
}

/// Two sells at 100: #101 first with 50, #102 second with 30.
void build_queue(MatchingEngine& engine) {
    ASSERT_TRUE(rest_sell(engine, 101, 100, 50).accepted());
    ASSERT_TRUE(rest_sell(engine, 102, 100, 30).accepted());
    ASSERT_EQ(engine.book().front_at(Side::Sell, Price{100}), OrderId{101});
}

// ---------------------------------------------------------------------------
// Priority retained
// ---------------------------------------------------------------------------

// Shrinking takes nothing from the orders behind, so the order keeps its place.
TEST(Modify, ReducingQuantityAtTheSamePriceKeepsPriority) {
    MatchingEngine engine;
    build_queue(engine);

    const Amended amended = amend(engine, 101, 100, 20);

    EXPECT_EQ(amended.result, (ModifyResult{ModifyStatus::Modified, Quantity{}, Quantity{20},
                                            QueuePriority::Retained}));
    EXPECT_TRUE(amended.trades.empty());

    // Still at the front, with the new size.
    EXPECT_EQ(engine.book().front_at(Side::Sell, Price{100}), OrderId{101});
    EXPECT_EQ(engine.book().remaining_of(OrderId{101}), Quantity{20});
    EXPECT_EQ(engine.book().quantity_at(Side::Sell, Price{100}), Quantity{50});
    EXPECT_EQ(engine.book().order_count_at(Side::Sell, Price{100}), 2U);
}

// Priority is only meaningful if it changes who trades next, so check that too.
TEST(Modify, AnOrderThatKeptPriorityStillFillsFirst) {
    MatchingEngine engine;
    build_queue(engine);

    ASSERT_EQ(amend(engine, 101, 100, 20).result.priority, QueuePriority::Retained);

    EventRecorder recorder;
    const SubmitResult result =
        engine.submit(Order::limit(OrderId{1}, Side::Buy, Price{100}, Quantity{20}), recorder);

    ASSERT_TRUE(result.accepted());
    const std::vector<Trade> trades = recorder.trades();
    ASSERT_EQ(trades.size(), 1U);
    EXPECT_EQ(trades[0].maker_id, OrderId{101});
}

TEST(Modify, LeavingQuantityUnchangedAtTheSamePriceKeepsPriority) {
    MatchingEngine engine;
    build_queue(engine);

    const Amended amended = amend(engine, 101, 100, 50);

    EXPECT_EQ(amended.result.priority, QueuePriority::Retained);
    EXPECT_EQ(engine.book().front_at(Side::Sell, Price{100}), OrderId{101});
    EXPECT_EQ(engine.book().remaining_of(OrderId{101}), Quantity{50});
}

// ---------------------------------------------------------------------------
// Priority lost
// ---------------------------------------------------------------------------

// Growing an order adds size ahead of people already waiting, so it goes back.
TEST(Modify, IncreasingQuantityLosesPriority) {
    MatchingEngine engine;
    build_queue(engine);

    const Amended amended = amend(engine, 101, 100, 80);

    EXPECT_EQ(amended.result, (ModifyResult{ModifyStatus::Modified, Quantity{}, Quantity{80},
                                            QueuePriority::Lost}));

    // #102 is now in front.
    EXPECT_EQ(engine.book().front_at(Side::Sell, Price{100}), OrderId{102});
    EXPECT_EQ(engine.book().remaining_of(OrderId{101}), Quantity{80});
    EXPECT_EQ(engine.book().quantity_at(Side::Sell, Price{100}), Quantity{110});
    EXPECT_EQ(engine.book().order_count_at(Side::Sell, Price{100}), 2U);
}

TEST(Modify, AnOrderThatLostPriorityFillsSecond) {
    MatchingEngine engine;
    build_queue(engine);

    ASSERT_EQ(amend(engine, 101, 100, 80).result.priority, QueuePriority::Lost);

    EventRecorder recorder;
    const SubmitResult result =
        engine.submit(Order::limit(OrderId{1}, Side::Buy, Price{100}, Quantity{30}), recorder);

    ASSERT_TRUE(result.accepted());
    const std::vector<Trade> trades = recorder.trades();
    ASSERT_EQ(trades.size(), 1U);
    EXPECT_EQ(trades[0].maker_id, OrderId{102});
}

TEST(Modify, ChangingPriceLosesPriorityEvenWhenTheQuantityShrinks) {
    MatchingEngine engine;
    build_queue(engine);

    const Amended amended = amend(engine, 101, 101, 20);

    EXPECT_EQ(amended.result.priority, QueuePriority::Lost);
    EXPECT_EQ(engine.book().quantity_at(Side::Sell, Price{100}), Quantity{30});
    EXPECT_EQ(engine.book().quantity_at(Side::Sell, Price{101}), Quantity{20});
    EXPECT_EQ(engine.book().front_at(Side::Sell, Price{100}), OrderId{102});
    EXPECT_EQ(engine.book().front_at(Side::Sell, Price{101}), OrderId{101});
}

// Moving to a new price puts the order at the back of that queue, not the front.
TEST(Modify, RepricingJoinsTheBackOfTheQueueAtTheNewPrice) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_sell(engine, 101, 100, 10).accepted());
    ASSERT_TRUE(rest_sell(engine, 201, 101, 10).accepted());
    ASSERT_TRUE(rest_sell(engine, 202, 101, 10).accepted());

    ASSERT_EQ(amend(engine, 101, 101, 10).result.priority, QueuePriority::Lost);

    EXPECT_EQ(engine.book().front_at(Side::Sell, Price{101}), OrderId{201});
    EXPECT_EQ(engine.book().order_count_at(Side::Sell, Price{101}), 3U);
    EXPECT_FALSE(engine.book().best_ask().has_value() && *engine.book().best_ask() == Price{100});
}

TEST(Modify, KeepsTheOrdersSideWhenItReprices) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_buy(engine, 1, 100, 50).accepted());

    ASSERT_TRUE(amend(engine, 1, 98, 50).result.modified());

    const auto resting = engine.book().resting_order(OrderId{1});
    ASSERT_TRUE(resting.has_value());
    EXPECT_EQ(resting->side, Side::Buy);
    EXPECT_EQ(resting->price, Price{98});
    EXPECT_EQ(engine.book().best_bid(), Price{98});
}

// ---------------------------------------------------------------------------
// Repricing across the spread
// ---------------------------------------------------------------------------

// A repriced order is a new order at that price, so it trades if it crosses.
TEST(Modify, RepricingAcrossTheSpreadTrades) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_sell(engine, 101, 100, 30).accepted());
    ASSERT_TRUE(rest_buy(engine, 1, 98, 50).accepted());

    const Amended amended = amend(engine, 1, 100, 50);

    ASSERT_EQ(amended.trades.size(), 1U);
    EXPECT_EQ(amended.trades[0].maker_id, OrderId{101});
    EXPECT_EQ(amended.trades[0].taker_id, OrderId{1});
    EXPECT_EQ(amended.trades[0].price, Price{100});
    EXPECT_EQ(amended.trades[0].quantity, Quantity{30});

    EXPECT_EQ(amended.result.filled, Quantity{30});
    EXPECT_EQ(amended.result.resting, Quantity{20});
    EXPECT_EQ(amended.result.priority, QueuePriority::Lost);
    EXPECT_EQ(engine.book().best_bid(), Price{100});
}

TEST(Modify, RepricingCanFillTheOrderCompletely) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_sell(engine, 101, 100, 50).accepted());
    ASSERT_TRUE(rest_buy(engine, 1, 98, 50).accepted());

    const Amended amended = amend(engine, 1, 100, 50);

    EXPECT_EQ(amended.result.filled, Quantity{50});
    EXPECT_EQ(amended.result.resting, Quantity{});
    EXPECT_TRUE(engine.book().empty());
    EXPECT_FALSE(engine.book().contains(OrderId{1}));
}

// The engine must not leave a crossed book behind a modify any more than behind
// a submit.
TEST(Modify, NeverLeavesTheBookCrossed) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_sell(engine, 101, 100, 30).accepted());
    ASSERT_TRUE(rest_buy(engine, 1, 98, 100).accepted());

    ASSERT_TRUE(amend(engine, 1, 105, 100).result.modified());

    const auto bid = engine.book().best_bid();
    const auto ask = engine.book().best_ask();
    ASSERT_TRUE(bid.has_value());
    EXPECT_FALSE(ask.has_value());
    EXPECT_EQ(*bid, Price{105});
}

// ---------------------------------------------------------------------------
// Rejections
// ---------------------------------------------------------------------------

TEST(Modify, UnknownOrderChangesNothing) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_buy(engine, 1, 100, 50).accepted());

    const Amended amended = amend(engine, 99, 101, 10);

    EXPECT_EQ(amended.result.status, ModifyStatus::UnknownOrder);
    EXPECT_TRUE(amended.trades.empty());
    EXPECT_EQ(engine.book().size(), 1U);
    EXPECT_EQ(engine.book().quantity_at(Side::Buy, Price{100}), Quantity{50});
}

// Modifying to zero would delete the order, which is what cancel is for.
TEST(Modify, ZeroQuantityIsRejectedAndTheOrderSurvives) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_buy(engine, 1, 100, 50).accepted());

    const Amended amended = amend(engine, 1, 100, 0);

    EXPECT_EQ(amended.result.status, ModifyStatus::RejectedInvalidQuantity);
    EXPECT_EQ(engine.book().remaining_of(OrderId{1}), Quantity{50});
}

TEST(Modify, TheReservedOrderIdIsRejectedAsMalformed) {
    MatchingEngine engine;

    EXPECT_EQ(amend(engine, OrderId::kNone, 100, 10).result.status,
              ModifyStatus::RejectedInvalidId);
}

TEST(Modify, AnAlreadyCancelledOrderCannotBeModified) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_buy(engine, 1, 100, 50).accepted());
    ASSERT_TRUE(engine.cancel(OrderId{1}, ignore_events).succeeded());

    EXPECT_EQ(amend(engine, 1, 101, 10).result.status, ModifyStatus::UnknownOrder);
    EXPECT_TRUE(engine.book().empty());
}

// ---------------------------------------------------------------------------
// Interaction with partial fills
// ---------------------------------------------------------------------------

// The new quantity is the new remaining, not a new total. An order with 20 left
// that is modified to 25 rests 25, whatever it filled before.
TEST(Modify, NewQuantityIsTheNewRemainingNotANewTotal) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_sell(engine, 101, 100, 50).accepted());
    ASSERT_TRUE(rest_buy(engine, 1, 100, 30).accepted());
    ASSERT_EQ(engine.book().remaining_of(OrderId{101}), Quantity{20});

    // 25 is more than the 20 remaining, so this is an increase and loses priority.
    const Amended amended = amend(engine, 101, 100, 25);

    EXPECT_EQ(amended.result.resting, Quantity{25});
    EXPECT_EQ(amended.result.priority, QueuePriority::Lost);
    EXPECT_EQ(engine.book().remaining_of(OrderId{101}), Quantity{25});
}

// The comparison that decides priority is against what is left, not what was
// originally sent. 20 remaining modified to 15 is a reduction.
TEST(Modify, PriorityComparesAgainstTheRemainingQuantity) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_sell(engine, 101, 100, 50).accepted());
    ASSERT_TRUE(rest_sell(engine, 102, 100, 10).accepted());
    ASSERT_TRUE(rest_buy(engine, 1, 100, 30).accepted());
    ASSERT_EQ(engine.book().remaining_of(OrderId{101}), Quantity{20});

    // 15 is below the original 50 but also below the remaining 20, so it is a
    // reduction and keeps priority.
    const Amended amended = amend(engine, 101, 100, 15);

    EXPECT_EQ(amended.result.priority, QueuePriority::Retained);
    EXPECT_EQ(engine.book().front_at(Side::Sell, Price{100}), OrderId{101});
}

// ---------------------------------------------------------------------------
// Invariants under randomised flow
// ---------------------------------------------------------------------------

// Mixed submits, modifies and cancels. After every modify: the quantities add
// up, the priority flag matches the rule, the order is where it should be, and
// the book is never left crossed.
TEST(ModifyInvariants, PriorityRuleHoldsAndQuantitiesAddUp) {
    constexpr int kSteps = 3000;

    std::mt19937 rng{20260612U};
    std::uniform_int_distribution<Price::Rep> price_dist{98, 102};
    std::uniform_int_distribution<Quantity::Rep> quantity_dist{1, 60};
    std::uniform_int_distribution<int> roll{0, 99};

    MatchingEngine engine;
    std::vector<OrderId> resting;
    OrderId::Rep next_id = 1;
    int modifies_applied = 0;
    int retained = 0;
    int lost = 0;

    for (int step = 0; step < kSteps; ++step) {
        const int action = roll(rng);

        if (!resting.empty() && action < 40) {
            std::uniform_int_distribution<std::size_t> pick{0, resting.size() - 1};
            const std::size_t slot = pick(rng);
            const OrderId id = resting[slot];

            const auto before = engine.book().resting_order(id);
            const Price new_price{price_dist(rng)};
            const Quantity new_quantity{quantity_dist(rng)};

            const ModifyResult result = engine.modify(id, new_price, new_quantity, ignore_events);

            if (!before.has_value()) {
                // Filled since it was submitted, so there is nothing to modify.
                ASSERT_EQ(result.status, ModifyStatus::UnknownOrder) << "step " << step;
                resting[slot] = resting.back();
                resting.pop_back();
                continue;
            }

            ASSERT_EQ(result.status, ModifyStatus::Modified) << "step " << step;
            ASSERT_EQ(result.filled + result.resting, new_quantity) << "step " << step;

            const bool should_keep =
                new_price == before->price && new_quantity <= before->remaining;
            ASSERT_EQ(result.priority, should_keep ? QueuePriority::Retained : QueuePriority::Lost)
                << "step " << step;

            if (should_keep) {
                ++retained;
                // A retained modify never trades, since the price did not move.
                ASSERT_EQ(result.filled, Quantity{}) << "step " << step;
            } else {
                ++lost;
            }
            ++modifies_applied;

            if (result.resting == Quantity{}) {
                resting[slot] = resting.back();
                resting.pop_back();
            }
        } else if (!resting.empty() && action < 55) {
            std::uniform_int_distribution<std::size_t> pick{0, resting.size() - 1};
            const std::size_t slot = pick(rng);
            const OrderId id = resting[slot];
            resting[slot] = resting.back();
            resting.pop_back();
            const CancelResult result = engine.cancel(id, ignore_events);
            ASSERT_TRUE(result.succeeded() || result.status == CancelStatus::UnknownOrder)
                << "step " << step;
        } else {
            const auto id = OrderId{next_id++};
            const Side side = roll(rng) % 2 == 0 ? Side::Buy : Side::Sell;
            const Order order =
                Order::limit(id, side, Price{price_dist(rng)}, Quantity{quantity_dist(rng)});
            const SubmitResult result = engine.submit(order, ignore_events);
            ASSERT_TRUE(result.accepted()) << "step " << step;
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

    // Both branches of the priority rule must actually have been exercised.
    EXPECT_GT(modifies_applied, 200);
    EXPECT_GT(retained, 50);
    EXPECT_GT(lost, 50);
}

}  // namespace
}  // namespace flashpoint
