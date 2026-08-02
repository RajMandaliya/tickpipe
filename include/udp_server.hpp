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

// sends raw ITCH 5.0 binary messages over UDP
// each packet = 2-byte length prefix + message body
// mirrors how real exchange multicast feeds work
class UdpServer {
public:
    static constexpr int    DEFAULT_PORT    = 12001;
    static constexpr double DEFAULT_SPEED   = 1.0;   // 1x realtime
    static constexpr int    MAX_PACKET_SIZE = 1024;

    UdpServer() noexcept;
    ~UdpServer();

    // bind socket and connect to target
    bool init(const char* host, int port) noexcept;

    // send one raw ITCH message — prepends 2-byte length
    bool send_message(const uint8_t* data, uint16_t len) noexcept;

    // replay entire ITCH file over UDP
    // speed_multiplier: 1.0 = realtime, 10.0 = 10x faster, 0 = unlimited
    void replay_file(const char* path, double speed_multiplier) noexcept;

    void close() noexcept;

    uint64_t packets_sent()  const noexcept { return packets_sent_; }
    uint64_t bytes_sent()    const noexcept { return bytes_sent_; }

private:
#ifdef _WIN32
    SOCKET   sock_;
#else
    int      sock_;
#endif
    struct sockaddr_in target_;
    uint64_t packets_sent_;
    uint64_t bytes_sent_;

    static uint16_t read_be16(const uint8_t* p) noexcept {
        return static_cast<uint16_t>(p[0] << 8 | p[1]);
    }

    static uint64_t now_ns() noexcept;
};