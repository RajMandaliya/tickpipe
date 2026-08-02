#pragma once
#include <cstdint>
#include <cstdio>
#include "itch_parser.hpp"
#include "pipeline.hpp"

// write-ahead log entry types
enum class WalEntryType : uint8_t {
    OrderSubmitted = 1,
    OrderMatched   = 2,
    OrderRejected  = 3,
};

// fixed-size log entry — no variable length fields
// every entry is exactly 64 bytes so we can seek by index
struct alignas(64) WalEntry {
    uint64_t     sequence;    // monotonically increasing, never reused
    uint64_t     timestamp_ns;
    WalEntryType type;
    uint8_t      pad[7];
    Order        order;       // the order this entry is about
    MatchResult  result;      // populated for OrderMatched entries
};

static_assert(sizeof(WalEntry) == 192, "WalEntry size mismatch");

// append-only write-ahead log
// every order goes through here before hitting the pipeline
// on crash — replay the log to reconstruct exchange state
class WriteAheadLog {
public:
    WriteAheadLog() noexcept;
    ~WriteAheadLog();

    // open log file — creates if not exists, appends if exists
    bool open(const char* path) noexcept;

    // append entry — fsync after write for durability
    bool append(WalEntryType type, const Order& order,
                const MatchResult& result) noexcept;

    // close the log file
    void close() noexcept;

    // replay all entries from log — calls callback for each
    // used for crash recovery
    bool replay(const char* path,
                void (*callback)(const WalEntry&, void* ctx),
                void* ctx) noexcept;

    uint64_t entries_written() const noexcept { return sequence_; }
    bool     is_open()         const noexcept { return file_ != nullptr; }

private:
    FILE*    file_;
    uint64_t sequence_;

    // current time in nanoseconds
    static uint64_t now_ns() noexcept;
};