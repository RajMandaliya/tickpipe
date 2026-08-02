#include "software_engine.hpp"
#include <algorithm>

MatchResult SoftwareEngine::submit(const Order& order) noexcept {
    ++orders_processed_;

    if (order.side == Side::Buy)
        return try_match_buy(order);
    else
        return try_match_sell(order);
}

MatchResult SoftwareEngine::try_match_buy(const Order& order) noexcept {
    MatchResult result{};

    // check if best ask <= buy price
    if (!asks_.empty()) {
        auto it = asks_.begin();  // lowest ask first
        if (it->first <= order.price) {
            auto& queue = it->second;
            auto& [ref, qty] = queue.front();

            result.matched        = true;
            result.buy_order_ref  = order.order_ref;
            result.sell_order_ref = ref;
            result.price          = it->first;
            result.quantity       = std::min(order.quantity, qty);

            qty -= result.quantity;
            if (qty == 0) queue.pop_front();
            if (queue.empty()) asks_.erase(it);

            ++matches_made_;
            shares_traded_ += result.quantity;
            last_price_     = result.price;
            return result;
        }
    }

    // no match — rest in book
    bids_[order.price].push_back({order.order_ref, order.quantity});
    return result;
}

MatchResult SoftwareEngine::try_match_sell(const Order& order) noexcept {
    MatchResult result{};

    // check if best bid >= sell price
    if (!bids_.empty()) {
        auto it = bids_.begin();  // highest bid first
        if (it->first >= order.price) {
            auto& queue = it->second;
            auto& [ref, qty] = queue.front();

            result.matched        = true;
            result.buy_order_ref  = ref;
            result.sell_order_ref = order.order_ref;
            result.price          = it->first;
            result.quantity       = std::min(order.quantity, qty);

            qty -= result.quantity;
            if (qty == 0) queue.pop_front();
            if (queue.empty()) bids_.erase(it);

            ++matches_made_;
            shares_traded_ += result.quantity;
            last_price_     = result.price;
            return result;
        }
    }

    // no match — rest in book
    asks_[order.price].push_back({order.order_ref, order.quantity});
    return result;
}

void SoftwareEngine::apply(const ResolvedOp& op) noexcept {
    switch (op.action) {

    case Action::Add:
        rest_order(op.side, op.price, op.order_ref, op.quantity);
        ++orders_processed_;
        break;

    case Action::Execute:
        if (!reduce_ref(op.side, op.price, op.order_ref, op.quantity)) {
            // C prints at a price the order isn't resting at, so the level
            // lookup misses. scan that side before calling it an orphan.
            bool found = false;
            auto scan = [&](auto& book) {
                for (auto lit = book.begin(); lit != book.end(); ++lit) {
                    auto& q = lit->second;
                    for (auto qit = q.begin(); qit != q.end(); ++qit) {
                        if (qit->first != op.order_ref) continue;
                        uint32_t fill = std::min(op.quantity, qit->second);
                        qit->second -= fill;
                        if (qit->second == 0) q.erase(qit);
                        if (q.empty()) book.erase(lit);
                        found = true;
                        return;
                    }
                }
            };
            if (op.side == Side::Buy) scan(bids_); else scan(asks_);
            if (!found) ++orphan_ops_;
        }

        // zero match number = non-printable, no volume and no price print
        if (op.match_number != 0) {
            ++matches_made_;
            shares_traded_ += op.quantity;
            last_price_     = op.price;
        }
        break;

    case Action::Cancel:
        if (!reduce_ref(op.side, op.price, op.order_ref, op.quantity))
            ++orphan_ops_;
        break;

    case Action::Delete:
        if (!remove_ref(op.side, op.price, op.order_ref))
            ++orphan_ops_;
        break;

    case Action::Trade:
        ++matches_made_;
        shares_traded_ += op.quantity;
        last_price_     = op.price;
        break;

    default:
        break;
    }
}

void SoftwareEngine::rest_order(Side side, uint32_t price,
                                uint64_t ref, uint32_t qty) noexcept {
    if (qty == 0) return;
    if (side == Side::Buy) bids_[price].push_back({ref, qty});
    else                   asks_[price].push_back({ref, qty});
}

bool SoftwareEngine::remove_ref(Side side, uint32_t price,
                                uint64_t ref) noexcept {
    auto strip = [&](auto& book) -> bool {
        auto lit = book.find(price);
        if (lit == book.end()) return false;
        auto& q = lit->second;
        for (auto qit = q.begin(); qit != q.end(); ++qit) {
            if (qit->first != ref) continue;
            q.erase(qit);
            if (q.empty()) book.erase(lit);
            return true;
        }
        return false;
    };
    return (side == Side::Buy) ? strip(bids_) : strip(asks_);
}

bool SoftwareEngine::reduce_ref(Side side, uint32_t price,
                                uint64_t ref, uint32_t qty) noexcept {
    auto shrink = [&](auto& book) -> bool {
        auto lit = book.find(price);
        if (lit == book.end()) return false;
        auto& q = lit->second;
        for (auto qit = q.begin(); qit != q.end(); ++qit) {
            if (qit->first != ref) continue;
            uint32_t take = std::min(qty, qit->second);
            qit->second -= take;
            if (qit->second == 0) q.erase(qit);
            if (q.empty()) book.erase(lit);
            return true;
        }
        return false;
    };
    return (side == Side::Buy) ? shrink(bids_) : shrink(asks_);
}

// maps are already ordered the right way per side, so this is just a walk
int SoftwareEngine::depth(Side side, DepthLevel* out, int max) const noexcept {
    int n = 0;
    auto walk = [&](const auto& book) {
        for (const auto& [px, queue] : book) {
            if (n >= max) break;
            uint32_t qty = 0;
            for (const auto& e : queue) qty += e.second;
            if (qty == 0) continue;
            out[n].price    = px;
            out[n].quantity = qty;
            out[n].orders   = static_cast<uint32_t>(queue.size());
            ++n;
        }
    };
    if (side == Side::Buy) walk(bids_); else walk(asks_);
    return n;
}

bool SoftwareEngine::best_bid(uint32_t& price, uint32_t& qty) const noexcept {
    if (bids_.empty()) return false;
    const auto& lvl = *bids_.begin();
    price = lvl.first;
    qty   = 0;
    for (const auto& e : lvl.second) qty += e.second;
    return true;
}

bool SoftwareEngine::best_ask(uint32_t& price, uint32_t& qty) const noexcept {
    if (asks_.empty()) return false;
    const auto& lvl = *asks_.begin();
    price = lvl.first;
    qty   = 0;
    for (const auto& e : lvl.second) qty += e.second;
    return true;
}

uint64_t SoftwareEngine::book_digest() const noexcept {
    uint64_t h = 0xcbf29ce484222325ULL;
    auto fold = [&h](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (i * 8)) & 0xFF;
            h *= 0x100000001b3ULL;
        }
    };
    // map iteration order is deterministic, so this is stable across runs
    auto walk = [&](const auto& book) {
        for (const auto& [px, queue] : book) {
            fold(px);
            for (const auto& e : queue) { fold(e.first); fold(e.second); }
        }
    };
    walk(bids_);
    walk(asks_);
    return h;
}

void SoftwareEngine::clear() noexcept {
    bids_.clear();
    asks_.clear();
    orders_processed_ = 0;
    matches_made_     = 0;
    shares_traded_    = 0;
    orphan_ops_       = 0;
    last_price_       = 0;
}