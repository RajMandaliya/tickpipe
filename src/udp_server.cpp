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

constexpr char UdpServer::SESSION[11];

uint64_t UdpServer::now_ns() noexcept {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<nanoseconds>(
            steady_clock::now().time_since_epoch()).count());
}

UdpServer::UdpServer() noexcept
    : sock_(INVALID)
    , payload_len_(0)
    , packet_msgs_(0)
    , next_seq_(1)          // MoldUDP64 sequences start at 1
    , packets_sent_(0)
    , messages_sent_(0)
    , bytes_sent_(0)
{
    std::memset(&target_, 0, sizeof(target_));
    std::memset(packet_, 0, sizeof(packet_));

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

    // a big send buffer stops the kernel dropping our own packets on the way out
    int sndbuf = 4 * 1024 * 1024;
#ifdef _WIN32
    setsockopt(sock_, SOL_SOCKET, SO_SNDBUF,
               reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf));
#else
    setsockopt(sock_, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
#endif

    target_.sin_family = AF_INET;
    target_.sin_port   = htons(static_cast<uint16_t>(port));

#ifdef _WIN32
    InetPtonA(AF_INET, host, &target_.sin_addr);
#else
    inet_pton(AF_INET, host, &target_.sin_addr);
#endif

    return true;
}

bool UdpServer::flush() noexcept {
    if (packet_msgs_ == 0) return true;

    uint8_t out[MOLD_HEADER + MAX_PAYLOAD];
    std::memcpy(out, SESSION, 10);
    write_be64(out + 10, next_seq_ - packet_msgs_);   // seq of first message
    write_be16(out + 18, packet_msgs_);
    std::memcpy(out + MOLD_HEADER, packet_, static_cast<size_t>(payload_len_));

    const int total = MOLD_HEADER + payload_len_;

#ifdef _WIN32
    int sent = sendto(sock_, reinterpret_cast<const char*>(out), total, 0,
                      reinterpret_cast<sockaddr*>(&target_), sizeof(target_));
#else
    ssize_t sent = sendto(sock_, out, static_cast<size_t>(total), 0,
                          reinterpret_cast<sockaddr*>(&target_), sizeof(target_));
#endif

    payload_len_ = 0;
    packet_msgs_ = 0;

    if (sent <= 0) return false;

    ++packets_sent_;
    bytes_sent_ += static_cast<uint64_t>(total);
    return true;
}

bool UdpServer::send_message(const uint8_t* data, uint16_t len) noexcept {
    const int need = 2 + len;
    if (need > MAX_PAYLOAD) return false;          // single message too large

    // no room left in this datagram, or the count field would wrap
    if (payload_len_ + need > MAX_PAYLOAD || packet_msgs_ == 0xFFFF)
        if (!flush()) return false;

    write_be16(packet_ + payload_len_, len);
    std::memcpy(packet_ + payload_len_ + 2, data, len);
    payload_len_ += need;
    ++packet_msgs_;
    ++next_seq_;
    ++messages_sent_;
    return true;
}

void UdpServer::replay_file(const char* path,
                             double speed_multiplier) noexcept {
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::printf("replay_server: cannot open %s\n", path);
        return;
    }

    std::printf("replay_server: starting replay of %s\n", path);
    std::printf("replay_server: speed=%s  port=%d  session=%s\n",
                speed_multiplier == 0 ? "unlimited" : "paced",
                DEFAULT_PORT, SESSION);

    uint8_t  len_buf[2];
    uint8_t  msg_buf[MAX_MSG_SIZE];
    uint64_t msg_count = 0;
    uint64_t t_start   = now_ns();
    uint64_t last_report = t_start;
    uint64_t first_ts  = 0;
    bool     ts_set    = false;

    while (std::fread(len_buf, 1, 2, f) == 2) {
        uint16_t msg_len = read_be16(len_buf);
        if (msg_len == 0 || msg_len > MAX_MSG_SIZE) break;
        if (std::fread(msg_buf, 1, msg_len, f) != msg_len) break;

        // rate control — pace packets to simulate a realtime feed
        if (speed_multiplier > 0 && msg_len >= 11) {
            uint64_t msg_ts = 0;
            for (int i = 5; i < 11 && i < msg_len; ++i)
                msg_ts = (msg_ts << 8) | msg_buf[i];

            if (!ts_set) { first_ts = msg_ts; ts_set = true; }

            const uint64_t file_elapsed = msg_ts - first_ts;
            const uint64_t wall_elapsed = static_cast<uint64_t>(
                (now_ns() - t_start) * speed_multiplier);

            if (file_elapsed > wall_elapsed) {
                const uint64_t sleep_ns = (file_elapsed - wall_elapsed)
                    / static_cast<uint64_t>(speed_multiplier);
                if (sleep_ns > 200000) {         // only worth a syscall above 0.2ms
                    // flushing on every micro-sleep shreds batching, and the
                    // receiver pays per packet — only flush if the buffered
                    // messages would otherwise sit for a noticeable time
                    if (sleep_ns > 2000000) flush();      // 2ms
                    std::this_thread::sleep_for(
                        std::chrono::nanoseconds(sleep_ns));
                }
            }
        }

        send_message(msg_buf, msg_len);
        ++msg_count;

        // progress on a timer, not a message count — a 10M interval is silent
        // for well over a minute
        const uint64_t now = now_ns();
        if (now - last_report > 15000000000ULL) {
            last_report = now;
            const double el = (now - t_start) / 1e9;
            std::printf("  %.1fM msgs · %llu packets · %.1f MB · %.0fs · %.0fK msg/s\n",
                        msg_count / 1e6,
                        static_cast<unsigned long long>(packets_sent_),
                        bytes_sent_ / 1e6, el, msg_count / el / 1e3);
            std::fflush(stdout);
        }
    }

    flush();

    const double elapsed = (now_ns() - t_start) / 1e9;
    std::printf("replay_server: done. messages=%llu packets=%llu "
                "time=%.1fs  %.0fK msg/s  avg %.1f msgs/packet\n",
                static_cast<unsigned long long>(msg_count),
                static_cast<unsigned long long>(packets_sent_),
                elapsed, msg_count / elapsed / 1e3,
                packets_sent_ ? static_cast<double>(msg_count) / packets_sent_ : 0.0);

    std::fclose(f);
}

void UdpServer::close() noexcept {
    if (sock_ != INVALID) {
        flush();
        close_sock(sock_);
        sock_ = INVALID;
    }
#ifdef _WIN32
    WSACleanup();
#endif
}