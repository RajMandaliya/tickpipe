#pragma once
#include <cstdint>
#include <cstdio>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

// MoldUDP64 transport for ITCH 5.0, the same framing NASDAQ uses.
//
//   [0..9]   session, 10 bytes ASCII
//   [10..17] sequence number of the FIRST message in this packet, 8 bytes BE
//   [18..19] message count, 2 bytes BE
//   [20..]   repeated: 2-byte BE length + message body
//
// Sequencing is what makes drops visible: without it a receiver silently
// builds a corrupt book. Batching many messages per datagram is what stops
// most of the drops happening in the first place.
class UdpServer {
public:
    static constexpr int    DEFAULT_PORT    = 12001;
    static constexpr double DEFAULT_SPEED   = 1.0;
    static constexpr int    MAX_MSG_SIZE    = 1024;

    // stay under a 1500-byte ethernet MTU once IP+UDP headers are added
    static constexpr int    MAX_PAYLOAD     = 1400;
    static constexpr int    MOLD_HEADER     = 20;
    static constexpr char   SESSION[11]     = "TICKPIPE01";

    UdpServer() noexcept;
    ~UdpServer();

    bool init(const char* host, int port) noexcept;

    // queue one ITCH message; flushes automatically when the datagram is full
    bool send_message(const uint8_t* data, uint16_t len) noexcept;

    // push whatever is buffered
    bool flush() noexcept;

    // speed_multiplier: 1.0 = realtime, 10.0 = 10x faster, 0 = unlimited
    void replay_file(const char* path, double speed_multiplier) noexcept;

    void close() noexcept;

    uint64_t packets_sent()  const noexcept { return packets_sent_; }
    uint64_t messages_sent() const noexcept { return messages_sent_; }
    uint64_t bytes_sent()    const noexcept { return bytes_sent_; }

private:
#ifdef _WIN32
    SOCKET   sock_;
#else
    int      sock_;
#endif
    struct sockaddr_in target_;

    uint8_t  packet_[MAX_PAYLOAD];
    int      payload_len_;      // bytes used after the header
    uint16_t packet_msgs_;      // messages in the current datagram
    uint64_t next_seq_;         // sequence of the next message queued

    uint64_t packets_sent_;
    uint64_t messages_sent_;
    uint64_t bytes_sent_;

    static uint16_t read_be16(const uint8_t* p) noexcept {
        return static_cast<uint16_t>(p[0] << 8 | p[1]);
    }
    static void write_be16(uint8_t* p, uint16_t v) noexcept {
        p[0] = static_cast<uint8_t>(v >> 8);
        p[1] = static_cast<uint8_t>(v);
    }
    static void write_be64(uint8_t* p, uint64_t v) noexcept {
        for (int i = 7; i >= 0; --i) { p[i] = static_cast<uint8_t>(v & 0xFF); v >>= 8; }
    }

    static uint64_t now_ns() noexcept;
};