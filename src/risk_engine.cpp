#include "risk_engine.hpp"
#include <cstring>
#include <cstdlib>

RiskEngine::RiskEngine() noexcept
    : instrument_count_(0)
    , accepted_(0)
    , rejected_(0)
{
    for (auto& p : positions_)   std::memset(&p, 0, sizeof(p));
    for (auto& r : ref_prices_)  r = 0;
}

RiskResult RiskEngine::check(const Order& order) noexcept {
    // basic sanity — catch malformed orders before anything else
    if (order.quantity == 0 || order.price == 0) {
        ++rejected_;
        return RiskResult::InvalidOrder;
    }

    // fat finger — single order too large
    if (order.quantity > MAX_ORDER_SIZE) {
        ++rejected_;
        return RiskResult::ExceedsMaxSize;
    }

    int idx = find_or_create(order.stock);

    // price band check — reject if too far from reference
    uint32_t ref = ref_prices_[idx];
    if (ref > 0) {
        uint32_t diff = (order.price > ref)
            ? order.price - ref
            : ref - order.price;
        if (diff >= MAX_PRICE_BAND) {
            ++rejected_;
            return RiskResult::PriceOutOfBand;
        }
    }

    // position check — would this fill breach max net position?
    int64_t current = positions_[idx].net_position;
    int64_t delta   = (order.side == Side::Buy)
        ? static_cast<int64_t>(order.quantity)
        : -static_cast<int64_t>(order.quantity);
    int64_t projected = current + delta;

    if (projected > MAX_NET_POSITION || projected < -MAX_NET_POSITION) {
        ++rejected_;
        return RiskResult::ExceedsPosition;
    }

    ++accepted_;
    return RiskResult::Accepted;
}

void RiskEngine::on_fill(const char* stock, Side side, uint32_t quantity) noexcept {
    int idx = find_or_create(stock);

    if (side == Side::Buy) {
        positions_[idx].net_position  += quantity;
        positions_[idx].gross_bought  += quantity;
    } else {
        positions_[idx].net_position  -= quantity;
        positions_[idx].gross_sold    += quantity;
    }
}

void RiskEngine::set_reference_price(const char* stock, uint32_t price) noexcept {
    int idx = find_or_create(stock);
    ref_prices_[idx] = price;
}

int RiskEngine::find_or_create(const char* stock) noexcept {
    // linear scan — fine at 128 instruments
    for (int i = 0; i < instrument_count_; ++i)
        if (std::memcmp(positions_[i].stock, stock, 8) == 0)
            return i;

    // new instrument
    if (instrument_count_ < MAX_INSTRUMENTS) {
        std::memcpy(positions_[instrument_count_].stock, stock, 8);
        positions_[instrument_count_].net_position = 0;
        return instrument_count_++;
    }

    return 0; // fallback — shouldn't happen in practice
}