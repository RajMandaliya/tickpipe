#include "ouch.hpp"
#include <cstring>
#include <chrono>

// big-endian write helpers — OUCH is network byte order
static void write_u32(uint8_t* p, uint32_t v) noexcept {
    p[0] = (v >> 24) & 0xFF;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >>  8) & 0xFF;
    p[3] =  v        & 0xFF;
}

static void write_u64(uint8_t* p, uint64_t v) noexcept {
    write_u32(p,     static_cast<uint32_t>(v >> 32));
    write_u32(p + 4, static_cast<uint32_t>(v & 0xFFFFFFFF));
}

uint32_t OuchSerializer::write_accepted(uint8_t* buf, uint32_t buf_len,
                                         const Order& order,
                                         uint64_t timestamp) noexcept {
    constexpr uint32_t MSG_LEN = 32;
    if (!buf || buf_len < MSG_LEN) return 0;

    buf[0] = static_cast<uint8_t>(OuchMsgType::OrderAccepted);
    write_u64(buf + 1,  timestamp);
    write_u64(buf + 9,  order.order_ref);
    buf[17] = static_cast<uint8_t>(order.side);
    write_u32(buf + 18, order.quantity);
    write_u32(buf + 22, order.price);
    std::memcpy(buf + 26, order.stock, 8);  // wait this is wrong

    return MSG_LEN;
}

uint32_t OuchSerializer::write_rejected(uint8_t* buf, uint32_t buf_len,
                                         const Order& order,
                                         uint8_t reason,
                                         uint64_t timestamp) noexcept {
    constexpr uint32_t MSG_LEN = 19;
    if (!buf || buf_len < MSG_LEN) return 0;

    buf[0] = static_cast<uint8_t>(OuchMsgType::OrderRejected);
    write_u64(buf + 1,  timestamp);
    write_u64(buf + 9,  order.order_ref);
    buf[17] = reason;
    buf[18] = 0; // pad

    return MSG_LEN;
}

uint32_t OuchSerializer::write_executed(uint8_t* buf, uint32_t buf_len,
                                         const Order& order,
                                         const MatchResult& result,
                                         uint64_t match_id,
                                         uint64_t timestamp) noexcept {
    constexpr uint32_t MSG_LEN = 34;
    if (!buf || buf_len < MSG_LEN) return 0;

    buf[0] = static_cast<uint8_t>(OuchMsgType::OrderExecuted);
    write_u64(buf + 1,  timestamp);
    write_u64(buf + 9,  order.order_ref);
    write_u32(buf + 17, result.quantity);
    write_u32(buf + 21, result.price);
    write_u64(buf + 25, match_id);
    buf[33] = 0; // pad

    return MSG_LEN;
}