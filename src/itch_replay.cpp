#include "itch_replay.hpp"
#include <chrono>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

static uint64_t now_ns() noexcept {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<nanoseconds>(
            steady_clock::now().time_since_epoch()).count());
}

ItchReplay::ItchReplay() noexcept
    : mapped_(nullptr)
    , mapped_size_(0)
    , file_(nullptr)
{}

ItchReplay::~ItchReplay() { close(); }

bool ItchReplay::open(const char* path) noexcept {
#ifdef _WIN32
    // Windows memory mapping
    HANDLE hfile = CreateFileA(
        path, GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);

    if (hfile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(hfile, &size)) {
        CloseHandle(hfile);
        return false;
    }

    mapped_size_ = static_cast<uint64_t>(size.QuadPart);

    HANDLE hmap = CreateFileMappingA(
        hfile, nullptr, PAGE_READONLY, 0, 0, nullptr);

    CloseHandle(hfile); // mapping keeps file open internally

    if (!hmap) return false;

    mapped_ = MapViewOfFile(hmap, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(hmap);

    return mapped_ != nullptr;
#else
    int fd = ::open(path, O_RDONLY);
    if (fd < 0) return false;

    struct stat st{};
    if (::fstat(fd, &st) < 0) { ::close(fd); return false; }

    mapped_size_ = static_cast<uint64_t>(st.st_size);
    mapped_ = ::mmap(nullptr, mapped_size_, PROT_READ,
                     MAP_PRIVATE, fd, 0);
    ::close(fd);

    if (mapped_ == MAP_FAILED) { mapped_ = nullptr; return false; }

    // hint to OS — sequential access pattern
    ::madvise(mapped_, mapped_size_, MADV_SEQUENTIAL);
    return true;
#endif
}

void ItchReplay::close() noexcept {
    if (mapped_) {
#ifdef _WIN32
        UnmapViewOfFile(mapped_);
#else
        ::munmap(mapped_, mapped_size_);
#endif
        mapped_      = nullptr;
        mapped_size_ = 0;
    }
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

ItchReplay::Stats ItchReplay::replay_all(
    void (*callback)(const Order&, void* ctx), void* ctx,
    const ReplayConfig& cfg) noexcept {

    Stats stats{};
    if (!mapped_) return stats;

    const uint8_t* p   = static_cast<const uint8_t*>(mapped_);
    const uint8_t* end = p + mapped_size_;

    ItchParser parser;
    Order      order{};

    uint64_t t0 = now_ns();

    while (p + 2 <= end) {
        uint16_t msg_len = read_be16(p);
        p += 2;

        if (msg_len == 0 || p + msg_len > end) break;

        ++stats.messages_total;
        stats.bytes_read += 2 + msg_len;

        if (cfg.directory) {
            // directory first — we need the locate map before we can filter
            if (cfg.directory->feed(p, msg_len)) {
                ++stats.directory_messages;
                p += msg_len;
                continue;
            }
            // drop unsubscribed symbols before paying for a parse
            if (msg_len >= 3 &&
                !cfg.directory->is_subscribed(read_be16(p + 1))) {
                ++stats.messages_filtered;
                p += msg_len;
                continue;
            }
        }

        if (parser.parse(p, msg_len, order)) {
            ++stats.messages_parsed;
            if (callback) callback(order, ctx);
        } else {
            ++stats.messages_skipped;
        }

        p += msg_len;

        if (cfg.max_messages && stats.messages_total >= cfg.max_messages) break;
    }

    stats.elapsed_ns       = now_ns() - t0;
    stats.parser_ignored   = parser.counts().ignored;
    stats.parser_malformed = parser.counts().malformed;
    stats.parser_unknown   = parser.counts().unknown;
    return stats;
}
