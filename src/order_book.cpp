#include "flashpoint/order_book.hpp"

#include <cassert>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>

namespace flashpoint {
OrderBook::OrderBook(std::size_t expected_orders) {
    nodes_.reserve(expected_orders);
    index_.reserve(expected_orders);
}

// ---------------------------------------------------------------------------
// Node pool
//
// Free nodes form a singly-linked stack threaded through the `next` field of the
// nodes themselves, so the free list costs no memory beyond the pool. Reusing
// the most recently freed node first is deliberate: it is the one most likely to
// still be in cache. LIFO approach here.
// ---------------------------------------------------------------------------

OrderBook::NodeIndex OrderBook::acquire_node(const Order& order) {
    NodeIndex index = kNullNode;

    if (free_head_ != kNullNode) {
        index = free_head_;
        free_head_ = nodes_[free_head_].next;
    } else {
        // kNullNode is the sentinel, so it can never be a valid slot; the pool
        // is therefore capped one short of the index type's range.
        assert(nodes_.size() < std::numeric_limits<NodeIndex>::max() &&
               "order pool exhausted the 32-bit index space");
        index = static_cast<NodeIndex>(nodes_.size());
        nodes_.emplace_back();
    }

    Node& node = nodes_[index];
    node.id = order.id();
    node.remaining = order.quantity();
    node.prev = kNullNode;
    node.next = kNullNode;
    return index;
}

void OrderBook::release_node(NodeIndex index) noexcept {
    nodes_[index].next = free_head_;
    free_head_ = index;
}

// ---------------------------------------------------------------------------
// Intrusive queue: FIFO
// ---------------------------------------------------------------------------

void OrderBook::link_back(Level& level, NodeIndex index) noexcept {
    nodes_[index].prev = level.tail;
    nodes_[index].next = kNullNode;

    if (level.tail == kNullNode) {
        level.head = index;
    } else {
        nodes_[level.tail].next = index;
    }
    level.tail = index;
}

void OrderBook::unlink(Level& level, NodeIndex index) noexcept {
    const NodeIndex prev = nodes_[index].prev;
    const NodeIndex next = nodes_[index].next;

    // Four cases collapsed into two independent checks: whether the node was the
    // head, and whether it was the tail. A node that is both is the last order
    // at the level, which correctly leaves head and tail null.
    if (prev == kNullNode) {
        level.head = next;
    } else {
        nodes_[prev].next = next;
    }

    if (next == kNullNode) {
        level.tail = prev;
    } else {
        nodes_[next].prev = prev;
    }
}

// ---------------------------------------------------------------------------
// Level lookup
// ---------------------------------------------------------------------------

OrderBook::Level& OrderBook::level_at(Side side, Price price) {
    // operator[] creates the level if it is absent, which is what add() wants.
    return side == Side::Buy ? bids_[price] : asks_[price];
}

const OrderBook::Level* OrderBook::find_level(Side side, Price price) const {
    if (side == Side::Buy) {
        const auto it = bids_.find(price);
        return it == bids_.end() ? nullptr : &it->second;
    }
    const auto it = asks_.find(price);
    return it == asks_.end() ? nullptr : &it->second;
}

// Written once as a template because the two sides are now different types.
// Erasing by iterator rather than by key avoids a second lookup.
template <typename LevelMap>
void OrderBook::detach(LevelMap& levels, const Locator& locator, Quantity remaining) {
    const auto level_it = levels.find(locator.price);
    assert(level_it != levels.end() && "index referenced a level that does not exist");
    Level& level = level_it->second;

    unlink(level, locator.node);
    release_node(locator.node);

    level.total -= remaining;
    --level.count;

    // Empty levels are erased rather than retained. Keeping them would make
    // best_bid() report a price with no depth behind it, which is wrong.
    if (level.count == 0) {
        assert(level.head == kNullNode && level.tail == kNullNode &&
               "level order count reached zero with a non-empty queue");
        levels.erase(level_it);
    }
}

// ---------------------------------------------------------------------------
// Mutation
// ---------------------------------------------------------------------------

bool OrderBook::add(const Order& order) {
    assert(order.is_valid() && "OrderBook::add requires a structurally valid order");

    if (index_.contains(order.id())) {
        return false;
    }

    // Held by reference across acquire_node(): map elements are address-stable,
    // so growing the node pool cannot invalidate this. The node pool is
    // addressed by index precisely because it is not address-stable.
    Level& level = level_at(order.side(), order.price());

    const NodeIndex index = acquire_node(order);
    link_back(level, index);
    level.total += order.quantity();
    ++level.count;

    index_.emplace(order.id(), Locator{index, order.price(), order.side()});
    return true;
}

bool OrderBook::remove(OrderId id) {
    const auto entry = index_.find(id);
    if (entry == index_.end()) {
        return false;
    }

    const Locator locator = entry->second;

    // Read the quantity before releasing the node: release_node overwrites the
    // node's links, and reading a released slot would be a latent bug the moment
    // the field layout changes.
    const Quantity remaining = nodes_[locator.node].remaining;

    if (locator.side == Side::Buy) {
        detach(bids_, locator, remaining);
    } else {
        detach(asks_, locator, remaining);
    }

    index_.erase(entry);
    return true;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

std::optional<Price> OrderBook::best_bid() const noexcept {
    if (bids_.empty()) {
        return std::nullopt;
    }
    // Bids are stored descending, so the highest is the first element. This is
    // O(1). rbegin() was not: std::map caches its leftmost node but not its
    // rightmost, so rbegin() walks the tree (DD-042).
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const noexcept {
    if (asks_.empty()) {
        return std::nullopt;
    }
    // Lowest ask is the first element.
    return asks_.begin()->first;
}

Quantity OrderBook::quantity_at(Side side, Price price) const {
    const Level* level = find_level(side, price);
    return level == nullptr ? Quantity{} : level->total;
}

std::size_t OrderBook::order_count_at(Side side, Price price) const {
    const Level* level = find_level(side, price);
    return level == nullptr ? 0U : level->count;
}

std::optional<OrderId> OrderBook::front_at(Side side, Price price) const {
    const Level* level = find_level(side, price);
    if (level == nullptr || level->head == kNullNode) {
        return std::nullopt;
    }
    return nodes_[level->head].id;
}

TopOfBook OrderBook::top_of_book() const {
    TopOfBook top;

    if (!bids_.empty()) {
        const auto& [price, level] = *bids_.begin();
        top.bid_price = price;
        top.bid_quantity = level.total;
    }
    if (!asks_.empty()) {
        const auto& [price, level] = *asks_.begin();
        top.ask_price = price;
        top.ask_quantity = level.total;
    }

    return top;
}

std::size_t OrderBook::snapshot(Side side, std::span<LevelSnapshot> out) const {
    std::size_t written = 0;

    // Both sides order the best price first, so this walks forward either way.
    // Before DD-042 the bid side had to iterate in reverse.
    const auto collect = [&out, &written](const auto& levels) {
        for (const auto& [price, level] : levels) {
            if (written >= out.size()) {
                break;
            }
            out[written] = LevelSnapshot{price, level.total, level.count};
            ++written;
        }
    };

    if (side == Side::Buy) {
        collect(bids_);
    } else {
        collect(asks_);
    }
    return written;
}

Quantity OrderBook::quantity_available(Side aggressor_side, Price limit) const {
    Quantity total{};

    // Each side is ordered best-first, so both loops walk forward and stop at
    // the first level the aggressor cannot reach.
    if (aggressor_side == Side::Buy) {
        for (const auto& [price, level] : asks_) {
            if (price > limit) {
                break;
            }
            total += level.total;
        }
    } else {
        for (const auto& [price, level] : bids_) {
            if (price < limit) {
                break;
            }
            total += level.total;
        }
    }

    return total;
}

std::optional<Quantity> OrderBook::remaining_of(OrderId id) const {
    const auto entry = index_.find(id);
    if (entry == index_.end()) {
        return std::nullopt;
    }
    return nodes_[entry->second.node].remaining;
}

std::optional<RestingOrder> OrderBook::resting_order(OrderId id) const {
    const auto entry = index_.find(id);
    if (entry == index_.end()) {
        return std::nullopt;
    }

    // The locator already carries side and price, so this costs one hash lookup
    // and one node read. No level lookup is needed.
    const Locator& locator = entry->second;
    return RestingOrder{id, locator.side, locator.price, nodes_[locator.node].remaining};
}

bool OrderBook::reduce(OrderId id, Quantity by) {
    const auto entry = index_.find(id);
    if (entry == index_.end()) {
        return false;
    }

    const Locator locator = entry->second;
    Node& node = nodes_[locator.node];

    assert(by > Quantity{} && by < node.remaining &&
           "reduce() requires 0 < by < remaining; use remove() to retire an order entirely");

    node.remaining -= by;

    // The cached aggregate has to follow the order's quantity down.
    if (locator.side == Side::Buy) {
        const auto level_it = bids_.find(locator.price);
        assert(level_it != bids_.end() && "index referenced a level that does not exist");
        level_it->second.total -= by;
    } else {
        const auto level_it = asks_.find(locator.price);
        assert(level_it != asks_.end() && "index referenced a level that does not exist");
        level_it->second.total -= by;
    }

    return true;
}

bool OrderBook::contains(OrderId id) const {
    return index_.contains(id);
}

std::size_t OrderBook::size() const noexcept {
    return index_.size();
}

bool OrderBook::empty() const noexcept {
    return index_.empty();
}

}  // namespace flashpoint
