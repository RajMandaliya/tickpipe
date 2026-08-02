#pragma once
#include <cstdint>
#include <cstdio>
#include "itch_parser.hpp"
#include "symbol_directory.hpp"

struct ReplayConfig {
    // null = take everything. otherwise 'R' messages are fed to it and
    // anything not subscribed is dropped before the parser sees it.
    SymbolDirectory* directory = nullptr;
    uint64_t max_messages = 0;      // 0 = whole file. debugging only.
};

class ItchReplay {
public:
    struct Stats {
        uint64_t messages_total;
        uint64_t messages_parsed;
        uint64_t messages_skipped;
        uint64_t messages_filtered;    // wrong symbol
        uint64_t directory_messages;   // 'R'
        uint64_t bytes_read;
        uint64_t elapsed_ns;

        // straight off the parser
        uint64_t parser_ignored;
        uint64_t parser_malformed;
        uint64_t parser_unknown;
    };

    ItchReplay() noexcept;
    ~ItchReplay();

    bool open(const char* path) noexcept;
    void close() noexcept;

    // replay entire file using memory map for maximum throughput
    Stats replay_all(void (*callback)(const Order&, void* ctx),
                     void* ctx,
                     const ReplayConfig& cfg = ReplayConfig{}) noexcept;

    bool     is_open()   const noexcept { return mapped_ != nullptr; }
    uint64_t file_size() const noexcept { return mapped_size_; }

private:
    // memory mapped file
    void*    mapped_;
    uint64_t mapped_size_;

    // fallback file handle if mmap fails
    FILE*    file_;

    static uint16_t read_be16(const uint8_t* p) noexcept {
        return static_cast<uint16_t>(p[0] << 8 | p[1]);
    }
};
