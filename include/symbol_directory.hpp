#pragma once
#include <cstdint>
#include <cstddef>

// Stock Directory ('R') messages at the head of the file map stock_locate ->
// ticker for that trading day. The locate is only valid for that one day.
//
// The parser drops 'R' as reference data, so this consumes the raw bytes
// itself and answers the one question the replay loop asks per message:
// "am I subscribed to this locate?"
//
// locate is a dense uint16, so everything is a flat array indexed by it —
// no hashing, no allocation, single load on the hot path.

class SymbolDirectory {
public:
    static constexpr uint32_t MAX_LOCATE  = 65536;
    static constexpr uint32_t MAX_PENDING = 64;

    SymbolDirectory() noexcept;

    // returns true if this was an 'R' and got consumed
    bool feed(const uint8_t* data, uint32_t len) noexcept;

    // safe to call before the directory is read — unmatched names sit in
    // pending until their 'R' shows up
    bool subscribe(const char* symbol) noexcept;
    void subscribe_all() noexcept { all_ = true; }

    bool is_subscribed(uint16_t locate) const noexcept {
        return all_ || subscribed_[locate];
    }

    // 8 chars, space padded, NOT null terminated
    const char* symbol_of(uint16_t locate) const noexcept {
        return known_[locate] ? symbols_[locate] : nullptr;
    }
    uint16_t locate_of(const char* symbol) const noexcept;

    uint32_t known_count()      const noexcept { return known_count_; }
    uint32_t subscribed_count() const noexcept { return subscribed_count_; }
    uint32_t unmatched_count()  const noexcept { return pending_count_; }

    // names that never appeared in the directory — wrong ticker, or not
    // NASDAQ-listed
    const char* unmatched(uint32_t i) const noexcept {
        return (i < pending_count_) ? pending_[i] : nullptr;
    }

private:
    char    symbols_[MAX_LOCATE][8];
    uint8_t known_[MAX_LOCATE];
    uint8_t subscribed_[MAX_LOCATE];

    char     pending_[MAX_PENDING][8];
    uint32_t pending_count_;

    uint32_t known_count_;
    uint32_t subscribed_count_;
    bool     all_;

    // pad to 8 with spaces, the way ITCH stores them
    static void pad8(const char* in, char* out) noexcept;
};