#include <iostream>
#include <cstring>
#include <chrono>
#include "udp_receiver.hpp"
#include "book_state.hpp"
#include "software_engine.hpp"
#include "risk_engine.hpp"
#include "wal.hpp"
#include "visualizer.hpp"
#include "numa.hpp"
#include "symbol_directory.hpp"

// The consumer side of the feed. It does not care whether bytes arrive from a
// file replay, a UDP replay server, or a live exchange socket — only that they
// are ITCH. Swapping the source is a change to main(), not to this pipeline.

using LiveClock = std::chrono::steady_clock;

struct ExchangeState {
    BookState*      book;
    SoftwareEngine* engine;
    RiskEngine*     risk;
    WriteAheadLog*  wal;
    Visualizer*     viz;

    uint64_t        messages;
    uint64_t        rejected;
    uint64_t        last_ts;
    LiveClock::time_point next_render;
};

static void on_order(const Order& msg, void* ctx) {
    auto* s = static_cast<ExchangeState*>(ctx);
    ++s->messages;
    s->last_ts = msg.timestamp_ns;

    ResolveResult r;
    if (s->book->resolve(msg, r)) {
        for (uint8_t i = 0; i < r.count; ++i) {
            const ResolvedOp& op = r.ops[i];
            MatchResult empty{};

            if (op.action == Action::Add) {
                Order probe{};
                probe.order_ref    = op.order_ref;
                probe.price        = op.price;
                probe.quantity     = op.quantity;
                probe.side         = op.side;
                probe.timestamp_ns = op.timestamp_ns;
                probe.valid        = 1;
                std::memcpy(probe.stock, op.stock, 8);

                if (s->risk->check(probe) != RiskResult::Accepted) {
                    s->wal->append(WalEntryType::OrderRejected, probe, empty);
                    ++s->rejected;
                    continue;
                }
                s->wal->append(WalEntryType::OrderSubmitted, probe, empty);
            }
            s->engine->apply(op);
        }
    }

    // render regardless — a blank screen tells you nothing
    const auto now = LiveClock::now();
    if (now >= s->next_render) {
        Visualizer::SessionInfo info{};
        info.symbol       = "LIVE";
        info.timestamp_ns = s->last_ts;
        info.messages     = s->messages;
        info.trades       = s->engine->matches_made();
        info.shares       = s->engine->shares_traded();
        info.last_price   = s->engine->last_price();
        info.speed        = 0.0;
        s->viz->render(*s->engine, info);
        s->next_render = now + std::chrono::milliseconds(100);
    }
}

int main() {
    ThreadPinner::pin_to_matching_core();

    std::printf("ExchangeCore Live Exchange\n");
    std::printf("==========================\n");
    std::printf("Waiting for replay server on port %d...\n\n",
                UdpReceiver::DEFAULT_PORT);

    BookState      book(22);
    SoftwareEngine engine;
    RiskEngine     risk;
    WriteAheadLog  wal;
    Visualizer     viz;

    wal.open("live_exchange.wal");

    // set reference prices for common stocks
    auto set_ref = [&](const char* ticker, uint32_t price) {
        char stock[8] = {' ',' ',' ',' ',' ',' ',' ',' '};
        for (int i = 0; i < 8 && ticker[i]; ++i) stock[i] = ticker[i];
        risk.set_reference_price(stock, price);
    };

    set_ref("AAPL", 2100000);   // $210 — July 30 2019
    set_ref("MSFT", 1400000);
    set_ref("AMZN", 18000000);
    set_ref("GOOG", 12000000);
    set_ref("META", 1900000);

    ExchangeState state{};
    state.book        = &book;
    state.engine      = &engine;
    state.risk        = &risk;
    state.wal         = &wal;
    state.viz         = &viz;
    state.next_render = LiveClock::now();

    Visualizer::clear();
    Visualizer::hide_cursor();

    UdpReceiver receiver;
    if (!receiver.init(UdpReceiver::DEFAULT_PORT)) {
        Visualizer::show_cursor();
        std::printf("error: failed to bind UDP port %d\n",
                    UdpReceiver::DEFAULT_PORT);
        return 1;
    }

    const auto t_start = LiveClock::now();

    // blocking — returns when replay server finishes

    SymbolDirectory dir;
    dir.subscribe("AAPL");

    ReceiverConfig rcfg;
    rcfg.directory = &dir;
    rcfg.idle_timeout_ms = 30000;

    receiver.receive_loop(on_order, &state, rcfg);

    std::printf("\n  Symbols known    : %u\n", dir.known_count());
    std::printf("  Subscribed       : %u\n", dir.subscribed_count());
    std::printf("  Unmatched names  : %u\n", dir.unmatched_count());

    const double elapsed =
        std::chrono::duration<double>(LiveClock::now() - t_start).count();

    Visualizer::show_cursor();
    wal.close();

    const auto& bs = book.stats();
    std::printf("\n\nLive Exchange Stats\n");
    std::printf("===================\n");
    std::printf("  Packets received : %llu\n", receiver.packets_received());
    std::printf("  Orders parsed    : %llu\n", receiver.orders_parsed());
    std::printf("  Book messages    : %llu\n", state.messages);
    std::printf("  Adds             : %llu\n", bs.adds);
    std::printf("  Executes         : %llu\n", bs.executes);
    std::printf("  Cancels          : %llu\n", bs.cancels);
    std::printf("  Deletes          : %llu\n", bs.deletes);
    std::printf("  Replaces         : %llu\n", bs.replaces);
    std::printf("  Filtered out     : %llu\n", receiver.messages_filtered());
    std::printf("  Directory msgs   : %llu\n", receiver.directory_messages());
    std::printf("  Short packets    : %llu\n", receiver.short_packets());
    std::printf("  Unresolved refs  : %llu\n", bs.unresolved);
    std::printf("  Peak live orders : %llu\n", bs.peak_live);
    std::printf("  Orphan ops       : %llu\n", engine.orphan_ops());
    std::printf("  Risk rejected    : %llu\n", state.rejected);
    std::printf("  Shares traded    : %llu\n", engine.shares_traded());
    std::printf("  WAL entries      : %llu\n", wal.entries_written());
    std::printf("  Gaps detected    : %llu\n", receiver.gaps());
    std::printf("  Messages lost    : %llu\n", receiver.messages_lost());
    std::printf("  Out of order     : %llu\n", receiver.out_of_order());
    std::printf("  Ring drops       : %llu\n", receiver.ring_dropped());
    std::printf("  Ring high water  : %.1f MB\n",
                receiver.ring_high_water() / 1048576.0);
    std::printf("  Elapsed          : %.2f sec\n", elapsed);
    const double p = receiver.process_ns() / 1e9;
    const double i = receiver.idle_ns() / 1e9;
    std::printf("  Book processing  : %.1f s (%.0f%%)\n", p, 100*p/(p+i));
    std::printf("  Book idle        : %.1f s (%.0f%%)\n", i, 100*i/(p+i));
    if (elapsed > 0.0)
        std::printf("  Throughput       : %.2fM msg/sec\n",
                    state.messages / elapsed / 1e6);

    std::remove("live_exchange.wal");
    return 0;
}
