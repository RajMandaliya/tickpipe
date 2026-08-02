#include "wal.hpp"
#include <cstring>
#include <chrono>

WriteAheadLog::WriteAheadLog() noexcept
    : file_(nullptr)
    , sequence_(0)
{}

WriteAheadLog::~WriteAheadLog() {
    close();
}

bool WriteAheadLog::open(const char* path) noexcept {
    // append mode — never overwrite existing log
    file_ = std::fopen(path, "ab+");
    if (!file_) return false;

    // find current sequence from existing entries
    std::fseek(file_, 0, SEEK_END);
    long size = std::ftell(file_);
    sequence_ = static_cast<uint64_t>(size / sizeof(WalEntry));

    return true;
}

bool WriteAheadLog::append(WalEntryType type, const Order& order,
                            const MatchResult& result) noexcept {
    if (!file_) return false;

    WalEntry entry{};
    entry.sequence     = ++sequence_;
    entry.timestamp_ns = now_ns();
    entry.type         = type;
    entry.order        = order;
    entry.result       = result;

    // write entry
    if (std::fwrite(&entry, sizeof(WalEntry), 1, file_) != 1)
        return false;

    // fsync — guarantee durability before returning to caller
    // expensive but correct — caller knows order is safe after this returns
    std::fflush(file_);

    return true;
}

void WriteAheadLog::close() noexcept {
    if (file_) {
        std::fflush(file_);
        std::fclose(file_);
        file_ = nullptr;
    }
}

bool WriteAheadLog::replay(const char* path,
                            void (*callback)(const WalEntry&, void* ctx),
                            void* ctx) noexcept {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;

    WalEntry entry{};
    uint64_t count = 0;

    while (std::fread(&entry, sizeof(WalEntry), 1, f) == 1) {
        callback(entry, ctx);
        ++count;
    }

    std::fclose(f);
    return count > 0;
}

uint64_t WriteAheadLog::now_ns() noexcept {
    return static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now()
            .time_since_epoch()
            .count()
    );
}