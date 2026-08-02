#pragma once
#include <cstdint>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

#include "itch_parser.hpp"

// listens on UDP port, receives ITCH messages, calls callback per order
class UdpReceiver {
public:
    static constexpr int    DEFAULT_PORT    = 12001;
    static constexpr int    MAX_PACKET_SIZE = 1026;
    static constexpr int    RECV_TIMEOUT_MS = 5000;

    UdpReceiver() noexcept;
    ~UdpReceiver();

    bool init(int port) noexcept;

    // receive loop — calls callback for each parsed order
    // returns when timeout expires (sender finished)
    void receive_loop(void (*callback)(const Order&, void* ctx),
                      void* ctx) noexcept;

    void close() noexcept;

    uint64_t packets_received() const noexcept { return packets_received_; }
    uint64_t orders_parsed()    const noexcept { return orders_parsed_; }

private:
#ifdef _WIN32
    SOCKET sock_;
#else
    int    sock_;
#endif
    uint64_t packets_received_;
    uint64_t orders_parsed_;

    static uint16_t read_be16(const uint8_t* p) noexcept {
        return static_cast<uint16_t>(p[0] << 8 | p[1]);
    }
};