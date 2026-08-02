#pragma once
#include <vector>
#include <map>
#include <deque>
#include "itch_parser.hpp"
#include "pipeline.hpp"
#include "book_state.hpp"

// naive software matching engine — standard implementation
// uses STL containers with dynamic allocation
// exists purely for benchmark comparison against the FPGA pipeline
//
// two entry points, not interchangeable:
//   submit()  we decide whether the order crosses. what the benchmark measures.
//   apply()   replay. ITCH adds already went through NASDAQ's matcher and did
//             not cross, so they only rest — trades arrive as E/C/P.
class SoftwareEngine {
public:
    SoftwareEngine() = default;

    // submit an order, returns match result immediately
    MatchResult submit(const Order& order) noexcept;

    void apply(const ResolvedOp& op) noexcept;

    uint64_t orders_processed() const noexcept { return orders_processed_; }
    uint64_t matches_made()     const noexcept { return matches_made_; }
    uint64_t shares_traded()    const noexcept { return shares_traded_; }
    uint64_t orphan_ops()       const noexcept { return orphan_ops_; }

    // aggregated top-of-book snapshot, best price first. returns level count.
    struct DepthLevel {
        uint32_t price;
        uint32_t quantity;   // total resting size at this price
        uint32_t orders;     // how many orders make it up
    };
    int depth(Side side, DepthLevel* out, int max) const noexcept;

    bool   best_bid(uint32_t& price, uint32_t& qty) const noexcept;
    bool   best_ask(uint32_t& price, uint32_t& qty) const noexcept;
    size_t bid_levels() const noexcept { return bids_.size(); }
    size_t ask_levels() const noexcept { return asks_.size(); }
    uint32_t last_price() const noexcept { return last_price_; }

    // FNV-1a over the whole ladder. proves two engines built the same book.
    uint64_t book_digest() const noexcept;
    
    void clear() noexcept;

private:
    // price -> queue of (order_ref, quantity)
    // std::map gives us sorted price levels automatically
    // descending for bids, ascending for asks
    using Level = std::deque<std::pair<uint64_t, uint32_t>>;

    std::map<uint32_t, Level, std::greater<uint32_t>> bids_;
    std::map<uint32_t, Level>                         asks_;

    uint64_t orders_processed_ = 0;
    uint64_t matches_made_     = 0;
    uint64_t shares_traded_    = 0;
    uint64_t orphan_ops_       = 0;   // ref wasn't at the level we expected
    uint32_t last_price_       = 0;

    MatchResult try_match_buy (const Order& order) noexcept;
    MatchResult try_match_sell(const Order& order) noexcept;

    // BookState gives us side and price, so we scan one level rather than the
    // whole book. a real engine holds an intrusive node pointer for O(1).
    void rest_order(Side side, uint32_t price, uint64_t ref, uint32_t qty) noexcept;
    bool remove_ref(Side side, uint32_t price, uint64_t ref) noexcept;
    bool reduce_ref(Side side, uint32_t price, uint64_t ref, uint32_t qty) noexcept;
};