#include "udp_receiver.hpp"
#include <cstring>
#include <cstdio>

#ifdef _WIN32
static void close_sock(SOCKET s) { closesocket(s); }
static const SOCKET INVALID = INVALID_SOCKET;
#else
static void close_sock(int s)    { ::close(s); }
static const int    INVALID = -1;
#endif

UdpReceiver::UdpReceiver() noexcept
    : sock_(INVALID)
    , packets_received_(0)
    , orders_parsed_(0)
{
#ifdef _WIN32
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

UdpReceiver::~UdpReceiver() { close(); }

bool UdpReceiver::init(int port) noexcept {
    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == INVALID) return false;

    // allow reuse so we can restart quickly
    int reuse = 1;
#ifdef _WIN32
    setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
    setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

    // receive timeout — detect when sender has finished
#ifdef _WIN32
    DWORD timeout = RECV_TIMEOUT_MS;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    struct timeval tv{ RECV_TIMEOUT_MS / 1000,
                       (RECV_TIMEOUT_MS % 1000) * 1000 };
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close();
        return false;
    }

    std::printf("udp_receiver: listening on port %d\n", port);
    return true;
}

void UdpReceiver::receive_loop(
    void (*callback)(const Order&, void* ctx), void* ctx) noexcept {

    uint8_t    buf[MAX_PACKET_SIZE];
    ItchParser parser;
    Order      order{};

    while (true) {
#ifdef _WIN32
        int n = recv(sock_, reinterpret_cast<char*>(buf),
                     MAX_PACKET_SIZE, 0);
#else
        ssize_t n = recv(sock_, buf, MAX_PACKET_SIZE, 0);
#endif

        // timeout = sender finished
        if (n <= 0) break;

        ++packets_received_;

        // strip 2-byte length prefix
        if (n < 3) continue;
        uint16_t msg_len = read_be16(buf);
        if (msg_len == 0 || msg_len > static_cast<uint16_t>(n - 2)) continue;

        // parse ITCH message
        if (parser.parse(buf + 2, msg_len, order)) {
            ++orders_parsed_;
            if (callback) callback(order, ctx);
        }
    }
}

void UdpReceiver::close() noexcept {
    if (sock_ != INVALID) {
        close_sock(sock_);
        sock_ = INVALID;
    }
#ifdef _WIN32
    WSACleanup();
#endif
}