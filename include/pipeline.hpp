#pragma once
#include <cstdint>
#include "itch_parser.hpp"

struct MatchResult {
    uint64_t buy_order_ref;
    uint64_t sell_order_ref;
    uint32_t price;
    uint32_t quantity;
    bool     matched;
};

// fixed-size price level — models FPGA block RAM, no heap
struct PriceLevel {
    static constexpr int MAX_ORDERS = 16;

    uint64_t order_refs[MAX_ORDERS];
    uint32_t quantities[MAX_ORDERS];
    uint32_t price;
    int      count;
    Side     side;

    void clear() noexcept { count = 0; price = 0; }
    bool is_full()  const noexcept { return count >= MAX_ORDERS; }
    bool is_empty() const noexcept { return count == 0; }
};

// flows between pipeline stages, like a real FPGA register
struct PipelineRegister {
    Order       order;
    uint32_t    level_index;
    MatchResult result;
    bool        valid;
    uint8_t     stage;
    uint8_t     pad[2];
};

// Price-indexed ladder, not a searched table.
//
// A price tick maps straight to a slot: slot = (price - base) / TICK. That is
// one subtract and one shift, the same address arithmetic an FPGA does into
// BRAM. The previous linear scan over occupied levels was an artifact of
// simulating a CAM in software and cost O(levels) per order.
//
// The ladder covers a fixed window around the opening price. Real systems
// either slide that window or fall back to a slow path; here anything outside
// is counted in window_misses() rather than silently mispriced.
class FpgaPipeline {
public:
    static constexpr int NUM_STAGES    = 5;
    static constexpr int CYCLE_LATENCY = 5;

    static constexpr uint32_t TICK         = 100;   // $0.01 at x10000
    static constexpr int      LADDER_SLOTS = 2048;  // $20.48 window
    static constexpr int      BITMAP_WORDS = LADDER_SLOTS / 64;
    static constexpr int      MAX_LEVELS   = LADDER_SLOTS;  // legacy name

    FpgaPipeline() noexcept;

    // advance one clock cycle, inject new order, return result from stage 5
    MatchResult clock(const Order& incoming) noexcept;

    // drain remaining orders after last input
    MatchResult flush() noexcept;

    uint64_t cycles_elapsed()   const noexcept { return cycles_; }
    uint64_t orders_processed() const noexcept { return orders_processed_; }
    uint64_t matches_made()     const noexcept { return matches_made_; }
    uint64_t window_misses()    const noexcept { return window_misses_; }
    uint32_t window_base()      const noexcept { return base_price_; }

private:
    PipelineRegister stage1_decode (const Order& in)             noexcept;
    PipelineRegister stage2_route  (const PipelineRegister& in)  noexcept;
    PipelineRegister stage3_match  (const PipelineRegister& in)  noexcept;
    PipelineRegister stage4_execute(const PipelineRegister& in)  noexcept;
    PipelineRegister stage5_respond(const PipelineRegister& in)  noexcept;

    PipelineRegister stages_[NUM_STAGES];
    PriceLevel       buy_levels_[LADDER_SLOTS];
    PriceLevel       sell_levels_[LADDER_SLOTS];

    // one bit per slot. finding best bid/ask is a priority encode over these,
    // which is a single cycle in hardware and a few clz/ctz here.
    uint64_t buy_bits_[BITMAP_WORDS];
    uint64_t sell_bits_[BITMAP_WORDS];

    uint32_t base_price_;      // price sitting at slot 0
    bool     base_set_;

    uint64_t cycles_;
    uint64_t orders_processed_;
    uint64_t matches_made_;
    uint64_t window_misses_;

    // -1 when the price falls outside the ladder window
    int slot_of(uint32_t price) noexcept {
        if (!base_set_) {
            // centre the window on the first price we see
            const uint32_t half = (LADDER_SLOTS / 2) * TICK;
            base_price_ = (price > half) ? (price - half) : 0;
            base_set_   = true;
        }
        if (price < base_price_) return -1;
        const uint32_t off = (price - base_price_) / TICK;
        return (off < static_cast<uint32_t>(LADDER_SLOTS))
                   ? static_cast<int>(off) : -1;
    }

    static void set_bit(uint64_t* bits, int slot) noexcept {
        bits[slot >> 6] |= (uint64_t{1} << (slot & 63));
    }
    static void clear_bit(uint64_t* bits, int slot) noexcept {
        bits[slot >> 6] &= ~(uint64_t{1} << (slot & 63));
    }

    // highest occupied slot — best bid
    int top_slot(const uint64_t* bits) const noexcept {
        for (int w = BITMAP_WORDS - 1; w >= 0; --w)
            if (bits[w])
                return (w << 6) + (63 - __builtin_clzll(bits[w]));
        return -1;
    }
    // lowest occupied slot — best ask
    int bottom_slot(const uint64_t* bits) const noexcept {
        for (int w = 0; w < BITMAP_WORDS; ++w)
            if (bits[w])
                return (w << 6) + __builtin_ctzll(bits[w]);
        return -1;
    }

    PriceLevel* best_bid() noexcept {
        const int s = top_slot(buy_bits_);
        return (s < 0) ? nullptr : &buy_levels_[s];
    }
    PriceLevel* best_ask() noexcept {
        const int s = bottom_slot(sell_bits_);
        return (s < 0) ? nullptr : &sell_levels_[s];
    }
};
