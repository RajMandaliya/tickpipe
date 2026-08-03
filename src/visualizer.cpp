#include "visualizer.hpp"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <algorithm>

// One frame is ~25 printf calls and a dozen ANSI escapes. On Windows each of
// those is a separate trip into conhost, which costs ~30ms per frame and made
// the console the slowest component in the whole system.
//
// So the frame is assembled in a stack buffer and pushed with a single
// fwrite. Same output, one syscall.

namespace {
constexpr size_t FRAME_CAP = 16384;

struct Frame {
    char   buf[FRAME_CAP];
    size_t len = 0;

    void put(const char* s) noexcept {
        const size_t n = std::strlen(s);
        if (len + n < FRAME_CAP) { std::memcpy(buf + len, s, n); len += n; }
    }
    void putf(const char* fmt, ...) noexcept __attribute__((format(printf, 2, 3)));
    void flush() noexcept {
        std::fwrite(buf, 1, len, stdout);
        std::fflush(stdout);
        len = 0;
    }
};

void Frame::putf(const char* fmt, ...) noexcept {
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf + len, FRAME_CAP - len, fmt, ap);
    va_end(ap);
    if (n > 0) len += static_cast<size_t>(n);
}

const char* GREEN  = "\033[32m";
const char* RED    = "\033[31m";
const char* YELLOW = "\033[33m";
const char* CYAN   = "\033[36m";
const char* DIM    = "\033[90m";
const char* WHITE  = "\033[37m";
const char* RESET  = "\033[0m";
} // namespace

static void ansi(const char* code) noexcept { std::fputs(code, stdout); }

void Visualizer::set_green()    noexcept { ansi(GREEN); }
void Visualizer::set_red()      noexcept { ansi(RED); }
void Visualizer::set_yellow()   noexcept { ansi(YELLOW); }
void Visualizer::set_cyan()     noexcept { ansi(CYAN); }
void Visualizer::set_dim()      noexcept { ansi(DIM); }
void Visualizer::set_white()    noexcept { ansi(WHITE); }
void Visualizer::reset()        noexcept { ansi(RESET); }
void Visualizer::move_to_top()  noexcept { ansi("\033[H"); }
void Visualizer::clear()        noexcept { ansi("\033[2J\033[H"); }
void Visualizer::hide_cursor()  noexcept { ansi("\033[?25l"); }
void Visualizer::show_cursor()  noexcept { ansi("\033[?25h"); }

Visualizer::Visualizer() noexcept
    : prev_bid_px_(0)
    , prev_ask_px_(0)
    , prev_last_(0)
{}

void Visualizer::fmt_time(uint64_t ns, char* out) noexcept {
    uint64_t total_ms = ns / 1000000ULL;
    uint32_t ms   = static_cast<uint32_t>(total_ms % 1000);
    uint64_t secs = total_ms / 1000;
    uint32_t h = static_cast<uint32_t>(secs / 3600);
    uint32_t m = static_cast<uint32_t>((secs / 60) % 60);
    uint32_t s = static_cast<uint32_t>(secs % 60);
    std::snprintf(out, 24, "%02u:%02u:%02u.%03u", h % 100, m, s, ms);
}

void Visualizer::render(const SoftwareEngine& eng,
                        const SessionInfo& info) noexcept {
    SoftwareEngine::DepthLevel bid[MAX_LEVELS];
    SoftwareEngine::DepthLevel ask[MAX_LEVELS];
    const int nb = eng.depth(Side::Buy,  bid, MAX_LEVELS);
    const int na = eng.depth(Side::Sell, ask, MAX_LEVELS);

    char clock[24];
    fmt_time(info.timestamp_ns, clock);

    Frame f;
    f.put("\033[H");

    f.put(WHITE);
    f.putf("  tickpipe  %-6s  %s", info.symbol, clock);
    if (info.speed > 0.0) f.putf("   %.0fx", info.speed);
    f.put("                    \n");
    f.put(DIM);
    f.put("  ============================================================\n");

    f.put(RESET);
    f.putf("  %8s %10s %8s | %-8s %-10s %-8s\n",
           "ORDERS", "BID QTY", "BID", "ASK", "ASK QTY", "ORDERS");
    f.put(DIM);
    f.put("  ------------------------------------------------------------\n");
    f.put(RESET);

    int rows = std::max(nb, na);
    if (rows == 0) rows = 1;

    for (int i = 0; i < rows && i < MAX_LEVELS; ++i) {
        if (i < nb) {
            f.put(i == 0 ? GREEN : DIM);
            f.putf("  %8u %10u %8.2f",
                   bid[i].orders, bid[i].quantity, bid[i].price / 10000.0);
            f.put(RESET);
        } else {
            f.putf("  %8s %10s %8s", "", "", "");
        }

        f.put(DIM); f.put(" | "); f.put(RESET);

        if (i < na) {
            f.put(i == 0 ? RED : DIM);
            f.putf("%-8.2f %-10u %-8u",
                   ask[i].price / 10000.0, ask[i].quantity, ask[i].orders);
            f.put(RESET);
        } else {
            f.putf("%-8s %-10s %-8s", "", "", "");
        }
        f.put("\n");
    }

    f.put(DIM);
    f.put("  ------------------------------------------------------------\n");
    f.put(RESET);

    if (nb > 0 && na > 0) {
        // signed: locked (0) and crossed (negative) markets are real
        const int64_t diff = static_cast<int64_t>(ask[0].price)
                           - static_cast<int64_t>(bid[0].price);
        const double spread = diff / 10000.0;
        const double mid    = (static_cast<int64_t>(ask[0].price)
                             + static_cast<int64_t>(bid[0].price)) / 20000.0;
        f.put(diff < 0 ? RED : CYAN);
        f.putf("  spread $%-7.2f mid $%-10.2f", spread, mid);
        f.put(RESET);
    } else {
        f.putf("  spread %-8s mid %-14s", "--", "--");
    }

    if (info.last_price > 0) {
        if      (info.last_price > prev_last_ && prev_last_) f.put(GREEN);
        else if (info.last_price < prev_last_ && prev_last_) f.put(RED);
        else                                                 f.put(YELLOW);
        f.putf("last $%.2f      \n", info.last_price / 10000.0);
        f.put(RESET);
    } else {
        f.putf("last %-10s\n", "--");
    }

    f.putf("  msgs %-12llu trades %-10llu shares %-12llu\n",
           static_cast<unsigned long long>(info.messages),
           static_cast<unsigned long long>(info.trades),
           static_cast<unsigned long long>(info.shares));
    f.putf("  levels %d x %d                                        \n",
           static_cast<int>(eng.bid_levels()),
           static_cast<int>(eng.ask_levels()));

    prev_bid_px_ = (nb > 0) ? bid[0].price : 0;
    prev_ask_px_ = (na > 0) ? ask[0].price : 0;
    prev_last_   = info.last_price;

    f.flush();
}