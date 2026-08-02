#pragma once
#include <cstdint>
#include <cstring>
#include "pipeline.hpp"

// OUCH 4.2 outbound message types
// these go from exchange back to the client
enum class OuchMsgType : uint8_t {
    OrderAccepted  = 'A',
    OrderRejected  = 'J',
    OrderExecuted  = 'E',
    OrderCancelled = 'C',
};

// OUCH Order Accepted — sent when order enters the book
struct OuchAccepted {
    OuchMsgType type       = OuchMsgType::OrderAccepted;
    uint64_t    timestamp;
    uint64_t    order_ref;
    Side        side;
    uint32_t    quantity;
    uint32_t    price;
    char        stock[8];
    uint8_t     pad[2];
} __attribute__((packed));

// OUCH Order Rejected — sent when risk engine blocks an order
struct OuchRejected {
    OuchMsgType type       = OuchMsgType::OrderRejected;
    uint64_t    timestamp;
    uint64_t    order_ref;
    uint8_t     reason;    // maps to RiskResult
    uint8_t     pad[3];
} __attribute__((packed));

// OUCH Order Executed — sent when a match occurs
struct OuchExecuted {
    OuchMsgType type            = OuchMsgType::OrderExecuted;
    uint64_t    timestamp;
    uint64_t    order_ref;
    uint32_t    executed_qty;
    uint32_t    execution_price;
    uint64_t    match_id;       // unique id for this execution
    uint8_t     pad[3];
} __attribute__((packed));

// OUCH serializer
// builds binary OUCH messages into a caller-supplied buffer
// no heap allocation — caller owns the buffer
class OuchSerializer {
public:
    // returns number of bytes written, 0 on failure
    static uint32_t write_accepted(uint8_t* buf, uint32_t buf_len,
                                   const Order& order,
                                   uint64_t timestamp) noexcept;

    static uint32_t write_rejected(uint8_t* buf, uint32_t buf_len,
                                   const Order& order,
                                   uint8_t reason,
                                   uint64_t timestamp) noexcept;

    static uint32_t write_executed(uint8_t* buf, uint32_t buf_len,
                                   const Order& order,
                                   const MatchResult& result,
                                   uint64_t match_id,
                                   uint64_t timestamp) noexcept;

    // parse an inbound OUCH message type without full decode
    static OuchMsgType peek(const uint8_t* buf) noexcept {
        if (!buf) return OuchMsgType::OrderRejected;
        return static_cast<OuchMsgType>(buf[0]);
    }

private:
    static uint64_t next_match_id() noexcept {
        static uint64_t id = 0;
        return ++id;
    }
};