#pragma once
#include <cstdint>
#include "software_engine.hpp"

// terminal order book visualizer
// uses ANSI escape codes — works in MSYS2 terminal
//
// this holds no book state. it pulls a depth snapshot from the engine and
// draws it. a display that maintains its own copy of the book is a second
// source of truth, and the two drift.
class Visualizer {
public:
    static constexpr int MAX_LEVELS = 10;

    struct SessionInfo {
        const char* symbol;
        uint64_t    timestamp_ns;    // ITCH clock, ns since midnight
        uint64_t    messages;
        uint64_t    trades;
        uint64_t    shares;
        uint32_t    last_price;
        double      speed;           // replay multiplier, 0 = unpaced
    };

    Visualizer() noexcept;

    void render(const SoftwareEngine& book, const SessionInfo& info) noexcept;

    static void clear()        noexcept;
    static void hide_cursor()  noexcept;
    static void show_cursor()  noexcept;

private:
    // previous top of book, so we can flag what moved since the last frame
    uint32_t prev_bid_px_;
    uint32_t prev_ask_px_;
    uint32_t prev_last_;

    static void set_green()   noexcept;
    static void set_red()     noexcept;
    static void set_yellow()  noexcept;
    static void set_cyan()    noexcept;
    static void set_dim()     noexcept;
    static void set_white()   noexcept;
    static void reset()       noexcept;
    static void move_to_top() noexcept;

    // ITCH timestamps are ns since midnight
    static void fmt_time(uint64_t ns, char* out) noexcept;  // out >= 24 bytes
};
