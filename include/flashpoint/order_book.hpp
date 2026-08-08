#pragma once

#include "flashpoint/order.hpp"
#include "flashpoint/types.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace flashpoint {

/// A single-instrument limit order book maintaining price-time priority.
///
/// The book knows nothing about symbols. A book is a per-instrument structure;
/// trading several instruments means holding several books, which is the
/// engine's concern (DD-014).
///
/// It is a container, not a matching engine. Nothing here prevents a bid from
/// resting above the best ask, guaranteeing a non-crossed book is the matching
/// engine's job. Adding that check here would duplicate the engine's
/// logic in a place that cannot produce a trade to resolve the cross.
///
/// # Complexity
///
/// | Operation | Cost |
/// |-----------|------|
/// | `add`     | O(log L) to find the level, O(1) to append |
/// | `remove`  | O(1) to find the order and unlink it, O(log L) to find its level |
/// | `best_bid` / `best_ask` | O(1) |
/// | `quantity_at` / `order_count_at` / `front_at` | O(log L) |
///
/// L is the number of distinct price levels, not the number of orders. Every
/// O(log L) term comes from the price-level container and nothing else; see
/// DD-015 for why `std::map` is used for now.
///
/// Interface contract:
///
/// Every accessor returns a value or an `OrderId`. Nothing returns an iterator,
/// a reference into the book, or any type derived from the internal containers.
/// This allows the price-level container to potentially be replaced at Milestone 11 if needed
/// without touching a single caller, and `tests/order_book_test.cpp` pins it with static
/// assertions so it cannot erode quietly.
class OrderBook {
public:
    OrderBook() = default;

    /// Pre-sizes the node pool and order index for `expected_orders` resting orders.
    explicit OrderBook(std::size_t expected_orders);

    /// Rests `order` at the back of the queue for its price and side.
    ///
    /// Returns false if an order with the same id is already resting, which is
    /// the only failure this container recognises.
    [[nodiscard]] bool add(const Order& order);

    /// Removes the order with `id` wherever it rests. Returns false if no such
    /// order is present.
    [[nodiscard]] bool remove(OrderId id);

    /// Highest resting bid price, or nullopt if there are no bids.
    [[nodiscard]] std::optional<Price> best_bid() const noexcept;

    /// Lowest resting ask price, or nullopt if there are no asks.
    [[nodiscard]] std::optional<Price> best_ask() const noexcept;

    /// Total resting quantity at one price on one side. Zero if the level is
    /// absent, which is indistinguishable from an empty level.
    [[nodiscard]] Quantity quantity_at(Side side, Price price) const;

    /// Number of distinct orders resting at one price on one side.
    [[nodiscard]] std::size_t order_count_at(Side side, Price price) const;

    /// The order at the front of the queue for a price and side, and the one that
    /// time priority says fills next. Nullopt if the level is empty.
    [[nodiscard]] std::optional<OrderId> front_at(Side side, Price price) const;

    [[nodiscard]] bool contains(OrderId id) const;

    /// Total number of resting orders across both sides.
    [[nodiscard]] std::size_t size() const noexcept;

    [[nodiscard]] bool empty() const noexcept;

private:
    /// Orders are addressed by index into a pooled vector rather than by
    /// pointer. Indices are half the size of a pointer, and unlike pointers
    /// they survive the pool reallocating as it grows, which is what lets the
    /// pool start empty instead of demanding a fixed capacity up front.
    using NodeIndex = std::uint32_t;

    static constexpr NodeIndex kNullNode = std::numeric_limits<NodeIndex>::max();

    /// A resting order, plus its links within the queue for its price level.
    ///
    /// Does not store side or price: those are implied by the level
    /// that contains it, and duplicating them across every order would inflate
    /// the hottest object in the book (DD-017). The cold-path bookkeeping that
    /// does need them lives in `Locator`.
    ///
    /// 8 + 8 + 4 + 4 = 24 bytes
    struct Node {
        OrderId id{};
        Quantity remaining{};
        NodeIndex prev = kNullNode;
        NodeIndex next = kNullNode;
    };

    /// One price level: an intrusive FIFO queue of node indices, plus cached
    /// aggregates so depth queries do not walk the queue.
    struct Level {
        NodeIndex head = kNullNode;  ///< Oldest order; fills first.
        NodeIndex tail = kNullNode;  ///< Newest order.
        Quantity total{};            ///< Sum of `remaining` over the queue.
        std::uint32_t count = 0;     ///< Number of orders in the queue.
    };

    /// Where an order lives. Kept in the index rather than in `Node` so that the
    /// hot path never pays for data only the cancel path reads.
    struct Locator {
        NodeIndex node = kNullNode;
        Price price{};
        Side side{};
    };

    /// Both sides use ascending order, so the best ask is `begin()` and the best
    /// bid is `rbegin()`. Both are O(1) on a `std::map`. Using one comparator for
    /// both sides rather than a reversed one for bids keeps the type identical
    /// on both sides, so every helper below is written once instead of twice.
    using Levels = std::map<Price, Level>;

    [[nodiscard]] Levels& levels_for(Side side) noexcept;
    [[nodiscard]] const Levels& levels_for(Side side) const noexcept;

    /// Returns the level for `side`/`price`, or nullptr when absent.
    [[nodiscard]] const Level* find_level(Side side, Price price) const;

    [[nodiscard]] NodeIndex acquire_node(const Order& order);
    void release_node(NodeIndex index) noexcept;

    void link_back(Level& level, NodeIndex index) noexcept;
    void unlink(Level& level, NodeIndex index) noexcept;

    std::vector<Node> nodes_;
    NodeIndex free_head_ = kNullNode;

    Levels bids_;
    Levels asks_;

    std::unordered_map<OrderId, Locator> index_;
};

}  // namespace flashpoint
