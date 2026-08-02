#pragma once
#include <cstdint>
#include <cstring>
#include "itch_parser.hpp"

// rejection reasons — tells the caller exactly why an order was blocked
enum class RiskResult : uint8_t {
    Accepted        = 0,
    ExceedsMaxSize,      // single order too large
    ExceedsPosition,     // would breach max net position
    PriceOutOfBand,      // price too far from reference
    InvalidOrder,        // malformed — zero qty, zero price etc
};

// per-instrument position tracker
// fixed array of instruments — no heap
struct PositionEntry {
    char     stock[8];
    int64_t  net_position;   // positive = long, negative = short
    uint64_t gross_bought;
    uint64_t gross_sold;
};

class RiskEngine {
public:
    static constexpr uint32_t MAX_ORDER_SIZE    = 10000;    // shares per order
    static constexpr int64_t  MAX_NET_POSITION  = 100000;   // max long or short
    static constexpr uint32_t MAX_PRICE_BAND    = 500000;   // $50.00 from reference x10000
    static constexpr int      MAX_INSTRUMENTS   = 128;

    RiskEngine() noexcept;

    // check order before sending to pipeline
    // returns Accepted if safe, rejection reason otherwise
    RiskResult check(const Order& order) noexcept;

    // call after a match is confirmed to update position
    void on_fill(const char* stock, Side side, uint32_t quantity) noexcept;

    // set reference price for an instrument — used for price band check
    void set_reference_price(const char* stock, uint32_t price) noexcept;

    // stats
    uint64_t orders_accepted() const noexcept { return accepted_; }
    uint64_t orders_rejected() const noexcept { return rejected_; }

private:
    PositionEntry positions_[MAX_INSTRUMENTS];
    uint32_t      ref_prices_[MAX_INSTRUMENTS];
    int           instrument_count_;

    uint64_t accepted_;
    uint64_t rejected_;

    // find position entry for a stock ticker
    // creates one if it doesn't exist
    int find_or_create(const char* stock) noexcept;
};