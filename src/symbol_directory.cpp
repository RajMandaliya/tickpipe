#include "symbol_directory.hpp"
#include <cstring>

SymbolDirectory::SymbolDirectory() noexcept
    : pending_count_(0)
    , known_count_(0)
    , subscribed_count_(0)
    , all_(false)
{
    std::memset(symbols_,    ' ', sizeof(symbols_));
    std::memset(known_,      0,   sizeof(known_));
    std::memset(subscribed_, 0,   sizeof(subscribed_));
    std::memset(pending_,    ' ', sizeof(pending_));
}

void SymbolDirectory::pad8(const char* in, char* out) noexcept {
    std::memset(out, ' ', 8);
    for (int i = 0; i < 8 && in[i]; ++i) out[i] = in[i];
}

// R is 39 bytes: locate[1] stock[11..18], rest is issue metadata we don't need
bool SymbolDirectory::feed(const uint8_t* data, uint32_t len) noexcept {
    if (!data || len != 39 || data[0] != 'R') return false;

    const uint16_t locate =
        static_cast<uint16_t>((static_cast<uint16_t>(data[1]) << 8) | data[2]);

    if (!known_[locate]) ++known_count_;
    std::memcpy(symbols_[locate], data + 11, 8);
    known_[locate] = 1;

    // resolve any subscription waiting on this name
    for (uint32_t i = 0; i < pending_count_; ++i) {
        if (std::memcmp(pending_[i], symbols_[locate], 8) != 0) continue;

        if (!subscribed_[locate]) {
            subscribed_[locate] = 1;
            ++subscribed_count_;
        }
        std::memmove(pending_[i], pending_[pending_count_ - 1], 8);
        --pending_count_;
        break;
    }
    return true;
}

bool SymbolDirectory::subscribe(const char* symbol) noexcept {
    if (!symbol) return false;

    char want[8];
    pad8(symbol, want);

    // already in the directory
    for (uint32_t l = 0; l < MAX_LOCATE; ++l) {
        if (!known_[l] || std::memcmp(symbols_[l], want, 8) != 0) continue;
        if (!subscribed_[l]) {
            subscribed_[l] = 1;
            ++subscribed_count_;
        }
        return true;
    }

    if (pending_count_ >= MAX_PENDING) return false;
    std::memcpy(pending_[pending_count_++], want, 8);
    return true;
}

uint16_t SymbolDirectory::locate_of(const char* symbol) const noexcept {
    if (!symbol) return 0;

    char want[8];
    pad8(symbol, want);

    for (uint32_t l = 0; l < MAX_LOCATE; ++l)
        if (known_[l] && std::memcmp(symbols_[l], want, 8) == 0)
            return static_cast<uint16_t>(l);

    return 0;
}