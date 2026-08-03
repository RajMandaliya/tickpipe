#include "udp_receiver.hpp"
#include <cstring>
#include <cstdio>
#include <chrono>

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
    , messages_filtered_(0)
    , directory_messages_(0)
    , bytes_received_(0)
    , short_packets_(0)
    , gaps_(0)
    , messages_lost_(0)
    , out_of_order_(0)
    , expected_seq_(0)
    , ring_dropped_(0)
    , ring_high_water_(0)
    , process_ns_(0)
    , idle_ns_(0)
    , render_ns_(0)
{
#ifdef _WIN32
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

UdpReceiver::~UdpReceiver() { close(); }

// 0 blocks forever
void UdpReceiver::set_timeout(int ms) noexcept {
#ifdef _WIN32
    DWORD t = static_cast<DWORD>(ms);
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&t), sizeof(t));
#else
    struct timeval tv{ ms / 1000, (ms % 1000) * 1000 };
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

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

// Socket thread. Copies datagrams into the ring and immediately goes back to
// recv(). It must do no parsing, no book work, no rendering — anything else
// here is time the kernel buffer spends filling up.
void UdpReceiver::recv_thread(SpscRing& ring, std::atomic<bool>& running,
                              int idle_timeout_ms, int core) noexcept {
    // give the socket its own core. demo 6 has always claimed core 1 for
    // network I/O; this is the first thread that actually needs it.
    if (core >= 0) ThreadPinner::pin_to_core(core);

    uint8_t buf[MAX_PACKET_SIZE];

    set_timeout(0);          // block until the sender appears
    bool armed = false;

    while (true) {
#ifdef _WIN32
        int n = recv(sock_, reinterpret_cast<char*>(buf), MAX_PACKET_SIZE, 0);
#else
        ssize_t n = recv(sock_, buf, MAX_PACKET_SIZE, 0);
#endif
        if (n <= 0) {
            if (!armed) continue;
            break;                                  // idle window elapsed
        }
        if (!armed) { armed = true; set_timeout(idle_timeout_ms); }

        ++packets_received_;
        bytes_received_ += static_cast<uint64_t>(n);

        ring.push(buf, static_cast<uint32_t>(n));   // drop is counted in-ring
    }

    running.store(false, std::memory_order_release);
}

void UdpReceiver::receive_loop(
    void (*callback)(const Order&, void* ctx), void* ctx,
    const ReceiverConfig& cfg) noexcept {

    int rcvbuf = cfg.recv_buffer_bytes;
#ifdef _WIN32
    setsockopt(sock_, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));
#else
    setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
#endif

    SpscRing ring(cfg.ring_pow2);
    std::atomic<bool> running{true};

    std::thread io(&UdpReceiver::recv_thread, this,
                   std::ref(ring), std::ref(running), cfg.idle_timeout_ms,
                   cfg.recv_core);

    uint8_t    buf[MAX_PACKET_SIZE];
    ItchParser parser;
    Order      order{};

    using SteadyClock = std::chrono::steady_clock;
    auto t_prev = SteadyClock::now();

    // book thread: drain the ring, rebuild, render
    while (true) {
        const uint32_t n = ring.pop(buf, MAX_PACKET_SIZE);

        // split wall time into "doing work" vs "waiting for data" so we can
        // tell a slow consumer apart from a descheduled one
        const auto t_now = SteadyClock::now();
        const uint64_t dt = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                t_now - t_prev).count());
        t_prev = t_now;
        if (n == 0) idle_ns_ += dt; else process_ns_ += dt;

        if (n == 0) {
            if (!running.load(std::memory_order_acquire) && ring.used() == 0)
                break;
            std::this_thread::yield();
            continue;
        }

        if (n < MOLD_HEADER) { ++short_packets_; continue; }

        // MoldUDP64: session[10] seq[8] count[2], then length-prefixed bodies
        const uint64_t seq   = read_be64(buf + 10);
        const uint16_t count = read_be16(buf + 18);

        if (expected_seq_ == 0) {
            expected_seq_ = seq;              // join the stream wherever it is
        } else if (seq > expected_seq_) {
            ++gaps_;
            messages_lost_ += seq - expected_seq_;
        } else if (seq < expected_seq_) {
            ++out_of_order_;                  // duplicate or reordered datagram
        }
        expected_seq_ = seq + count;

        uint32_t off = MOLD_HEADER;
        for (uint16_t m = 0; m < count; ++m) {
            if (off + 2 > n) { ++short_packets_; break; }
            const uint16_t msg_len = read_be16(buf + off);
            off += 2;
            if (msg_len == 0 || off + msg_len > n) { ++short_packets_; break; }

            const uint8_t* msg = buf + off;
            off += msg_len;

            if (cfg.directory) {
                if (cfg.directory->feed(msg, msg_len)) {
                    ++directory_messages_;
                    continue;
                }
                if (msg_len >= 3 &&
                    !cfg.directory->is_subscribed(read_be16(msg + 1))) {
                    ++messages_filtered_;
                    continue;
                }
            }

            if (parser.parse(msg, msg_len, order)) {
                ++orders_parsed_;
                if (callback) callback(order, ctx);
            }
        }
    }

    io.join();
    ring_dropped_    = ring.dropped();
    ring_high_water_ = ring.high_water();
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