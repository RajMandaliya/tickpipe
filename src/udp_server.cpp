#include "udp_server.hpp"
#include <cstring>
#include <cstdio>
#include <chrono>
#include <thread>

#ifdef _WIN32
static void close_sock(SOCKET s) { closesocket(s); }
static const SOCKET INVALID = INVALID_SOCKET;
#else
static void close_sock(int s) { ::close(s); }
static const int INVALID = -1;
#endif

uint64_t UdpServer::now_ns() noexcept {
    return static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now()
            .time_since_epoch()
            .count()
    );
}

UdpServer::UdpServer() noexcept
    : sock_(INVALID)
    , packets_sent_(0)
    , bytes_sent_(0)
{
    std::memset(&target_, 0, sizeof(target_));

#ifdef _WIN32
    // winsock requires explicit init
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

UdpServer::~UdpServer() { close(); }

bool UdpServer::init(const char* host, int port) noexcept {
    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == INVALID) return false;

    target_.sin_family = AF_INET;
    target_.sin_port   = htons(static_cast<uint16_t>(port));

#ifdef _WIN32
    InetPtonA(AF_INET, host, &target_.sin_addr);
#else
    inet_pton(AF_INET, host, &target_.sin_addr);
#endif

    return true;
}

bool UdpServer::send_message(const uint8_t* data, uint16_t len) noexcept {
    // prepend 2-byte big-endian length — same framing as ITCH file format
    uint8_t buf[1026];
    buf[0] = (len >> 8) & 0xFF;
    buf[1] =  len       & 0xFF;
    std::memcpy(buf + 2, data, len);

#ifdef _WIN32
    int sent = sendto(sock_,
        reinterpret_cast<const char*>(buf), len + 2, 0,
        reinterpret_cast<sockaddr*>(&target_), sizeof(target_));
#else
    ssize_t sent = sendto(sock_, buf, len + 2, 0,
        reinterpret_cast<sockaddr*>(&target_), sizeof(target_));
#endif

    if (sent <= 0) return false;

    ++packets_sent_;
    bytes_sent_ += len + 2;
    return true;
}

void UdpServer::replay_file(const char* path,
                             double speed_multiplier) noexcept {
    // memory map the file for fast sequential reads
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::printf("replay_server: cannot open %s\n", path);
        return;
    }

    std::printf("replay_server: starting replay of %s\n", path);
    std::printf("replay_server: speed=%.1fx  port=%d\n",
                speed_multiplier == 0 ? 999.0 : speed_multiplier,
                DEFAULT_PORT);

    uint8_t  len_buf[2];
    uint8_t  msg_buf[MAX_PACKET_SIZE];
    uint64_t msg_count   = 0;
    uint64_t t_start     = now_ns();
    uint64_t first_ts    = 0;  // first message timestamp from file
    bool     ts_set      = false;

    while (std::fread(len_buf, 1, 2, f) == 2) {
        uint16_t msg_len = read_be16(len_buf);
        if (msg_len == 0 || msg_len > MAX_PACKET_SIZE) break;
        if (std::fread(msg_buf, 1, msg_len, f) != msg_len) break;

        // rate control — pace packets to simulate realtime feed
        // skip if unlimited speed (speed_multiplier == 0)
        if (speed_multiplier > 0 && msg_len >= 11) {
            // extract 6-byte timestamp from ITCH message (bytes 5-10)
            uint64_t msg_ts = 0;
            for (int i = 5; i < 11 && i < msg_len; ++i)
                msg_ts = (msg_ts << 8) | msg_buf[i];

            if (!ts_set) {
                first_ts = msg_ts;
                ts_set   = true;
            }

            // how far ahead we are in the file vs wall clock
            uint64_t file_elapsed  = msg_ts - first_ts;
            uint64_t wall_elapsed  = static_cast<uint64_t>(
                (now_ns() - t_start) * speed_multiplier);

            // sleep if we're sending faster than the speed target
            if (file_elapsed > wall_elapsed) {
                uint64_t sleep_ns = (file_elapsed - wall_elapsed)
                                    / static_cast<uint64_t>(speed_multiplier);
                if (sleep_ns > 1000) {
                    std::this_thread::sleep_for(
                        std::chrono::nanoseconds(sleep_ns));
                }
            }
        }

        send_message(msg_buf, msg_len);
        ++msg_count;

        // progress every 10M messages
        if (msg_count % 10000000 == 0) {
            double elapsed = (now_ns() - t_start) / 1e9;
            std::printf("  sent %lluM messages in %.1fs\n",
                        msg_count / 1000000, elapsed);
        }
    }

    double elapsed = (now_ns() - t_start) / 1e9;
    std::printf("replay_server: done. sent=%llu  time=%.2fs  "
                "throughput=%.0fM/sec\n",
                msg_count, elapsed, msg_count / elapsed / 1e6);

    std::fclose(f);
}

void UdpServer::close() noexcept {
    if (sock_ != INVALID) {
        close_sock(sock_);
        sock_ = INVALID;
    }
#ifdef _WIN32
    WSACleanup();
#endif
}