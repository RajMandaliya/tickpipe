#pragma once
#include <cstdint>

// Market microstructure analytics in integer fixed point.
//
// No floating point anywhere in the hot path. This is not stylistic — an FPGA
// has no FPU, and float64 results are not reproducible across compilers or
// optimisation levels, so a float implementation cannot be checksummed the way
// the rest of this system is.
//
// Representation:
//   price      uint32, units of 1e-4 dollars      ($218.1234 -> 2181234)
//   quantity   uint32, shares
//   outputs    int64,  units documented per field
//
// Every metric has a float64 twin in the validation harness; the README quotes
// the measured error bound rather than asserting correctness.

class Microstructure {
public:
    struct Metrics {
        // Stoikov microprice: size-weighted fair value. Weights are crossed on
        // purpose — heavy bid size pulls fair value toward the ask, because
        // the thin side is the side that moves.
        //   P = (Pb*Qa + Pa*Qb) / (Qa + Qb)
        int64_t  microprice_1e8;

        int64_t  mid_1e8;             // (Pb + Pa) / 2
        int64_t  micro_minus_mid_1e8; // the part that carries information

        // Cont-Kukanov-Stoikov order flow imbalance, in shares.
        // Increments only when top of book changes; sign encodes direction.
        int64_t  ofi;

        // 2 * |Ptrade - Pmid|, in 1e-4 dollars. Exact: doubling removes the
        // half-tick the midpoint would otherwise introduce.
        uint64_t effective_spread_sum_1e4;
        uint64_t effective_spread_n;

        // sqrt(sum of squared log returns), 1e-9 units
        uint64_t realized_vol_1e9;
        uint64_t return_samples;

        // Kyle's lambda: price impact per share.  sum(dP*V) / sum(V*V)
        int64_t  kyle_lambda_1e12;

        uint64_t book_updates;
        uint64_t trades;

        // locked (bid == ask) or crossed (bid > ask) books. microprice and
        // spread are undefined there, so those updates are counted and skipped
        // rather than allowed to poison the averages.
        uint64_t degenerate_books;
    };

    Microstructure() noexcept;

    // call on every top-of-book change
    void on_book(uint32_t bid_px, uint32_t bid_qty,
                 uint32_t ask_px, uint32_t ask_qty) noexcept;

    // call on every execution. side = the aggressor.
    void on_trade(uint32_t px, uint32_t qty, bool buyer_initiated) noexcept;

    const Metrics& metrics() const noexcept { return m_; }
    void reset() noexcept;

    double effective_spread_bps() const noexcept;   // reporting only

    // ---- exposed for the validation harness ----

    // log(1+x) for |x| << 1 via truncated series, x and result in 1e-12 units.
    // Returns are held at 1e-12 rather than 1e-9 because the quantization of x
    // dominates the error, not the series truncation: at |x| ~ 1e-4, one unit
    // of 1e-9 is already 1e-5 relative.
    static int64_t log1p_1e12(int64_t x_1e12) noexcept;

    // integer square root of a 128-bit value
    static uint64_t isqrt128(unsigned __int128 v) noexcept;

private:
    Metrics m_{};

    // previous top of book, for OFI and returns
    uint32_t pb_, qb_, pa_, qa_;
    bool     have_prev_;

    unsigned __int128 sum_sq_ret_1e18_;   // squared log returns, 1e-24 units
    __int128 sum_dp_v_;                   // Kyle numerator
    unsigned __int128 sum_v_sq_;          // Kyle denominator
    int64_t  prev_mid_1e4_x2_;            // doubled, so it stays integral

    // Kyle's regression needs the mid as of the PREVIOUS trade. Reusing the
    // per-update mid makes dP identically zero, because on_book has already
    // advanced it by the time a trade arrives.
    int64_t  prev_trade_mid_x2_;
};