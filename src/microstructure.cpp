#include "microstructure.hpp"

Microstructure::Microstructure() noexcept { reset(); }

void Microstructure::reset() noexcept {
    m_ = Metrics{};
    pb_ = qb_ = pa_ = qa_ = 0;
    have_prev_ = false;
    sum_sq_ret_1e18_ = 0;
    sum_dp_v_ = 0;
    sum_v_sq_ = 0;
    prev_mid_1e4_x2_ = 0;
    prev_trade_mid_x2_ = 0;
}

// log(1+x) = x - x^2/2 + x^3/3 - x^4/4 + ...
//
// Inputs are tick-to-tick returns, so |x| is on the order of 1e-4 and the
// series converges immediately. Four terms bound the truncation error at
// |x|^5/5 < 2e-21 for |x| < 1e-3, which is nine orders of magnitude below one
// unit of output.
//
// The scale is 1e-12, not 1e-9: quantizing x to 1e-9 when |x| ~ 1e-4 is
// already a 1e-5 relative error, and that — not the series — was what limited
// accuracy at 1e-9.
//
// 128-bit intermediates are required: x^4 at |x| = 1e-2 is (1e10)^4 = 1e40
// before rescaling, so each power is divided down as it is formed.
int64_t Microstructure::log1p_1e12(int64_t x) noexcept {
    const __int128 X = x;
    const __int128 S = 1000000000000LL;                // 1e12

    const __int128 x2 = (X * X) / S;                   // x^2
    const __int128 x3 = (x2 * X) / S;                  // x^3
    const __int128 x4 = (x3 * X) / S;                  // x^4

    const __int128 r = X - x2 / 2 + x3 / 3 - x4 / 4;
    return static_cast<int64_t>(r);
}

// Bit-by-bit integer square root. No std::sqrt: it returns double, and a
// double result would make the checksum compiler-dependent.
uint64_t Microstructure::isqrt128(unsigned __int128 v) noexcept {
    if (v == 0) return 0;
    unsigned __int128 rem = 0, root = 0;
    for (int i = 63; i >= 0; --i) {
        root <<= 1;
        rem = (rem << 2) | ((v >> (i * 2)) & 3);
        if (rem > root) {
            rem -= root | 1;
            root |= 2;
        }
    }
    return static_cast<uint64_t>(root >> 1);
}

void Microstructure::on_book(uint32_t bid_px, uint32_t bid_qty,
                             uint32_t ask_px, uint32_t ask_qty) noexcept {
    if (bid_px == 0 || ask_px == 0 || bid_qty == 0 || ask_qty == 0) return;

    // microprice is only defined on a two-sided, uncrossed book
    if (bid_px >= ask_px) { ++m_.degenerate_books; return; }

    ++m_.book_updates;

    // ---- microprice -------------------------------------------------------
    // Pb*Qa peaks near 4.3e9 * 4.3e9 = 1.8e19 in the worst case, which is the
    // top of uint64. Two of them summed, then scaled by 1e4, needs 128 bits.
    const unsigned __int128 num =
        (static_cast<unsigned __int128>(bid_px) * ask_qty +
         static_cast<unsigned __int128>(ask_px) * bid_qty) * 10000u;
    const unsigned __int128 den = static_cast<unsigned __int128>(bid_qty) + ask_qty;

    m_.microprice_1e8 = static_cast<int64_t>(num / den);

    // mid needs no rounding: sum first, scale by 1e4/2 = 5000
    m_.mid_1e8 = (static_cast<int64_t>(bid_px) + ask_px) * 5000;
    m_.micro_minus_mid_1e8 = m_.microprice_1e8 - m_.mid_1e8;

    // ---- order flow imbalance --------------------------------------------
    // e = 1{Pb>=Pb'}Qb - 1{Pb<=Pb'}Qb' - 1{Pa<=Pa'}Qa + 1{Pa>=Pa'}Qa'
    if (have_prev_) {
        int64_t e = 0;
        if (bid_px >= pb_) e += static_cast<int64_t>(bid_qty);
        if (bid_px <= pb_) e -= static_cast<int64_t>(qb_);
        if (ask_px <= pa_) e -= static_cast<int64_t>(ask_qty);
        if (ask_px >= pa_) e += static_cast<int64_t>(qa_);
        m_.ofi += e;

        // ---- realized volatility -----------------------------------------
        // returns are computed on the doubled mid so no half-tick is lost
        const int64_t mid_x2 = static_cast<int64_t>(bid_px) + ask_px;
        if (prev_mid_1e4_x2_ > 0 && mid_x2 != prev_mid_1e4_x2_) {
            const __int128 d = static_cast<__int128>(mid_x2) - prev_mid_1e4_x2_;
            const int64_t x_1e12 =
                static_cast<int64_t>((d * 1000000000000LL) / prev_mid_1e4_x2_);

            const int64_t r = log1p_1e12(x_1e12);      // 1e-12 units
            sum_sq_ret_1e18_ += static_cast<unsigned __int128>(
                static_cast<__int128>(r) * r);         // now 1e-24 units
            ++m_.return_samples;
            // sqrt of a 1e-24 quantity is 1e-12; report at 1e-9
            m_.realized_vol_1e9 = isqrt128(sum_sq_ret_1e18_) / 1000;
        }
        prev_mid_1e4_x2_ = mid_x2;
    } else {
        prev_mid_1e4_x2_ = static_cast<int64_t>(bid_px) + ask_px;
        have_prev_ = true;
    }

    pb_ = bid_px; qb_ = bid_qty;
    pa_ = ask_px; qa_ = ask_qty;
}

void Microstructure::on_trade(uint32_t px, uint32_t qty,
                              bool buyer_initiated) noexcept {
    if (!have_prev_ || pb_ == 0 || pa_ == 0) return;

    ++m_.trades;

    // effective spread = 2|P - M|. Doubling both sides keeps it exact:
    // 2P - (Pb + Pa), no division, no half-tick rounding.
    const int64_t mid_x2 = static_cast<int64_t>(pb_) + pa_;
    const int64_t d = 2 * static_cast<int64_t>(px) - mid_x2;
    m_.effective_spread_sum_1e4 += static_cast<uint64_t>(d < 0 ? -d : d);
    ++m_.effective_spread_n;

    // ---- Kyle's lambda ---------------------------------------------------
    // slope of dP on signed volume: sum(dP*V) / sum(V^2), where dP is the mid
    // move since the LAST TRADE, not since the last book update
    const int64_t v = buyer_initiated ? static_cast<int64_t>(qty)
                                      : -static_cast<int64_t>(qty);
    if (prev_trade_mid_x2_ > 0) {
        const int64_t dp_x2 = mid_x2 - prev_trade_mid_x2_;
        sum_dp_v_ += static_cast<__int128>(dp_x2) * v;
        sum_v_sq_ += static_cast<unsigned __int128>(
            static_cast<__int128>(v) * v);
        if (sum_v_sq_ > 0) {
            // dp is doubled and in 1e-4 dollars, so the full conversion to
            // 1e-12 dollars/share is x1e12 / 2 / 1e4 = x5e7. Folding it into a
            // single division avoids truncating twice.
            m_.kyle_lambda_1e12 = static_cast<int64_t>(
                (sum_dp_v_ * 50000000) / static_cast<__int128>(sum_v_sq_));
        }
    }
    prev_trade_mid_x2_ = mid_x2;
}

// reporting only — never used in a computation that feeds a checksum
double Microstructure::effective_spread_bps() const noexcept {
    if (m_.effective_spread_n == 0 || m_.mid_1e8 == 0) return 0.0;
    const double avg = static_cast<double>(m_.effective_spread_sum_1e4)
                     / static_cast<double>(m_.effective_spread_n);
    return avg / (static_cast<double>(m_.mid_1e8) / 1e4) * 10000.0;
}