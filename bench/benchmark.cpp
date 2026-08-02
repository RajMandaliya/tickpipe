#include <iostream>
#include <cstdio>
#include <chrono>
#include <vector>
#include <algorithm>
#include <cstring>
#include "itch_parser.hpp"
#include "pipeline.hpp"
#include "software_engine.hpp"

using Clock = std::chrono::steady_clock;

static uint64_t now_ns() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<nanoseconds>(Clock::now().time_since_epoch()).count());
}

// Timing a single sub-100ns operation on Windows measures the clock, not the
// code — steady_clock ticks at 100ns there. So we time a batch and divide, and
// treat each batch mean as one sample. Percentiles are over batch means, which
// hides per-op outliers but produces numbers that are actually measurements.
struct Dist {
    double p50, p90, p99, p999, min, max, mean;
    uint64_t total_ns;
};

static Dist summarize(std::vector<double>& v, uint64_t total_ns) {
    std::sort(v.begin(), v.end());
    auto at = [&](double p) {
        size_t i = static_cast<size_t>(p / 100.0 * v.size());
        if (i >= v.size()) i = v.size() - 1;
        return v[i];
    };
    double sum = 0;
    for (double x : v) sum += x;
    return { at(50), at(90), at(99), at(99.9),
             v.front(), v.back(), sum / v.size(), total_ns };
}

template <class F>
static Dist run_batches(F&& fn, int batch, int nbatches) {
    std::vector<double> per_op;
    per_op.reserve(nbatches);
    uint64_t total = 0;
    for (int b = 0; b < nbatches; ++b) {
        const uint64_t t0 = now_ns();
        for (int i = 0; i < batch; ++i) fn(b * batch + i);
        const uint64_t t1 = now_ns();
        total += (t1 - t0);
        per_op.push_back(static_cast<double>(t1 - t0) / batch);
    }
    return summarize(per_op, total);
}

static void print_dist(const char* name, const Dist& d, int ops) {
    std::printf("  %-18s %7.1f %7.1f %7.1f %7.1f %7.1f %7.1f\n",
                name, d.mean, d.p50, d.p90, d.p99, d.p999, d.max);
    std::printf("  %-18s %.2f M ops/sec over %d ops\n", "",
                ops / (d.total_ns / 1e9) / 1e6, ops);
}

static void header() {
    std::printf("  %-18s %7s %7s %7s %7s %7s %7s\n",
                "", "mean", "p50", "p90", "p99", "p999", "max");
    std::printf("  %-18s %s\n", "",
                "----------------------------------------------");
}

// how small a duration this machine can actually resolve
static void bench_timer_resolution() {
    std::printf("\n=== Timer Resolution ===\n");
    uint64_t min_delta = UINT64_MAX;
    for (int i = 0; i < 200000; ++i) {
        const uint64_t a = now_ns();
        uint64_t b;
        do { b = now_ns(); } while (b == a);
        if (b - a < min_delta) min_delta = b - a;
    }
    std::printf("  steady_clock tick : %llu ns\n",
                static_cast<unsigned long long>(min_delta));
    std::printf("  Anything faster than this cannot be timed one op at a\n");
    std::printf("  time, which is why the numbers below are batch means.\n");
}

// deterministic pseudo-random so prices spread across levels and the run is
// repeatable — a single hot price level is not a book
static uint32_t lcg(uint32_t& s) { s = s * 1664525u + 1013904223u; return s; }

static void bench_itch_parser() {
    std::printf("\n=== ITCH Parser ===\n");

    // a mix of types and lengths, not one message replayed
    constexpr int POOL = 4096;
    static uint8_t  msgs[POOL][44];
    static uint16_t lens[POOL];
    uint32_t seed = 12345;

    for (int i = 0; i < POOL; ++i) {
        uint32_t r = lcg(seed) % 100;
        uint8_t* m = msgs[i];
        std::memset(m, 0, 44);
        uint64_t ref = 1000 + i;
        auto put64 = [&](uint8_t* p, uint64_t v) {
            for (int k = 7; k >= 0; --k) { p[k] = v & 0xFF; v >>= 8; } };
        auto put32 = [&](uint8_t* p, uint32_t v) {
            p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; };
        m[1] = 0; m[2] = 1;
        for (int k = 0; k < 6; ++k) m[5+k] = static_cast<uint8_t>(lcg(seed));

        if (r < 45) {                       // Add
            m[0]='A'; put64(m+11, ref); m[19] = (r&1)?'B':'S';
            put32(m+20, 100 + (lcg(seed)%900));
            std::memcpy(m+24, "AAPL    ", 8);
            put32(m+32, 2000000 + (lcg(seed)%200000));
            lens[i] = 36;
        } else if (r < 65) {                // Delete
            m[0]='D'; put64(m+11, ref); lens[i] = 19;
        } else if (r < 80) {                // Executed
            m[0]='E'; put64(m+11, ref); put32(m+19, 100);
            put64(m+23, i); lens[i] = 31;
        } else if (r < 90) {                // Replace
            m[0]='U'; put64(m+11, ref); put64(m+19, ref+POOL);
            put32(m+27, 200); put32(m+31, 2050000); lens[i] = 35;
        } else {                            // Cancel
            m[0]='X'; put64(m+11, ref); put32(m+19, 50); lens[i] = 23;
        }
    }

    ItchParser parser;
    Order      order{};
    auto step = [&](int i) {
        const int k = i & (POOL - 1);
        parser.parse(msgs[k], lens[k], order);
    };

    for (int i = 0; i < 100000; ++i) step(i);          // warm
    parser.reset_counts();

    constexpr int BATCH = 1000, NB = 2000;
    Dist d = run_batches(step, BATCH, NB);
    header();
    print_dist("parse (ns/msg)", d, BATCH * NB);
}

static Order make_order(uint32_t& seed, uint64_t id) {
    Order o{};
    o.order_ref = id;
    // spread across ~200 price levels so the book has real shape
    o.price     = 2000000 + (lcg(seed) % 200) * 100;
    o.quantity  = 100;
    o.side      = (id & 1) ? Side::Buy : Side::Sell;
    o.valid     = 1;
    return o;
}

static void bench_engines() {
    std::printf("\n=== Matching Engines ===\n");
    constexpr int BATCH = 1000, NB = 1000;

    {
        FpgaPipeline pipe;
        uint32_t seed = 999;
        auto step = [&](int i) { pipe.clock(make_order(seed, i)); };
        for (int i = 0; i < 10000; ++i) step(i);
        Dist d = run_batches(step, BATCH, NB);
        header();
        print_dist("FPGA pipeline", d, BATCH * NB);
    }
    {
        SoftwareEngine eng;
        uint32_t seed = 999;
        auto step = [&](int i) { eng.submit(make_order(seed, i)); };
        for (int i = 0; i < 10000; ++i) step(i);
        Dist d = run_batches(step, BATCH, NB);
        print_dist("Software engine", d, BATCH * NB);
    }
}

int main() {
    std::printf("ExchangeCore Benchmark Suite\n");
    std::printf("============================\n");
    std::printf("All figures are per-operation cost in nanoseconds,\n");
    std::printf("measured as batch means. Machine dependent.\n");

    bench_timer_resolution();
    bench_itch_parser();
    bench_engines();

    std::printf("\nDone.\n");
    return 0;
}