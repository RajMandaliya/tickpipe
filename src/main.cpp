#include <iostream>
#include <chrono>
#include <cstring>
#include <vector>
#include <algorithm>
#include <thread>
#include "itch_parser.hpp"
#include "pipeline.hpp"
#include "software_engine.hpp"
#include "risk_engine.hpp"
#include "wal.hpp"
#include "ouch.hpp"
#include "numa.hpp"
#include "visualizer.hpp"
#include "itch_replay.hpp"
#include "book_state.hpp"
#include "software_engine.hpp"
#include "symbol_directory.hpp"

static uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now()
            .time_since_epoch()
            .count()
    );
}

static uint64_t percentile(std::vector<uint64_t>& v, double p) {
    if (v.empty()) return 0;
    size_t idx = static_cast<size_t>(p / 100.0 * v.size());
    if (idx >= v.size()) idx = v.size() - 1;
    return v[idx];
}

static void divider() {
    std::printf("  %s\n",
        "------------------------------------------------------------");
}

// ── Section 1: ITCH parser throughput ─────────────────────────────────────
static void demo_itch_parser() {
    std::printf("\n[ 1 ] ITCH Binary Parser\n");
    divider();

    uint8_t msg[] = {
        'A',
        0x00, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x0F, 0x42, 0x40, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
        'B',
        0x00, 0x00, 0x00, 0x64,
        'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ',
        0x00, 0x16, 0xE3, 0x60,
    };

    ItchParser parser;
    Order      order{};
    constexpr int SAMPLES = 1000000;

    // warmup
    for (int i = 0; i < 100000; ++i)
        parser.parse(msg, sizeof(msg), order);

    uint64_t t0 = now_ns();
    for (int i = 0; i < SAMPLES; ++i)
        parser.parse(msg, sizeof(msg), order);
    uint64_t t1 = now_ns();

    double elapsed_s  = (t1 - t0) / 1e9;
    double msgs_per_s = SAMPLES / elapsed_s;
    double ns_per_msg = static_cast<double>(t1 - t0) / SAMPLES;

    std::printf("  Parsed %d ITCH messages\n", SAMPLES);
    std::printf("  Throughput : %llu M messages/sec\n",
                static_cast<uint64_t>(msgs_per_s / 1e6));
    std::printf("  Latency    : %.1f ns per message\n", ns_per_msg);
    std::printf("  Protocol   : NASDAQ ITCH 5.0 binary, big-endian\n");
    std::printf("  Allocation : zero heap - fixed Order struct on stack\n");
}

// ── Section 2: FPGA vs Software latency comparison ────────────────────────
static void demo_latency_comparison() {
    std::printf("\n[ 2 ] FPGA Pipeline vs Software Engine - Latency\n");
    divider();

    using BenchClock = std::chrono::steady_clock;
    auto tick_ns = []() -> uint64_t {
        using namespace std::chrono;
        return static_cast<uint64_t>(duration_cast<nanoseconds>(
            BenchClock::now().time_since_epoch()).count());
    };

    // what this machine can actually resolve. on Windows steady_clock ticks
    // at ~100ns, so timing one sub-100ns operation measures the clock.
    uint64_t tick = UINT64_MAX;
    for (int i = 0; i < 50000; ++i) {
        const uint64_t a = tick_ns();
        uint64_t b;
        do { b = tick_ns(); } while (b == a);
        if (b - a < tick) tick = b - a;
    }

    constexpr int WARMUP = 10000;
    constexpr int BATCH  = 1000;
    constexpr int NB     = 1000;

    // spread across ~200 levels — one hot price is not a book
    auto lcg = [](uint32_t& s) { s = s * 1664525u + 1013904223u; return s; };
    auto make_order = [&lcg](uint32_t& seed, uint64_t id) -> Order {
        Order o{};
        o.order_ref = id;
        o.price     = 1500000 + (lcg(seed) % 200) * 100;
        o.quantity  = 100;
        o.side      = (id & 1) ? Side::Buy : Side::Sell;
        o.valid     = 1;
        return o;
    };

    struct Res { double mean, p50, p99, p999; double mops; };

    // time a batch and divide: each batch mean is one sample
    auto bench = [&](auto&& step) -> Res {
        for (int i = 0; i < WARMUP; ++i) step(i);

        std::vector<double> per_op;
        per_op.reserve(NB);
        uint64_t total = 0;
        for (int b = 0; b < NB; ++b) {
            const uint64_t t0 = tick_ns();
            for (int i = 0; i < BATCH; ++i) step(WARMUP + b * BATCH + i);
            const uint64_t t1 = tick_ns();
            total += (t1 - t0);
            per_op.push_back(static_cast<double>(t1 - t0) / BATCH);
        }
        std::sort(per_op.begin(), per_op.end());
        auto at = [&](double p) {
            size_t i = static_cast<size_t>(p / 100.0 * per_op.size());
            if (i >= per_op.size()) i = per_op.size() - 1;
            return per_op[i];
        };
        double sum = 0;
        for (double x : per_op) sum += x;
        return { sum / per_op.size(), at(50), at(99), at(99.9),
                 (BATCH * NB) / (total / 1e9) / 1e6 };
    };

    Res fpga{}, sw{};
    {
        FpgaPipeline pipeline;
        uint32_t seed = 999;
        fpga = bench([&](int i) { pipeline.clock(make_order(seed, i)); });
    }
    {
        SoftwareEngine engine;
        uint32_t seed = 999;
        sw = bench([&](int i) { engine.submit(make_order(seed, i)); });
    }

    std::printf("  Timer resolution on this machine: %llu ns\n",
                static_cast<unsigned long long>(tick));
    std::printf("  Figures are ns per operation, measured as batch means\n");
    std::printf("  over %d batches of %d.\n\n", NB, BATCH);

    std::printf("  %-20s %8s %8s %8s %8s %12s\n",
                "Engine", "mean", "p50", "p99", "p999", "throughput");
    divider();
    std::printf("  %-20s %8.1f %8.1f %8.1f %8.1f %8.1f M/s\n",
                "FPGA Pipeline", fpga.mean, fpga.p50, fpga.p99, fpga.p999,
                fpga.mops);
    std::printf("  %-20s %8.1f %8.1f %8.1f %8.1f %8.1f M/s\n",
                "Software Engine", sw.mean, sw.p50, sw.p99, sw.p999, sw.mops);
    divider();
    std::printf("  Tail: %.2fx at p999, %.2fx on mean\n",
                sw.p999 / fpga.p999, sw.mean / fpga.mean);

    std::printf("\n  The FPGA model enforces: no heap allocation, no\n");
    std::printf("  branching in the hot path, fixed pipeline stages.\n");
}

// ── Section 3: Risk engine ─────────────────────────────────────────────────
static void demo_risk_engine() {
    std::printf("\n[ 3 ] Risk Engine\n");
    divider();

    RiskEngine risk;
    char stock[8] = {'A','A','P','L',' ',' ',' ',' '};
    risk.set_reference_price(stock, 1500000);

    auto test = [&](const char* label, uint32_t qty,
                    uint32_t price, Side side) {
        Order o{};
        o.order_ref = 1;
        o.price     = price;
        o.quantity  = qty;
        o.side      = side;
        o.valid     = 1;
        std::memcpy(o.stock, stock, 8);

        RiskResult r = risk.check(o);
        const char* verdict = (r == RiskResult::Accepted)
            ? "ACCEPTED" : "REJECTED";
        std::printf("  [%s] %s\n", verdict, label);
    };

    test("Buy  100 AAPL @ $150.00 - normal order",
         100,   1500000, Side::Buy);
    test("Buy 50000 AAPL @ $150.00 - fat finger (limit: 10,000)",
         50000, 1500000, Side::Buy);
    test("Buy  100 AAPL @ $200.00 - price out of band ($50 from ref)",
         100,   2000000, Side::Buy);
    test("Buy    0 AAPL @ $150.00 - zero quantity",
         0,     1500000, Side::Buy);
    test("Sell 100 AAPL @ $150.00 - normal order",
         100,   1500000, Side::Sell);

    std::printf("\n  Accepted: %llu   Rejected: %llu\n",
                risk.orders_accepted(), risk.orders_rejected());
}

// ── Section 4: WAL crash recovery ─────────────────────────────────────────
static void demo_wal() {
    std::printf("\n[ 4 ] Write-Ahead Log - Crash Recovery\n");
    divider();

    const char* path = "exchange.wal";
    WriteAheadLog wal;
    wal.open(path);

    char stock[8] = {'A','A','P','L',' ',' ',' ',' '};

    auto write_entry = [&](uint64_t id, Side side,
                           uint32_t qty, WalEntryType type) {
        Order o{};
        o.order_ref = id;
        o.price     = 1500000;
        o.quantity  = qty;
        o.side      = side;
        o.valid     = 1;
        std::memcpy(o.stock, stock, 8);
        MatchResult empty{};
        wal.append(type, o, empty);
    };

    write_entry(1, Side::Sell, 100, WalEntryType::OrderSubmitted);
    write_entry(2, Side::Buy,  100, WalEntryType::OrderSubmitted);
    write_entry(3, Side::Buy,  100, WalEntryType::OrderMatched);
    write_entry(4, Side::Buy,  50000, WalEntryType::OrderRejected);
    write_entry(5, Side::Sell, 200, WalEntryType::OrderSubmitted);

    wal.close();
    std::printf("  Written %llu entries to %s\n",
                wal.entries_written(), path);
    std::printf("  Simulating crash...\n");
    std::printf("  Replaying log for recovery:\n\n");

    int count = 0;
    wal.replay(path, [](const WalEntry& e, void* ctx) {
        auto* n = static_cast<int*>(ctx);
        ++(*n);
        const char* type =
            e.type == WalEntryType::OrderSubmitted ? "submitted" :
            e.type == WalEntryType::OrderMatched   ? "matched"   :
                                                     "rejected";
        std::printf("    seq=%-3llu  %-10s  ref=%-3llu  qty=%u\n",
                    e.sequence, type,
                    e.order.order_ref, e.order.quantity);
    }, &count);

    std::printf("\n  Replayed %d entries - state fully recovered\n", count);
    std::remove(path);
}

// ── Section 5: OUCH protocol ───────────────────────────────────────────────
static void demo_ouch() {
    std::printf("\n[ 5 ] OUCH Binary Protocol\n");
    divider();

    char stock[8] = {'A','A','P','L',' ',' ',' ',' '};
    uint8_t buf[64];

    Order o{};
    o.order_ref = 42;
    o.price     = 1500000;
    o.quantity  = 100;
    o.side      = Side::Buy;
    o.valid     = 1;
    std::memcpy(o.stock, stock, 8);

    uint64_t ts = now_ns();

    uint32_t len = OuchSerializer::write_accepted(buf, sizeof(buf), o, ts);
    std::printf("  OrderAccepted  : %u bytes  ref=%llu\n",
                len, o.order_ref);

    len = OuchSerializer::write_rejected(buf, sizeof(buf), o, 1, ts);
    std::printf("  OrderRejected  : %u bytes  reason=fat_finger\n", len);

    MatchResult result{};
    result.matched        = true;
    result.buy_order_ref  = 42;
    result.sell_order_ref = 43;
    result.price          = 1500000;
    result.quantity       = 100;

    len = OuchSerializer::write_executed(buf, sizeof(buf), o, result, 1, ts);
    std::printf("  OrderExecuted  : %u bytes  qty=%u  price=$%.2f\n",
                len, result.quantity, result.price / 10000.0);

    std::printf("\n  Binary format, big-endian, zero-copy.\n");
    std::printf("  Clients parse response messages off the wire directly.\n");
}

// ── Section 6: NUMA thread pinning ────────────────────────────────────────
static void demo_numa() {
    std::printf("\n[ 6 ] NUMA Thread Pinning\n");
    divider();

    int cores = ThreadPinner::core_count();
    std::printf("  Available cores : %d\n", cores);
    std::printf("  Before pin      : core %d\n",
                ThreadPinner::current_core());

    ThreadPinner::pin_to_matching_core();
    std::printf("  After pin       : core %d (matching engine)\n",
                ThreadPinner::current_core());

    if (cores > 1) {
        std::thread io([&]{
            ThreadPinner::pin_to_core(1);
            std::printf("  I/O thread      : core %d\n",
                        ThreadPinner::current_core());
        });
        io.join();
    }

    std::printf("\n  Dedicated cores eliminate OS scheduler jitter.\n");
    std::printf("  Core 0: matching engine\n");
    std::printf("  Core 1: network I/O\n");
}

// ── Section 7: Live order book visualizer ─────────────────────────────────
namespace {

constexpr uint64_t MARKET_CLOSE = 57600000000000ULL;

struct ReplayCtx {
    BookState*      book;
    SoftwareEngine* engine;

    // the book is empty by 20:00 — every order gets cancelled after hours —
    // so end-of-file state says nothing. snapshot at the bell instead.
    bool     close_taken;
    uint64_t close_digest;
    uint32_t close_bid_px, close_bid_qty;
    uint32_t close_ask_px, close_ask_qty;
    uint32_t close_last;
    uint64_t close_shares;
    size_t   close_bid_levels, close_ask_levels;
};

void on_message(const Order& msg, void* ctx) {
    auto* c = static_cast<ReplayCtx*>(ctx);

    // first message at or past the bell, before applying it
    if (!c->close_taken && msg.timestamp_ns >= MARKET_CLOSE) {
        c->close_taken      = true;
        c->close_digest     = c->engine->book_digest();
        c->engine->best_bid(c->close_bid_px, c->close_bid_qty);
        c->engine->best_ask(c->close_ask_px, c->close_ask_qty);
        c->close_last       = c->engine->last_price();
        c->close_shares     = c->engine->shares_traded();
        c->close_bid_levels = c->engine->bid_levels();
        c->close_ask_levels = c->engine->ask_levels();
    }

    ResolveResult r;
    if (!c->book->resolve(msg, r)) return;
    for (uint8_t i = 0; i < r.count; ++i)
        c->engine->apply(r.ops[i]);
}

using PaceClock = std::chrono::steady_clock;

struct PacedCtx {
    BookState*      book;
    SoftwareEngine* engine;
    Visualizer*     viz;
    const char*     symbol;
    double          speed;
    uint64_t        itch_t0;
    uint64_t        msgs;
    PaceClock::time_point wall_t0;
    PaceClock::time_point next_render;
    bool            started;
};

// 09:30:00 in ns since midnight
constexpr uint64_t MARKET_OPEN = 34200000000000ULL;

void on_paced(const Order& msg, void* c) {
    auto* x = static_cast<PacedCtx*>(c);

    ResolveResult r;
    if (x->book->resolve(msg, r))
        for (uint8_t i = 0; i < r.count; ++i) x->engine->apply(r.ops[i]);
    ++x->msgs;

    // pre-open builds the book at full speed, nothing to watch yet
    if (msg.timestamp_ns < MARKET_OPEN) return;

    if (!x->started) {
        x->started     = true;
        x->itch_t0     = msg.timestamp_ns;
        x->wall_t0     = PaceClock::now();
        x->next_render = x->wall_t0;
    }

    // where this message belongs on the wall clock
    const int64_t sim_ns =
        static_cast<int64_t>((msg.timestamp_ns - x->itch_t0) / x->speed);
    const auto target = x->wall_t0 + std::chrono::nanoseconds(sim_ns);
    const auto now    = PaceClock::now();

    // only sleep when meaningfully ahead — a syscall per message would dominate
    if (target - now > std::chrono::milliseconds(1))
        std::this_thread::sleep_until(target);

    if (now >= x->next_render) {
        Visualizer::SessionInfo info{};
        info.symbol       = x->symbol;
        info.timestamp_ns = msg.timestamp_ns;
        info.messages     = x->msgs;
        info.trades       = x->engine->matches_made();
        info.shares       = x->engine->shares_traded();
        info.last_price   = x->engine->last_price();
        info.speed        = x->speed;
        x->viz->render(*x->engine, info);
        x->next_render = now + std::chrono::milliseconds(40);
    }
}
}// namespace

static void demo_visualizer() {
    std::printf("\n[ 8 ] Live Order Book - Real Market Data\n");
    divider();

    const char*  path   = "../itch_data";
    const char*  symbol = "AAPL";
    // above ~325x the file scan becomes the limit, not the clock
    const double speed  = 300.0;

    ItchReplay replay;
    if (!replay.open(path)) {
        std::printf("  itch_data not found - skipping\n");
        return;
    }

    SymbolDirectory dir;
    dir.subscribe(symbol);

    BookState      book(20);
    SoftwareEngine engine;
    Visualizer     viz;

    PacedCtx ctx{};
    ctx.book   = &book;
    ctx.engine = &engine;
    ctx.viz    = &viz;
    ctx.symbol = symbol;
    ctx.speed  = speed;

    ReplayConfig cfg;
    cfg.directory = &dir;

    Visualizer::clear();
    Visualizer::hide_cursor();

    replay.replay_all(on_paced, &ctx, cfg);

    Visualizer::show_cursor();
    std::printf("\n");
}

static void demo_real_data() {
    std::printf("\n[ 7 ] Real NASDAQ Market Data Replay\n");
    divider();

    const char* path   = "../itch_data";
    const char* symbol = "AAPL";

    ItchReplay replay;
    if (!replay.open(path)) {
        std::printf("  itch_data file not found - skipping\n");
        std::printf("  download from: emi.nasdaq.com/ITCH\n");
        return;
    }

    SymbolDirectory dir;
    dir.subscribe(symbol);          // resolved when its 'R' arrives

    BookState      book(20);        // 1M slots, plenty for one symbol
    SoftwareEngine engine;
    ReplayCtx      ctx{};
    ctx.book   = &book;
    ctx.engine = &engine;

    ReplayConfig cfg;
    cfg.directory = &dir;           // max_messages stays 0 = whole file

    std::printf("  File     : 07302019.NASDAQ_ITCH50 (July 30 2019)\n");
    std::printf("  Symbol   : %s\n", symbol);
    std::printf("  Replaying entire trading day...\n");
    std::fflush(stdout);

    auto stats = replay.replay_all(on_message, &ctx, cfg);

    const double elapsed_s = stats.elapsed_ns / 1e9;
    const auto&  bs        = book.stats();

    // ---- deterministic: same on any machine, compiler or run ----
    std::printf("\n  -- feed --\n");
    std::printf("  Messages total   : %llu\n", stats.messages_total);
    std::printf("  Symbols in file  : %u\n",   dir.known_count());
    std::printf("  Filtered out     : %llu\n", stats.messages_filtered);
    std::printf("  Book messages    : %llu\n", stats.messages_parsed);
    std::printf("  Non-book         : %llu\n", stats.parser_ignored);
    std::printf("  Malformed        : %llu\n", stats.parser_malformed);
    std::printf("  Unknown type     : %llu\n", stats.parser_unknown);

    std::printf("\n  -- %s book operations --\n", symbol);
    std::printf("  Adds             : %llu\n", bs.adds);
    std::printf("  Executes         : %llu\n", bs.executes);
    std::printf("  Cancels          : %llu\n", bs.cancels);
    std::printf("  Deletes          : %llu\n", bs.deletes);
    std::printf("  Replaces         : %llu\n", bs.replaces);
    std::printf("  Hidden trades    : %llu\n", bs.trades);
    std::printf("  Peak live orders : %llu\n", bs.peak_live);
    std::printf("  Max probe        : %llu\n", bs.max_probe);

    std::printf("\n  -- at the closing bell (16:00:00) --\n");
    if (ctx.close_taken) {
        std::printf("  Best bid         : $%.4f x %u\n",
                    ctx.close_bid_px / 10000.0, ctx.close_bid_qty);
        std::printf("  Best ask         : $%.4f x %u\n",
                    ctx.close_ask_px / 10000.0, ctx.close_ask_qty);
        std::printf("  Spread           : $%.4f\n",
                    (static_cast<int64_t>(ctx.close_ask_px) -
                     static_cast<int64_t>(ctx.close_bid_px)) / 10000.0);
        std::printf("  Levels           : %zu bid / %zu ask\n",
                    ctx.close_bid_levels, ctx.close_ask_levels);
        std::printf("  Last trade       : $%.4f\n", ctx.close_last / 10000.0);
        std::printf("  Shares traded    : %llu\n", ctx.close_shares);
        std::printf("  Book digest      : 0x%016llx\n",
                    static_cast<unsigned long long>(ctx.close_digest));
    } else {
        std::printf("  (file ended before 16:00)\n");
    }

    std::printf("\n  -- full session --\n");
    std::printf("  Last trade       : $%.4f\n", engine.last_price() / 10000.0);
    std::printf("  Shares traded    : %llu\n", engine.shares_traded());
    std::printf("  Unresolved refs  : %llu\n", bs.unresolved);
    std::printf("  Orphan ops       : %llu\n", engine.orphan_ops());
    std::printf("  Op digest        : 0x%016llx\n",
                static_cast<unsigned long long>(book.digest()));

    // ---- this machine only ----
    std::printf("\n  -- timing (this machine, cache state affects result) --\n");
    std::printf("  Bytes read       : %.1f GB\n", stats.bytes_read / 1e9);
    std::printf("  Elapsed          : %.2f s\n", elapsed_s);
    std::printf("  Throughput       : %.1f M msg/s\n",
                stats.messages_total / elapsed_s / 1e6);
    std::printf("  I/O bandwidth    : %.0f MB/s\n",
                stats.bytes_read / elapsed_s / 1e6);
}

// ── Main ───────────────────────────────────────────────────────────────────
int main() {
    std::printf("ExchangeCore\n");
    std::printf("Hardware-Accurate Electronic Exchange Simulator\n");
    std::printf("================================================\n");

    demo_itch_parser();
    demo_latency_comparison();
    demo_risk_engine();
    demo_wal();
    demo_ouch();
    demo_numa();
    demo_real_data();
    demo_visualizer();

    return 0;
}