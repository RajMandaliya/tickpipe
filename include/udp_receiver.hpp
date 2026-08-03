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

#include <thread>
#include <atomic>
#include "itch_parser.hpp"
#include "symbol_directory.hpp"
#include "spsc_ring.hpp"
#include "numa.hpp"

struct ReceiverConfig {
    // null = take everything. otherwise 'R' messages are fed to it and
    // anything not subscribed is dropped before the parser sees it.
    SymbolDirectory* directory = nullptr;

    // how long a silence means the sender is done. only armed after the
    // first packet arrives, so launch order does not matter.
    int idle_timeout_ms = 5000;

    // kernel receive buffer. the default is ~64 KB, which a replay server
    // running flat out will overrun.
    int recv_buffer_bytes = 8 * 1024 * 1024;

    // userspace ring between the socket thread and the book thread.
    // 1<<27 = 128 MB, far more burst absorption than the OS will grant a socket.
    uint32_t ring_pow2 = 27;

    // dedicate a core to draining the socket. -1 leaves it to the scheduler.
    int recv_core = 1;
};

// listens on UDP port, receives ITCH messages, calls callback per order
class UdpReceiver {
public:
    static constexpr int DEFAULT_PORT    = 12001;
    static constexpr int MAX_PACKET_SIZE = 2048;   // MoldUDP64 datagram
    static constexpr int MOLD_HEADER     = 20;

    UdpReceiver() noexcept;
    ~UdpReceiver();

    bool init(int port) noexcept;

    // Blocks until the first packet arrives, then returns once the stream has
    // been silent for idle_timeout_ms.
    void receive_loop(void (*callback)(const Order&, void* ctx),
                      void* ctx,
                      const ReceiverConfig& cfg = ReceiverConfig{}) noexcept;

    void close() noexcept;

    uint64_t packets_received()   const noexcept { return packets_received_; }
    uint64_t orders_parsed()      const noexcept { return orders_parsed_; }
    uint64_t messages_filtered()  const noexcept { return messages_filtered_; }
    uint64_t directory_messages() const noexcept { return directory_messages_; }
    uint64_t bytes_received()     const noexcept { return bytes_received_; }
    uint64_t short_packets()      const noexcept { return short_packets_; }

    // gap detection — the whole reason for sequencing the wire
    uint64_t gaps()               const noexcept { return gaps_; }
    uint64_t messages_lost()      const noexcept { return messages_lost_; }
    uint64_t out_of_order()       const noexcept { return out_of_order_; }
    uint64_t expected_seq()       const noexcept { return expected_seq_; }

    // ring pressure — non-zero drops mean the book thread fell behind
    uint64_t ring_dropped()       const noexcept { return ring_dropped_; }
    uint64_t ring_high_water()    const noexcept { return ring_high_water_; }

    // where the book thread spent its time
    uint64_t process_ns()         const noexcept { return process_ns_; }
    uint64_t idle_ns()            const noexcept { return idle_ns_; }
    uint64_t render_ns()          const noexcept { return render_ns_; }

private:
#ifdef _WIN32
    SOCKET sock_;
#else
    int    sock_;
#endif
    uint64_t packets_received_;
    uint64_t orders_parsed_;
    uint64_t messages_filtered_;
    uint64_t directory_messages_;
    uint64_t bytes_received_;
    uint64_t short_packets_;
    uint64_t gaps_;
    uint64_t messages_lost_;
    uint64_t out_of_order_;
    uint64_t expected_seq_;
    uint64_t ring_dropped_;
    uint64_t ring_high_water_;
    uint64_t process_ns_;
    uint64_t idle_ns_;
    uint64_t render_ns_;

    // drains the socket and nothing else
    void recv_thread(SpscRing& ring, std::atomic<bool>& running,
                     int idle_timeout_ms, int core) noexcept;

    void set_timeout(int ms) noexcept;   // 0 = block forever

    static uint16_t read_be16(const uint8_t* p) noexcept {
        return static_cast<uint16_t>(p[0] << 8 | p[1]);
    }
    static uint64_t read_be64(const uint8_t* p) noexcept {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
        return v;
    }
};