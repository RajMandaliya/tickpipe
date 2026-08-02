#include "visualizer.hpp"
#include <cstdio>
#include <cstring>
#include <algorithm>

static void ansi(const char* code) noexcept { std::fputs(code, stdout); }

void Visualizer::set_green()    noexcept { ansi("\033[32m"); }
void Visualizer::set_red()      noexcept { ansi("\033[31m"); }
void Visualizer::set_yellow()   noexcept { ansi("\033[33m"); }
void Visualizer::set_cyan()     noexcept { ansi("\033[36m"); }
void Visualizer::set_dim()      noexcept { ansi("\033[90m"); }
void Visualizer::set_white()    noexcept { ansi("\033[37m"); }
void Visualizer::reset()        noexcept { ansi("\033[0m");  }
void Visualizer::move_to_top()  noexcept { ansi("\033[H");   }
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

    move_to_top();

    set_white();
    std::printf("  Tickpipe  %-6s  %s", info.symbol, clock);
    if (info.speed > 0.0) std::printf("   %.0fx", info.speed);
    std::printf("                    \n");
    set_dim();
    std::printf("  ============================================================\n");

    reset();
    std::printf("  %8s %10s %8s | %-8s %-10s %-8s\n",
                "ORDERS", "BID QTY", "BID", "ASK", "ASK QTY", "ORDERS");
    set_dim();
    std::printf("  ------------------------------------------------------------\n");
    reset();

    int rows = std::max(nb, na);
    if (rows == 0) rows = 1;

    for (int i = 0; i < rows && i < MAX_LEVELS; ++i) {
        if (i < nb) {
            // inside price highlighted, depth behind it dimmer
            if (i == 0) set_green(); else set_dim();
            std::printf("  %8u %10u %8.2f",
                        bid[i].orders, bid[i].quantity, bid[i].price / 10000.0);
            reset();
        } else {
            std::printf("  %8s %10s %8s", "", "", "");
        }

        set_dim(); std::printf(" | "); reset();

        if (i < na) {
            if (i == 0) set_red(); else set_dim();
            std::printf("%-8.2f %-10u %-8u",
                        ask[i].price / 10000.0, ask[i].quantity, ask[i].orders);
            reset();
        } else {
            std::printf("%-8s %-10s %-8s", "", "", "");
        }
        std::printf("\n");
    }

    set_dim();
    std::printf("  ------------------------------------------------------------\n");
    reset();

    // spread only means anything with both sides present
    if (nb > 0 && na > 0) {
        // signed: locked (0) and crossed (negative) markets are real
        const int64_t diff = static_cast<int64_t>(ask[0].price)
                           - static_cast<int64_t>(bid[0].price);
        const double spread = diff / 10000.0;
        const double mid    = (static_cast<int64_t>(ask[0].price)
                             + static_cast<int64_t>(bid[0].price)) / 20000.0;
        if (diff < 0) set_red(); else set_cyan();
        std::printf("  spread $%-7.2f mid $%-10.2f", spread, mid);
        reset();
    } else {
        std::printf("  spread %-8s mid %-14s", "--", "--");
    }

    if (info.last_price > 0) {
        if      (info.last_price > prev_last_ && prev_last_) set_green();
        else if (info.last_price < prev_last_ && prev_last_) set_red();
        else                                                 set_yellow();
        std::printf("last $%.2f      \n", info.last_price / 10000.0);
        reset();
    } else {
        std::printf("last %-10s\n", "--");
    }

    std::printf("  msgs %-12llu trades %-10llu shares %-12llu\n",
                static_cast<unsigned long long>(info.messages),
                static_cast<unsigned long long>(info.trades),
                static_cast<unsigned long long>(info.shares));
    std::printf("  levels %d x %d                                        \n",
                static_cast<int>(eng.bid_levels()),
                static_cast<int>(eng.ask_levels()));

    prev_bid_px_ = (nb > 0) ? bid[0].price : 0;
    prev_ask_px_ = (na > 0) ? ask[0].price : 0;
    prev_last_   = info.last_price;

    std::fflush(stdout);
}
