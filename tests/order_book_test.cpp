// Tests for the limit order book.
//
// Three things carry most of the weight here:
//
//   * Price priority: best bid is the highest, best ask the lowest.
//   * Time priority: FIFO within a level, which is only meaningfully testable
//     because front_at() makes queue position observable.
//   * The intrusive queue's unlink paths: Removing the head, the tail, a middle
//     order, and the sole order are four distinct code paths through the same
//     six lines of pointer surgery, and each is tested separately.
//
// The final test is a differential one: several thousand mixed operations run
// against both the book and a deliberately naive reference model, asserting the
// two agree. That is the test most likely to catch an intrusive-list bug that
// the hand-written cases miss, and it is the reason the reference model is
// written for obviousness rather than speed.

#include "flashpoint/order_book.hpp"

#include "flashpoint/order.hpp"
#include "flashpoint/ostream.hpp"
#include "flashpoint/types.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <map>
#include <numeric>
#include <optional>
#include <random>
#include <type_traits>
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

// ---------------------------------------------------------------------------
// Interface guard
//
// The price-level container is a std::map currently and is expected to become
// something faster once Milestone 10 produces a profile. That swap is only
// cheap if no caller can depend on the container, so these assertions pin the
// public surface to values and handles.
//
// This is the condition that makes deferring the container choice a decision
// rather than a trap. If a later milestone adds an accessor returning a
// reference or an iterator into the book, this fails immediately, at the point
// the mistake is cheap to undo.
// ---------------------------------------------------------------------------

template <typename T, typename = void>
struct HasIteratorType : std::false_type {};

template <typename T>
struct HasIteratorType<T, std::void_t<typename T::iterator>> : std::true_type {};

template <typename T, typename = void>
struct HasBegin : std::false_type {};

template <typename T>
struct HasBegin<T, std::void_t<decltype(std::declval<const T&>().begin())>> : std::true_type {};

static_assert(!HasIteratorType<OrderBook>::value,
              "OrderBook must not expose an iterator type; callers would then depend on the "
              "internal container and the Milestone 11 swap would stop being a private change.");
static_assert(!HasBegin<OrderBook>::value, "OrderBook must not be iterable.");

// Every accessor returns a value or a handle. Never a reference, a pointer, or
// anything derived from the internal containers.
using ConstBook = const OrderBook&;
static_assert(std::is_same_v<decltype(std::declval<ConstBook>().best_bid()), std::optional<Price>>);
static_assert(std::is_same_v<decltype(std::declval<ConstBook>().best_ask()), std::optional<Price>>);
static_assert(
    std::is_same_v<decltype(std::declval<ConstBook>().quantity_at(Side::Buy, Price{})), Quantity>);
static_assert(std::is_same_v<decltype(std::declval<ConstBook>().order_count_at(Side::Buy, Price{})),
                             std::size_t>);
static_assert(std::is_same_v<decltype(std::declval<ConstBook>().front_at(Side::Buy, Price{})),
                             std::optional<OrderId>>);
static_assert(std::is_same_v<decltype(std::declval<ConstBook>().contains(OrderId{})), bool>);
static_assert(std::is_same_v<decltype(std::declval<ConstBook>().remaining_of(OrderId{})),
                             std::optional<Quantity>>);
// resting_order returns a snapshot by value, not a view into the book.
static_assert(std::is_same_v<decltype(std::declval<ConstBook>().resting_order(OrderId{})),
                             std::optional<RestingOrder>>);
static_assert(std::is_same_v<decltype(std::declval<ConstBook>().size()), std::size_t>);

// Silently dropping "this id already rests" or "no such order" would be a bug
// the engine must not be able to write by accident.
static_assert(
    std::is_same_v<decltype(std::declval<OrderBook&>().add(std::declval<const Order&>())), bool>);

// ---------------------------------------------------------------------------
// Empty book
// ---------------------------------------------------------------------------

TEST(OrderBook, StartsEmptyWithNoTopOfBook) {
    const OrderBook book;

    EXPECT_TRUE(book.empty());
    EXPECT_EQ(book.size(), 0U);
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_FALSE(book.contains(OrderId{1}));
}

// An absent level and an empty level must be indistinguishable, because empty
// levels are never retained.
TEST(OrderBook, QueriesOnAbsentLevelsReturnEmptyRatherThanFailing) {
    const OrderBook book;

    EXPECT_EQ(book.quantity_at(Side::Buy, Price{100}), Quantity{});
    EXPECT_EQ(book.order_count_at(Side::Sell, Price{100}), 0U);
    EXPECT_FALSE(book.front_at(Side::Buy, Price{100}).has_value());
}

TEST(OrderBook, RemovingFromAnEmptyBookReportsFailure) {
    OrderBook book;
    EXPECT_FALSE(book.remove(OrderId{1}));
}

// ---------------------------------------------------------------------------
// Adding
// ---------------------------------------------------------------------------

TEST(OrderBook, RestingAnOrderMakesItVisible) {
    OrderBook book;

    EXPECT_TRUE(book.add(buy(1, 100, 50)));

    EXPECT_FALSE(book.empty());
    EXPECT_EQ(book.size(), 1U);
    EXPECT_TRUE(book.contains(OrderId{1}));
    EXPECT_EQ(book.best_bid(), Price{100});
    EXPECT_EQ(book.quantity_at(Side::Buy, Price{100}), Quantity{50});
    EXPECT_EQ(book.order_count_at(Side::Buy, Price{100}), 1U);
}

// The two sides are independent: a bid must never show up as an ask, even at the
// same price.
TEST(OrderBook, SidesAreIndependentAtTheSamePrice) {
    OrderBook book;

    ASSERT_TRUE(book.add(buy(1, 100, 50)));
    ASSERT_TRUE(book.add(sell(2, 100, 70)));

    EXPECT_EQ(book.quantity_at(Side::Buy, Price{100}), Quantity{50});
    EXPECT_EQ(book.quantity_at(Side::Sell, Price{100}), Quantity{70});
    EXPECT_EQ(book.best_bid(), Price{100});
    EXPECT_EQ(book.best_ask(), Price{100});
}

// Duplicate ids should be recognised. It must reject
// the second order without disturbing the first.
TEST(OrderBook, DuplicateOrderIdIsRejectedAndLeavesTheBookUnchanged) {
    OrderBook book;
    ASSERT_TRUE(book.add(buy(1, 100, 50)));

    EXPECT_FALSE(book.add(buy(1, 101, 999)));

    EXPECT_EQ(book.size(), 1U);
    EXPECT_EQ(book.best_bid(), Price{100});
    EXPECT_EQ(book.quantity_at(Side::Buy, Price{101}), Quantity{});
}

TEST(OrderBook, NegativePricesRestAndOrderCorrectly) {
    OrderBook book;

    ASSERT_TRUE(book.add(buy(1, -500, 10)));
    ASSERT_TRUE(book.add(buy(2, -400, 10)));

    EXPECT_EQ(book.best_bid(), Price{-400});
}

// ---------------------------------------------------------------------------
// Price priority
// ---------------------------------------------------------------------------

// The defining property of the bid side: best is the highest, regardless of
// insertion order.
TEST(OrderBook, BestBidIsTheHighestPriceWhateverTheInsertionOrder) {
    OrderBook book;

    ASSERT_TRUE(book.add(buy(1, 100, 10)));
    ASSERT_TRUE(book.add(buy(2, 102, 10)));
    ASSERT_TRUE(book.add(buy(3, 101, 10)));

    EXPECT_EQ(book.best_bid(), Price{102});
}

TEST(OrderBook, BestAskIsTheLowestPriceWhateverTheInsertionOrder) {
    OrderBook book;

    ASSERT_TRUE(book.add(sell(1, 105, 10)));
    ASSERT_TRUE(book.add(sell(2, 103, 10)));
    ASSERT_TRUE(book.add(sell(3, 104, 10)));

    EXPECT_EQ(book.best_ask(), Price{103});
}

// ---------------------------------------------------------------------------
// Aggregation
// ---------------------------------------------------------------------------

TEST(OrderBook, LevelAggregatesQuantityAndCountAcrossOrders) {
    OrderBook book;

    ASSERT_TRUE(book.add(buy(1, 100, 10)));
    ASSERT_TRUE(book.add(buy(2, 100, 25)));
    ASSERT_TRUE(book.add(buy(3, 100, 5)));

    EXPECT_EQ(book.quantity_at(Side::Buy, Price{100}), Quantity{40});
    EXPECT_EQ(book.order_count_at(Side::Buy, Price{100}), 3U);
    EXPECT_EQ(book.size(), 3U);
}

TEST(OrderBook, RemovingAnOrderDecrementsItsLevelAggregates) {
    OrderBook book;
    ASSERT_TRUE(book.add(buy(1, 100, 10)));
    ASSERT_TRUE(book.add(buy(2, 100, 25)));

    ASSERT_TRUE(book.remove(OrderId{1}));

    EXPECT_EQ(book.quantity_at(Side::Buy, Price{100}), Quantity{25});
    EXPECT_EQ(book.order_count_at(Side::Buy, Price{100}), 1U);
    EXPECT_EQ(book.size(), 1U);
}

// ---------------------------------------------------------------------------
// Time priority
// ---------------------------------------------------------------------------

TEST(OrderBook, TheFirstOrderAtAPriceIsTheFirstToFill) {
    OrderBook book;

    ASSERT_TRUE(book.add(buy(1, 100, 10)));
    ASSERT_TRUE(book.add(buy(2, 100, 10)));
    ASSERT_TRUE(book.add(buy(3, 100, 10)));

    EXPECT_EQ(book.front_at(Side::Buy, Price{100}), OrderId{1});
}

// A larger order arriving later must not jump the queue. Size confers no
// priority, only time does.
TEST(OrderBook, LaterAndLargerOrdersDoNotJumpTheQueue) {
    OrderBook book;

    ASSERT_TRUE(book.add(buy(1, 100, 1)));
    ASSERT_TRUE(book.add(buy(2, 100, 1000)));

    EXPECT_EQ(book.front_at(Side::Buy, Price{100}), OrderId{1});
}

TEST(OrderBook, RemovingTheFrontOrderPromotesTheNextInLine) {
    OrderBook book;
    ASSERT_TRUE(book.add(buy(1, 100, 10)));
    ASSERT_TRUE(book.add(buy(2, 100, 10)));
    ASSERT_TRUE(book.add(buy(3, 100, 10)));

    ASSERT_TRUE(book.remove(OrderId{1}));
    EXPECT_EQ(book.front_at(Side::Buy, Price{100}), OrderId{2});

    ASSERT_TRUE(book.remove(OrderId{2}));
    EXPECT_EQ(book.front_at(Side::Buy, Price{100}), OrderId{3});
}

// ---------------------------------------------------------------------------
// Intrusive queue unlink paths
//
// Four structurally distinct cases through the same pointer surgery. Each is
// exercised on its own so a failure names which one broke.
// ---------------------------------------------------------------------------

TEST(OrderBookUnlink, RemovingTheHeadKeepsTheRestInOrder) {
    OrderBook book;
    ASSERT_TRUE(book.add(buy(1, 100, 10)));
    ASSERT_TRUE(book.add(buy(2, 100, 20)));
    ASSERT_TRUE(book.add(buy(3, 100, 30)));

    ASSERT_TRUE(book.remove(OrderId{1}));

    EXPECT_EQ(book.front_at(Side::Buy, Price{100}), OrderId{2});
    EXPECT_EQ(book.order_count_at(Side::Buy, Price{100}), 2U);
    EXPECT_EQ(book.quantity_at(Side::Buy, Price{100}), Quantity{50});
}

TEST(OrderBookUnlink, RemovingTheTailLeavesTheFrontUntouched) {
    OrderBook book;
    ASSERT_TRUE(book.add(buy(1, 100, 10)));
    ASSERT_TRUE(book.add(buy(2, 100, 20)));
    ASSERT_TRUE(book.add(buy(3, 100, 30)));

    ASSERT_TRUE(book.remove(OrderId{3}));

    EXPECT_EQ(book.front_at(Side::Buy, Price{100}), OrderId{1});
    EXPECT_EQ(book.order_count_at(Side::Buy, Price{100}), 2U);
    EXPECT_EQ(book.quantity_at(Side::Buy, Price{100}), Quantity{30});
}

// The case that a singly-linked list could not do in O(1), and the reason the
// queue is doubly linked.
TEST(OrderBookUnlink, RemovingAMiddleOrderSplicesTheNeighboursTogether) {
    OrderBook book;
    ASSERT_TRUE(book.add(buy(1, 100, 10)));
    ASSERT_TRUE(book.add(buy(2, 100, 20)));
    ASSERT_TRUE(book.add(buy(3, 100, 30)));

    ASSERT_TRUE(book.remove(OrderId{2}));

    EXPECT_EQ(book.front_at(Side::Buy, Price{100}), OrderId{1});
    EXPECT_EQ(book.order_count_at(Side::Buy, Price{100}), 2U);
    EXPECT_EQ(book.quantity_at(Side::Buy, Price{100}), Quantity{40});

    // The splice must hold when the new tail is then removed.
    ASSERT_TRUE(book.remove(OrderId{1}));
    EXPECT_EQ(book.front_at(Side::Buy, Price{100}), OrderId{3});
}

TEST(OrderBookUnlink, RemovingTheOnlyOrderEmptiesTheLevel) {
    OrderBook book;
    ASSERT_TRUE(book.add(buy(1, 100, 10)));

    ASSERT_TRUE(book.remove(OrderId{1}));

    EXPECT_TRUE(book.empty());
    EXPECT_FALSE(book.front_at(Side::Buy, Price{100}).has_value());
}

TEST(OrderBook, RemovingAnAbsentOrRemovedOrderReportsFailure) {
    OrderBook book;
    ASSERT_TRUE(book.add(buy(1, 100, 10)));

    EXPECT_FALSE(book.remove(OrderId{99}));
    ASSERT_TRUE(book.remove(OrderId{1}));
    EXPECT_FALSE(book.remove(OrderId{1}));
    EXPECT_FALSE(book.contains(OrderId{1}));
}

// ---------------------------------------------------------------------------
// Level lifecycle
// ---------------------------------------------------------------------------

// Retaining a drained level would make best_bid() report a price with no depth
// behind it.
TEST(OrderBook, DrainingTheBestLevelPromotesTheNextOne) {
    OrderBook book;
    ASSERT_TRUE(book.add(buy(1, 102, 10)));
    ASSERT_TRUE(book.add(buy(2, 101, 10)));
    ASSERT_TRUE(book.add(buy(3, 100, 10)));

    ASSERT_TRUE(book.remove(OrderId{1}));
    EXPECT_EQ(book.best_bid(), Price{101});

    ASSERT_TRUE(book.remove(OrderId{2}));
    EXPECT_EQ(book.best_bid(), Price{100});

    ASSERT_TRUE(book.remove(OrderId{3}));
    EXPECT_FALSE(book.best_bid().has_value());
}

TEST(OrderBook, DrainingOneSideLeavesTheOtherIntact) {
    OrderBook book;
    ASSERT_TRUE(book.add(buy(1, 100, 10)));
    ASSERT_TRUE(book.add(sell(2, 105, 10)));

    ASSERT_TRUE(book.remove(OrderId{1}));

    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_EQ(book.best_ask(), Price{105});
    EXPECT_EQ(book.size(), 1U);
}

// Emptying the book completely and refilling it exercises node-pool reuse: the
// second wave of orders runs entirely on recycled slots off the free list.
TEST(OrderBook, RefillingAfterCompleteDrainReusesThePoolCorrectly) {
    OrderBook book;

    for (OrderId::Rep id = 1; id <= 10; ++id) {
        ASSERT_TRUE(book.add(buy(id, 100, 10)));
    }
    for (OrderId::Rep id = 1; id <= 10; ++id) {
        ASSERT_TRUE(book.remove(OrderId{id}));
    }
    ASSERT_TRUE(book.empty());

    for (OrderId::Rep id = 11; id <= 20; ++id) {
        ASSERT_TRUE(book.add(buy(id, 100, 10)));
    }

    EXPECT_EQ(book.size(), 10U);
    EXPECT_EQ(book.quantity_at(Side::Buy, Price{100}), Quantity{100});
    // Recycled nodes must not scramble arrival order.
    EXPECT_EQ(book.front_at(Side::Buy, Price{100}), OrderId{11});
}

// ---------------------------------------------------------------------------
// quantity_available
//
// Fill-or-kill needs to know how much it could trade before it trades anything.
// The side passed in is the aggressor's, so a Buy is measured against the asks.
// ---------------------------------------------------------------------------

TEST(QuantityAvailable, IsZeroOnAnEmptyBook) {
    const OrderBook book;

    EXPECT_EQ(book.quantity_available(Side::Buy, Price{100}), Quantity{});
    EXPECT_EQ(book.quantity_available(Side::Sell, Price{100}), Quantity{});
}

TEST(QuantityAvailable, SumsAsksAtOrBelowABuyersLimit) {
    OrderBook book;
    ASSERT_TRUE(book.add(sell(1, 100, 10)));
    ASSERT_TRUE(book.add(sell(2, 101, 20)));
    ASSERT_TRUE(book.add(sell(3, 105, 40)));

    EXPECT_EQ(book.quantity_available(Side::Buy, Price{99}), Quantity{});
    EXPECT_EQ(book.quantity_available(Side::Buy, Price{100}), Quantity{10});
    EXPECT_EQ(book.quantity_available(Side::Buy, Price{101}), Quantity{30});
    EXPECT_EQ(book.quantity_available(Side::Buy, Price{104}), Quantity{30});
    EXPECT_EQ(book.quantity_available(Side::Buy, Price{105}), Quantity{70});
    EXPECT_EQ(book.quantity_available(Side::Buy, Price{999}), Quantity{70});
}

TEST(QuantityAvailable, SumsBidsAtOrAboveASellersLimit) {
    OrderBook book;
    ASSERT_TRUE(book.add(buy(1, 100, 10)));
    ASSERT_TRUE(book.add(buy(2, 99, 20)));
    ASSERT_TRUE(book.add(buy(3, 95, 40)));

    EXPECT_EQ(book.quantity_available(Side::Sell, Price{101}), Quantity{});
    EXPECT_EQ(book.quantity_available(Side::Sell, Price{100}), Quantity{10});
    EXPECT_EQ(book.quantity_available(Side::Sell, Price{99}), Quantity{30});
    EXPECT_EQ(book.quantity_available(Side::Sell, Price{96}), Quantity{30});
    EXPECT_EQ(book.quantity_available(Side::Sell, Price{95}), Quantity{70});
    EXPECT_EQ(book.quantity_available(Side::Sell, Price{-999}), Quantity{70});
}

// Only the opposite side counts. A buyer's own resting bids are not liquidity.
TEST(QuantityAvailable, IgnoresTheAggressorsOwnSide) {
    OrderBook book;
    ASSERT_TRUE(book.add(buy(1, 100, 50)));

    EXPECT_EQ(book.quantity_available(Side::Buy, Price{200}), Quantity{});
    EXPECT_EQ(book.quantity_available(Side::Sell, Price{100}), Quantity{50});
}

TEST(QuantityAvailable, TracksRemovalsAndPartialFills) {
    OrderBook book;
    ASSERT_TRUE(book.add(sell(1, 100, 30)));
    ASSERT_TRUE(book.add(sell(2, 100, 20)));
    ASSERT_EQ(book.quantity_available(Side::Buy, Price{100}), Quantity{50});

    ASSERT_TRUE(book.reduce(OrderId{1}, Quantity{10}));
    EXPECT_EQ(book.quantity_available(Side::Buy, Price{100}), Quantity{40});

    ASSERT_TRUE(book.remove(OrderId{2}));
    EXPECT_EQ(book.quantity_available(Side::Buy, Price{100}), Quantity{20});
}

// ---------------------------------------------------------------------------
// Differential test against a reference model
// ---------------------------------------------------------------------------

/// A deliberately naive order book. Linear scans everywhere, a vector per level,
/// no pooling. Written to be obviously correct at a glance, because its whole
/// job is to be the thing we trust when the real book disagrees with it.
class ReferenceBook {
public:
    struct Entry {
        OrderId id;
        Quantity quantity;
    };

    void add(const Order& order) {
        levels_for(order.side())[order.price()].push_back(Entry{order.id(), order.quantity()});
    }

    void remove(OrderId id) {
        for (const Side side : {Side::Buy, Side::Sell}) {
            auto& levels = levels_for(side);
            for (auto level = levels.begin(); level != levels.end(); ++level) {
                auto& queue = level->second;
                for (auto entry = queue.begin(); entry != queue.end(); ++entry) {
                    if (entry->id == id) {
                        queue.erase(entry);
                        if (queue.empty()) {
                            levels.erase(level);
                        }
                        return;
                    }
                }
            }
        }
    }

    [[nodiscard]] std::size_t size() const {
        std::size_t total = 0;
        for (const auto& levels : {bids_, asks_}) {
            for (const auto& [price, queue] : levels) {
                total += queue.size();
            }
        }
        return total;
    }

    [[nodiscard]] std::optional<Price> best_bid() const {
        return bids_.empty() ? std::nullopt : std::optional<Price>{bids_.rbegin()->first};
    }

    [[nodiscard]] std::optional<Price> best_ask() const {
        return asks_.empty() ? std::nullopt : std::optional<Price>{asks_.begin()->first};
    }

    [[nodiscard]] Quantity quantity_at(Side side, Price price) const {
        const auto* queue = find(side, price);
        if (queue == nullptr) {
            return Quantity{};
        }
        return std::accumulate(queue->begin(), queue->end(), Quantity{},
                               [](Quantity sum, const Entry& e) { return sum + e.quantity; });
    }

    [[nodiscard]] std::size_t order_count_at(Side side, Price price) const {
        const auto* queue = find(side, price);
        return queue == nullptr ? 0U : queue->size();
    }

    [[nodiscard]] std::optional<OrderId> front_at(Side side, Price price) const {
        const auto* queue = find(side, price);
        if (queue == nullptr || queue->empty()) {
            return std::nullopt;
        }
        return queue->front().id;
    }

private:
    using Queue = std::vector<Entry>;
    using Levels = std::map<Price, Queue>;

    Levels& levels_for(Side side) {
        return side == Side::Buy ? bids_ : asks_;
    }

    const Levels& levels_for(Side side) const {
        return side == Side::Buy ? bids_ : asks_;
    }

    const Queue* find(Side side, Price price) const {
        const Levels& levels = levels_for(side);
        const auto it = levels.find(price);
        return it == levels.end() ? nullptr : &it->second;
    }

    Levels bids_;
    Levels asks_;
};

// Several thousand interleaved adds and removes, checked against the reference
// model. The price range is narrow on purpose: it forces heavy contention on a
// few levels, which is what produces long queues, repeated middle-removals, and
// levels that drain and are recreated, which are exactly the situations where an
// intrusive list goes wrong.
//
// The seed is fixed so a failure is reproducible. A random seed would turn a
// real bug into a story about a test that "sometimes fails".
TEST(OrderBookDifferential, AgreesWithAReferenceModelAcrossMixedOperations) {
    constexpr int kSteps = 4000;
    constexpr Price::Rep kMinPrice = 95;
    constexpr Price::Rep kMaxPrice = 105;

    std::mt19937 rng{20240607U};
    std::uniform_int_distribution<Price::Rep> price_dist{kMinPrice, kMaxPrice};
    std::uniform_int_distribution<Quantity::Rep> quantity_dist{1, 100};
    std::uniform_int_distribution<int> roll{0, 99};

    OrderBook book;
    ReferenceBook reference;
    std::vector<OrderId> live;
    OrderId::Rep next_id = 1;

    for (int step = 0; step < kSteps; ++step) {
        // Skewed towards adds so the book grows overall while still churning.
        const bool removing = !live.empty() && roll(rng) < 40;

        if (removing) {
            std::uniform_int_distribution<std::size_t> pick{0, live.size() - 1};
            const std::size_t slot = pick(rng);
            const OrderId id = live[slot];
            live[slot] = live.back();
            live.pop_back();

            ASSERT_TRUE(book.remove(id)) << "step " << step << " removing " << id;
            reference.remove(id);
        } else {
            const Side side = (roll(rng) % 2 == 0) ? Side::Buy : Side::Sell;
            const Order order{OrderId{next_id++}, side, Price{price_dist(rng)},
                              Quantity{quantity_dist(rng)}};

            ASSERT_TRUE(book.add(order)) << "step " << step << " adding " << order;
            reference.add(order);
            live.push_back(order.id());
        }

        ASSERT_EQ(book.size(), reference.size()) << "step " << step;
        ASSERT_EQ(book.best_bid(), reference.best_bid()) << "step " << step;
        ASSERT_EQ(book.best_ask(), reference.best_ask()) << "step " << step;
    }

    // A full sweep of every level on both sides once the churn has finished.
    for (Price::Rep p = kMinPrice; p <= kMaxPrice; ++p) {
        for (const Side side : {Side::Buy, Side::Sell}) {
            const Price price{p};
            EXPECT_EQ(book.quantity_at(side, price), reference.quantity_at(side, price))
                << to_string(side) << ' ' << price;
            EXPECT_EQ(book.order_count_at(side, price), reference.order_count_at(side, price))
                << to_string(side) << ' ' << price;
            EXPECT_EQ(book.front_at(side, price), reference.front_at(side, price))
                << to_string(side) << ' ' << price;
        }
    }

    // The churn must actually have built a book worth checking, or the whole
    // test could pass by exercising nothing.
    EXPECT_GT(book.size(), 100U);
}

}  // namespace
}  // namespace flashpoint
