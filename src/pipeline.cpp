#include "pipeline.hpp"
#include <algorithm>
#include <cstring>

FpgaPipeline::FpgaPipeline() noexcept
    : base_price_(0)
    , base_set_(false)
    , cycles_(0)
    , orders_processed_(0)
    , matches_made_(0)
    , window_misses_(0)
{
    for (auto& s : stages_)      s = PipelineRegister{};
    for (auto& l : buy_levels_)  l.clear();
    for (auto& l : sell_levels_) l.clear();
    std::memset(buy_bits_,  0, sizeof(buy_bits_));
    std::memset(sell_bits_, 0, sizeof(sell_bits_));
}

MatchResult FpgaPipeline::clock(const Order& incoming) noexcept {
    ++cycles_;

    // advance back-to-front so we don't overwrite data still in flight
    MatchResult output = stage5_respond(stages_[4]).result;
    stages_[4] = stage4_execute(stages_[3]);
    stages_[3] = stage3_match  (stages_[2]);
    stages_[2] = stage2_route  (stages_[1]);
    stages_[1] = stage1_decode (incoming);

    if (incoming.valid)
        ++orders_processed_;

    return output;
}

MatchResult FpgaPipeline::flush() noexcept {
    Order empty{};
    empty.valid = 0;
    MatchResult last{};
    for (int i = 0; i < NUM_STAGES; ++i)
        last = clock(empty);
    return last;
}

PipelineRegister FpgaPipeline::stage1_decode(const Order& in) noexcept {
    PipelineRegister reg{};
    reg.order  = in;
    reg.valid  = in.valid;
    reg.stage  = 1;
    reg.result.matched = false;
    return reg;
}

// address arithmetic, not a search
PipelineRegister FpgaPipeline::stage2_route(const PipelineRegister& in) noexcept {
    PipelineRegister reg = in;
    reg.stage = 2;
    if (!in.valid) return reg;

    const int slot = slot_of(in.order.price);
    if (slot < 0) {
        // outside the ladder window — drop rather than misprice it
        ++window_misses_;
        reg.valid = false;
        return reg;
    }
    reg.level_index = static_cast<uint32_t>(slot);
    return reg;
}

PipelineRegister FpgaPipeline::stage3_match(const PipelineRegister& in) noexcept {
    PipelineRegister reg = in;
    reg.stage = 3;
    if (!in.valid) return reg;

    if (in.order.side == Side::Buy) {
        PriceLevel* ask = best_ask();
        if (ask && !ask->is_empty() && ask->price <= in.order.price) {
            reg.result.matched        = true;
            reg.result.buy_order_ref  = in.order.order_ref;
            reg.result.sell_order_ref = ask->order_refs[0];
            reg.result.price          = ask->price;
            reg.result.quantity       = std::min(in.order.quantity,
                                                 ask->quantities[0]);
        }
    } else {
        PriceLevel* bid = best_bid();
        if (bid && !bid->is_empty() && bid->price >= in.order.price) {
            reg.result.matched        = true;
            reg.result.buy_order_ref  = bid->order_refs[0];
            reg.result.sell_order_ref = in.order.order_ref;
            reg.result.price          = bid->price;
            reg.result.quantity       = std::min(in.order.quantity,
                                                 bid->quantities[0]);
        }
    }
    return reg;
}

PipelineRegister FpgaPipeline::stage4_execute(const PipelineRegister& in) noexcept {
    PipelineRegister reg = in;
    reg.stage = 4;
    if (!in.valid) return reg;

    if (in.result.matched) {
        const bool  buy_side = (in.order.side == Side::Buy);
        const int   slot     = buy_side ? bottom_slot(sell_bits_)
                                        : top_slot(buy_bits_);
        if (slot >= 0) {
            PriceLevel* resting = buy_side ? &sell_levels_[slot]
                                           : &buy_levels_[slot];
            if (!resting->is_empty()) {
                resting->quantities[0] -= in.result.quantity;
                if (resting->quantities[0] == 0) {
                    // shift remaining orders forward
                    for (int i = 0; i < resting->count - 1; ++i) {
                        resting->order_refs[i] = resting->order_refs[i + 1];
                        resting->quantities[i] = resting->quantities[i + 1];
                    }
                    --resting->count;
                    if (resting->count == 0)
                        clear_bit(buy_side ? sell_bits_ : buy_bits_, slot);
                }
            }
        }
        ++matches_made_;
    } else {
        // no match — rest in book at the routed slot
        const int   slot  = static_cast<int>(in.level_index);
        const bool  buy   = (in.order.side == Side::Buy);
        PriceLevel* level = buy ? &buy_levels_[slot] : &sell_levels_[slot];

        if (!level->is_full()) {
            level->order_refs[level->count] = in.order.order_ref;
            level->quantities[level->count] = in.order.quantity;
            level->price = in.order.price;
            level->side  = buy ? Side::Buy : Side::Sell;
            ++level->count;
            set_bit(buy ? buy_bits_ : sell_bits_, slot);
        }
    }
    return reg;
}

PipelineRegister FpgaPipeline::stage5_respond(const PipelineRegister& in) noexcept {
    PipelineRegister reg = in;
    reg.stage = 5;
    // OUCH serialization goes here in a later milestone
    return reg;
}
