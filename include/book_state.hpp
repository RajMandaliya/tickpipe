#pragma once
#include <cstdint>
#include <cstddef>
#include "itch_parser.hpp"

// E, C, X, D and U carry only an order reference — no side, price or stock.
// The exchange assumes you remembered the order from its Add. This keeps that
// state and hands the engines fully populated operations.
//
//   bytes -> ItchParser -> BookState -> FpgaPipeline / SoftwareEngine
//
// Open addressing over a fixed table allocated once at construction. Nothing
// allocates during replay, which is also how an FPGA book indexes live orders.

struct RestingOrder {
    uint64_t order_ref;
    uint32_t quantity;
    uint32_t price;
    Side     side;
    char     stock[8];
};

struct ResolvedOp {
    Action   action;
    uint64_t order_ref;
    uint32_t quantity;       // shares this op touches
    uint32_t price;
    uint64_t match_number;
    uint64_t timestamp_ns;
    Side     side;
    char     stock[8];
};

// U becomes two ops: remove the old ref, insert the new one. Everything else
// produces at most one.
struct ResolveResult {
    ResolvedOp ops[2];
    uint8_t    count;
};

class BookState {
public:
    struct Stats {
        uint64_t adds;
        uint64_t executes;
        uint64_t cancels;
        uint64_t deletes;
        uint64_t replaces;
        uint64_t trades;
        uint64_t unresolved;
        uint64_t table_full;
        uint64_t live;
        uint64_t peak_live;
        uint64_t max_probe;
    };

    // slots = 1 << capacity_pow2. 22 gives 4.19M slots / ~134 MB. NASDAQ peaks
    // in the low millions of live orders; linear probing degrades past ~0.7.
    explicit BookState(uint32_t capacity_pow2 = 22);
    ~BookState();

    BookState(const BookState&)            = delete;
    BookState& operator=(const BookState&) = delete;

    // false means nothing actionable. usually an unresolved ref, which is
    // normal early in a replay — those orders were added before the file starts.
    bool resolve(const Order& msg, ResolveResult& out) noexcept;

    void reset() noexcept;

    // FNV-1a over every resolved op. same file -> same value on any machine,
    // compiler or run. this is the conformance check.
    uint64_t digest() const noexcept { return digest_; }

    const Stats& stats() const noexcept { return stats_; }
    uint64_t     capacity() const noexcept { return capacity_; }
    double       load_factor() const noexcept {
        return static_cast<double>(stats_.live) / static_cast<double>(capacity_);
    }

private:
    RestingOrder* slots_;
    uint8_t*      state_;
    uint64_t      capacity_;
    uint64_t      mask_;
    Stats         stats_{};
    uint64_t      digest_ = 0xcbf29ce484222325ULL;

    static constexpr uint8_t EMPTY     = 0;
    static constexpr uint8_t OCCUPIED  = 1;
    static constexpr uint8_t TOMBSTONE = 2;

    // order refs are dense sequential ints and collide badly unmixed
    static uint64_t hash_ref(uint64_t ref) noexcept {
        ref ^= ref >> 33;
        ref *= 0xff51afd7ed558ccdULL;
        ref ^= ref >> 33;
        ref *= 0xc4ceb9fe1a85ec53ULL;
        ref ^= ref >> 33;
        return ref;
    }

    void fold(uint64_t v) noexcept {
        for (int i = 0; i < 8; ++i) {
            digest_ ^= (v >> (i * 8)) & 0xFF;
            digest_ *= 0x100000001b3ULL;
        }
    }
    void fold_op(const ResolvedOp& op) noexcept;
    bool resolve_impl(const Order& msg, ResolveResult& out) noexcept;

    RestingOrder* find(uint64_t ref) noexcept;
    bool          insert(const RestingOrder& ro) noexcept;
    bool          erase(uint64_t ref) noexcept;
};