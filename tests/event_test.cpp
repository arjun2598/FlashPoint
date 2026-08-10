// Tests for the engine's event stream.
//
// What matters:
//
//   * The sequence of event types each operation produces, in order.
//   * Sequence numbers start at 1 and never skip, so a consumer can detect a
//     gap.
//   * A modify publishes Modified, not a second Accepted. The client never
//     resubmitted the order, and a stream that said otherwise would be lying.
//   * Each event type carries the fields its contract promises.

#include "flashpoint/event.hpp"

#include "flashpoint/matching_engine.hpp"
#include "flashpoint/order.hpp"
#include "flashpoint/ostream.hpp"
#include "flashpoint/trade.hpp"
#include "flashpoint/types.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace flashpoint {
namespace {

using testing_support::EventRecorder;
using testing_support::ignore_events;

[[nodiscard]] SubmitResult rest_sell(MatchingEngine& engine, OrderId::Rep id, Price::Rep price,
                                     Quantity::Rep quantity) {
    return engine.submit(Order::limit(OrderId{id}, Side::Sell, Price{price}, Quantity{quantity}),
                         ignore_events);
}

[[nodiscard]] SubmitResult rest_buy(MatchingEngine& engine, OrderId::Rep id, Price::Rep price,
                                    Quantity::Rep quantity) {
    return engine.submit(Order::limit(OrderId{id}, Side::Buy, Price{price}, Quantity{quantity}),
                         ignore_events);
}

// ---------------------------------------------------------------------------
// Which events each operation publishes
// ---------------------------------------------------------------------------

TEST(EventStream, AnOrderThatJustRestsPublishesOnlyAccepted) {
    MatchingEngine engine;
    EventRecorder recorder;

    ASSERT_TRUE(
        engine.submit(Order::limit(OrderId{1}, Side::Buy, Price{100}, Quantity{50}), recorder)
            .accepted());

    EXPECT_EQ(recorder.types(), (std::vector<EventType>{EventType::Accepted}));
}

// Acknowledgement first, then the executions, the way a FIX New is followed by
// fills.
TEST(EventStream, AnOrderThatTradesPublishesAcceptedThenEachTrade) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_sell(engine, 101, 100, 20).accepted());
    ASSERT_TRUE(rest_sell(engine, 102, 101, 20).accepted());

    EventRecorder recorder;
    ASSERT_TRUE(
        engine.submit(Order::limit(OrderId{1}, Side::Buy, Price{101}, Quantity{40}), recorder)
            .accepted());

    EXPECT_EQ(recorder.types(),
              (std::vector<EventType>{EventType::Accepted, EventType::Trade, EventType::Trade}));
}

TEST(EventStream, AnImmediateOrCancelRemainderPublishesCancelledLast) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_sell(engine, 101, 100, 20).accepted());

    EventRecorder recorder;
    ASSERT_TRUE(engine
                    .submit(Order::limit(OrderId{1}, Side::Buy, Price{100}, Quantity{50},
                                         TimeInForce::ImmediateOrCancel),
                            recorder)
                    .accepted());

    EXPECT_EQ(recorder.types(), (std::vector<EventType>{EventType::Accepted, EventType::Trade,
                                                        EventType::Cancelled}));
    const std::vector<Event> cancelled = recorder.of_type(EventType::Cancelled);
    ASSERT_EQ(cancelled.size(), 1U);
    EXPECT_EQ(cancelled[0].quantity, Quantity{30});
}

// Fill-or-kill decides before it trades, so a failure shows no Trade at all.
TEST(EventStream, AFillOrKillThatCannotFillPublishesAcceptedThenCancelled) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_sell(engine, 101, 100, 20).accepted());

    EventRecorder recorder;
    ASSERT_TRUE(engine
                    .submit(Order::limit(OrderId{1}, Side::Buy, Price{100}, Quantity{50},
                                         TimeInForce::FillOrKill),
                            recorder)
                    .accepted());

    EXPECT_EQ(recorder.types(),
              (std::vector<EventType>{EventType::Accepted, EventType::Cancelled}));
    EXPECT_EQ(recorder.of_type(EventType::Cancelled)[0].quantity, Quantity{50});
}

TEST(EventStream, AMarketOrderWithNothingOppositePublishesAcceptedThenCancelled) {
    MatchingEngine engine;
    EventRecorder recorder;

    ASSERT_TRUE(
        engine.submit(Order::market(OrderId{1}, Side::Buy, Quantity{50}), recorder).accepted());

    EXPECT_EQ(recorder.types(),
              (std::vector<EventType>{EventType::Accepted, EventType::Cancelled}));
}

TEST(EventStream, CancelPublishesCancelled) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_buy(engine, 1, 100, 50).accepted());

    EventRecorder recorder;
    ASSERT_TRUE(engine.cancel(OrderId{1}, recorder).succeeded());

    ASSERT_EQ(recorder.types(), (std::vector<EventType>{EventType::Cancelled}));
    EXPECT_EQ(recorder.events[0].order_id, OrderId{1});
    EXPECT_EQ(recorder.events[0].quantity, Quantity{50});
    EXPECT_EQ(recorder.events[0].price, Price{100});
    EXPECT_EQ(recorder.events[0].side, Side::Buy);
}

// ---------------------------------------------------------------------------
// Rejections
// ---------------------------------------------------------------------------

TEST(EventStream, AMalformedOrderPublishesRejectedAndNothingElse) {
    MatchingEngine engine;
    EventRecorder recorder;

    const SubmitResult result =
        engine.submit(Order::limit(OrderId{1}, Side::Buy, Price{100}, Quantity{0}), recorder);

    EXPECT_EQ(result.status, SubmitStatus::RejectedInvalid);
    ASSERT_EQ(recorder.types(), (std::vector<EventType>{EventType::Rejected}));
    EXPECT_EQ(recorder.events[0].reason, RejectReason::MalformedOrder);
    EXPECT_EQ(recorder.events[0].order_id, OrderId{1});
}

TEST(EventStream, ADuplicateIdPublishesRejectedWithThatReason) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_buy(engine, 1, 100, 50).accepted());

    EventRecorder recorder;
    const SubmitResult result =
        engine.submit(Order::limit(OrderId{1}, Side::Buy, Price{101}, Quantity{10}), recorder);

    EXPECT_EQ(result.status, SubmitStatus::RejectedDuplicateId);
    ASSERT_EQ(recorder.events.size(), 1U);
    EXPECT_EQ(recorder.events[0].reason, RejectReason::DuplicateOrderId);
}

TEST(EventStream, AMissedCancelPublishesRejectedUnknownOrder) {
    MatchingEngine engine;
    EventRecorder recorder;

    EXPECT_FALSE(engine.cancel(OrderId{99}, recorder).succeeded());

    ASSERT_EQ(recorder.events.size(), 1U);
    EXPECT_EQ(recorder.events[0].type, EventType::Rejected);
    EXPECT_EQ(recorder.events[0].reason, RejectReason::UnknownOrder);
}

TEST(EventStream, TheReservedIdPublishesRejectedInvalidOrderId) {
    MatchingEngine engine;
    EventRecorder recorder;

    EXPECT_FALSE(engine.cancel(OrderId{OrderId::kNone}, recorder).succeeded());
    EXPECT_FALSE(
        engine.modify(OrderId{OrderId::kNone}, Price{100}, Quantity{10}, recorder).modified());

    ASSERT_EQ(recorder.events.size(), 2U);
    EXPECT_EQ(recorder.events[0].reason, RejectReason::InvalidOrderId);
    EXPECT_EQ(recorder.events[1].reason, RejectReason::InvalidOrderId);
}

TEST(EventStream, AZeroQuantityModifyPublishesRejectedInvalidQuantity) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_buy(engine, 1, 100, 50).accepted());

    EventRecorder recorder;
    EXPECT_FALSE(engine.modify(OrderId{1}, Price{100}, Quantity{0}, recorder).modified());

    ASSERT_EQ(recorder.events.size(), 1U);
    EXPECT_EQ(recorder.events[0].reason, RejectReason::InvalidQuantity);
}

// ---------------------------------------------------------------------------
// Modify
// ---------------------------------------------------------------------------

// The subtle one. A modify that loses priority removes the order and puts it
// back, but the client did not resubmit anything, so the stream must not claim
// a second Accepted.
TEST(EventStream, ModifyPublishesModifiedAndNeverASecondAccepted) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_buy(engine, 1, 100, 50).accepted());

    EventRecorder recorder;
    ASSERT_TRUE(engine.modify(OrderId{1}, Price{99}, Quantity{80}, recorder).modified());

    EXPECT_EQ(recorder.types(), (std::vector<EventType>{EventType::Modified}));
    EXPECT_EQ(recorder.count_of(EventType::Accepted), 0U);
}

TEST(EventStream, ModifiedCarriesThePriorityOutcome) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_buy(engine, 1, 100, 50).accepted());

    EventRecorder retained;
    ASSERT_TRUE(engine.modify(OrderId{1}, Price{100}, Quantity{20}, retained).modified());
    ASSERT_EQ(retained.events.size(), 1U);
    EXPECT_EQ(retained.events[0].priority, QueuePriority::Retained);
    EXPECT_EQ(retained.events[0].quantity, Quantity{20});

    EventRecorder lost;
    ASSERT_TRUE(engine.modify(OrderId{1}, Price{99}, Quantity{20}, lost).modified());
    ASSERT_EQ(lost.events.size(), 1U);
    EXPECT_EQ(lost.events[0].priority, QueuePriority::Lost);
    EXPECT_EQ(lost.events[0].price, Price{99});
}

TEST(EventStream, ARepricingModifyPublishesModifiedThenItsTrades) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_sell(engine, 101, 100, 30).accepted());
    ASSERT_TRUE(rest_buy(engine, 1, 98, 50).accepted());

    EventRecorder recorder;
    ASSERT_TRUE(engine.modify(OrderId{1}, Price{100}, Quantity{50}, recorder).modified());

    EXPECT_EQ(recorder.types(), (std::vector<EventType>{EventType::Modified, EventType::Trade}));
}

// ---------------------------------------------------------------------------
// Event contents
// ---------------------------------------------------------------------------

TEST(EventStream, ATradeEventNamesBothSidesAndTheExecutionPrice) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_sell(engine, 101, 100, 30).accepted());

    EventRecorder recorder;
    ASSERT_TRUE(
        engine.submit(Order::limit(OrderId{1}, Side::Buy, Price{105}, Quantity{30}), recorder)
            .accepted());

    const std::vector<Event> trades = recorder.of_type(EventType::Trade);
    ASSERT_EQ(trades.size(), 1U);
    EXPECT_EQ(trades[0].order_id, OrderId{1});           // the taker
    EXPECT_EQ(trades[0].counterparty_id, OrderId{101});  // the maker
    EXPECT_EQ(trades[0].price, Price{100});              // the maker's price
    EXPECT_EQ(trades[0].quantity, Quantity{30});
    EXPECT_EQ(trades[0].side, Side::Buy);  // the aggressor
}

// to_trade() rebuilds the narrower value for a tape consumer.
TEST(EventStream, ATradeEventConvertsToATrade) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_sell(engine, 101, 100, 30).accepted());

    EventRecorder recorder;
    ASSERT_TRUE(
        engine.submit(Order::limit(OrderId{1}, Side::Buy, Price{100}, Quantity{30}), recorder)
            .accepted());

    const std::vector<Trade> trades = recorder.trades();
    ASSERT_EQ(trades.size(), 1U);
    EXPECT_EQ(trades[0], (Trade{.maker_id = OrderId{101},
                                .taker_id = OrderId{1},
                                .price = Price{100},
                                .quantity = Quantity{30},
                                .aggressor = Side::Buy}));
}

// A market order has no price of its own, so its events carry zero rather than
// a number the client never specified.
TEST(EventStream, MarketOrderEventsCarryNoPrice) {
    MatchingEngine engine;
    EventRecorder recorder;

    ASSERT_TRUE(
        engine.submit(Order::market(OrderId{1}, Side::Buy, Quantity{50}), recorder).accepted());

    ASSERT_EQ(recorder.events.size(), 2U);
    EXPECT_EQ(recorder.events[0].price, Price{});
    EXPECT_EQ(recorder.events[1].price, Price{});
}

// ---------------------------------------------------------------------------
// Sequence numbers
// ---------------------------------------------------------------------------

TEST(EventStream, SequenceNumbersStartAtOneAndIncreaseByOne) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_sell(engine, 101, 100, 20).accepted());
    ASSERT_TRUE(rest_sell(engine, 102, 101, 20).accepted());

    EventRecorder recorder;
    ASSERT_TRUE(
        engine.submit(Order::limit(OrderId{1}, Side::Buy, Price{101}, Quantity{40}), recorder)
            .accepted());

    // The two setup orders already consumed 1 and 2.
    ASSERT_EQ(recorder.events.size(), 3U);
    EXPECT_EQ(recorder.events[0].sequence, SequenceNumber{3});
    EXPECT_EQ(recorder.events[1].sequence, SequenceNumber{4});
    EXPECT_EQ(recorder.events[2].sequence, SequenceNumber{5});
}

// The whole point of numbering: a consumer can tell it missed something.
TEST(EventStream, SequenceNumbersNeverSkipAcrossMixedOperations) {
    MatchingEngine engine;
    EventRecorder recorder;

    ASSERT_TRUE(
        engine.submit(Order::limit(OrderId{1}, Side::Sell, Price{100}, Quantity{50}), recorder)
            .accepted());
    ASSERT_TRUE(
        engine.submit(Order::limit(OrderId{2}, Side::Buy, Price{100}, Quantity{20}), recorder)
            .accepted());
    ASSERT_TRUE(engine.modify(OrderId{1}, Price{99}, Quantity{40}, recorder).modified());
    EXPECT_FALSE(engine.cancel(OrderId{404}, recorder).succeeded());
    ASSERT_TRUE(engine.cancel(OrderId{1}, recorder).succeeded());

    ASSERT_FALSE(recorder.events.empty());
    for (std::size_t i = 0; i < recorder.events.size(); ++i) {
        EXPECT_EQ(recorder.events[i].sequence, SequenceNumber{i + 1}) << "at index " << i;
    }
    EXPECT_EQ(engine.last_sequence(), recorder.events.back().sequence);
}

// Rejections are numbered too. A consumer that skipped them would see gaps.
TEST(EventStream, RejectionsConsumeSequenceNumbers) {
    MatchingEngine engine;
    EventRecorder recorder;

    EXPECT_FALSE(engine.cancel(OrderId{1}, recorder).succeeded());
    EXPECT_FALSE(engine.cancel(OrderId{2}, recorder).succeeded());

    ASSERT_EQ(recorder.events.size(), 2U);
    EXPECT_EQ(recorder.events[0].sequence, SequenceNumber{1});
    EXPECT_EQ(recorder.events[1].sequence, SequenceNumber{2});
}

TEST(EventStream, LastSequenceIsZeroBeforeAnythingHappens) {
    const MatchingEngine engine;
    EXPECT_EQ(engine.last_sequence(), SequenceNumber{});
    EXPECT_FALSE(engine.last_sequence().is_valid());
}

// ---------------------------------------------------------------------------
// The stream agrees with the returned summaries
// ---------------------------------------------------------------------------

// SubmitResult is a summary of what the stream already said. If the two ever
// disagree, one of them is wrong.
TEST(EventStream, TradeEventsAccountForExactlyTheReportedFill) {
    MatchingEngine engine;
    ASSERT_TRUE(rest_sell(engine, 101, 100, 20).accepted());
    ASSERT_TRUE(rest_sell(engine, 102, 101, 25).accepted());

    EventRecorder recorder;
    const SubmitResult result =
        engine.submit(Order::limit(OrderId{1}, Side::Buy, Price{101}, Quantity{60},
                                   TimeInForce::ImmediateOrCancel),
                      recorder);

    Quantity from_trades{};
    for (const Event& event : recorder.of_type(EventType::Trade)) {
        from_trades += event.quantity;
    }
    Quantity from_cancels{};
    for (const Event& event : recorder.of_type(EventType::Cancelled)) {
        from_cancels += event.quantity;
    }

    EXPECT_EQ(from_trades, result.filled);
    EXPECT_EQ(from_cancels, result.cancelled);
    EXPECT_EQ(result.filled + result.resting + result.cancelled, Quantity{60});
}

}  // namespace
}  // namespace flashpoint
