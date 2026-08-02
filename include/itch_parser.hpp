#pragma once
#include <cstdint>
#include <cstddef>

// NASDAQ TotalView-ITCH 5.0.
// BinaryFILE prefixes each body with a 2-byte BE length; ItchReplay strips it,
// so data[0] here is the type byte and len is the body length.
//
// common header:
//   [0]     type
//   [1..2]  stock locate
//   [3..4]  tracking number   (exchange internal, unused)
//   [5..10] timestamp, ns since midnight

enum class Side : uint8_t {
    None = 0,
    Buy  = 'B',
    Sell = 'S',
};

enum class Action : uint8_t {
    None = 0,
    Add,      // A, F
    Execute,  // E, C
    Cancel,   // X
    Delete,   // D
    Replace,  // U
    Trade,    // P
};

struct alignas(64) Order {
    uint64_t timestamp_ns;
    uint64_t order_ref;       // U: the original ref
    uint64_t new_order_ref;   // U only
    uint64_t match_number;    // E, C, P
    uint32_t quantity;
    uint32_t price;           // x10000
    uint16_t stock_locate;
    Side     side;
    Action   action;
    uint8_t  valid;           // pipeline bubble flag
    char     stock[8];        // A, F, P only
};

class ItchParser {
public:
    struct Counts {
        uint64_t parsed;
        uint64_t ignored;     // known type, not a book message
        uint64_t malformed;   // known type, wrong length
        uint64_t unknown;
    };

    bool parse(const uint8_t* data, uint32_t len, Order& out) noexcept;

    static uint16_t expected_length(uint8_t type) noexcept;   // 0 = unknown
    static bool     is_book_message(uint8_t type) noexcept;

    const Counts& counts() const noexcept { return counts_; }
    void reset_counts() noexcept { counts_ = Counts{}; }

private:
    Counts counts_{};

    static uint16_t be16(const uint8_t* p) noexcept {
        return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
    }
    static uint32_t be32(const uint8_t* p) noexcept {
        return (static_cast<uint32_t>(p[0]) << 24)
             | (static_cast<uint32_t>(p[1]) << 16)
             | (static_cast<uint32_t>(p[2]) <<  8)
             |  static_cast<uint32_t>(p[3]);
    }
    // 6-byte timestamp. a full day is ~2.3e13 ns, so this cannot be 32-bit.
    static uint64_t be48(const uint8_t* p) noexcept {
        uint64_t v = 0;
        for (int i = 0; i < 6; ++i) v = (v << 8) | p[i];
        return v;
    }
    static uint64_t be64(const uint8_t* p) noexcept {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
        return v;
    }
};