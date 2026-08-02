#include "itch_parser.hpp"
#include <cstring>

// lengths are fixed per type. a single `len < 36` floor lets C/R/F/Q/P/I
// through and decodes them as Add Orders, and rejects D/X/E/U for being
// too short — so the book only ever grows.
uint16_t ItchParser::expected_length(uint8_t type) noexcept {
    switch (type) {
        case 'S': return 12;  // system event
        case 'R': return 39;  // stock directory
        case 'H': return 25;  // trading action
        case 'Y': return 20;  // reg sho
        case 'L': return 26;  // participant position
        case 'V': return 35;  // mwcb decline
        case 'W': return 12;  // mwcb status
        case 'K': return 28;  // ipo quoting
        case 'J': return 35;  // luld collar
        case 'h': return 21;  // operational halt

        case 'A': return 36;  // add
        case 'F': return 40;  // add w/ mpid
        case 'E': return 31;  // executed
        case 'C': return 36;  // executed w/ price
        case 'X': return 23;  // cancel
        case 'D': return 19;  // delete
        case 'U': return 35;  // replace

        case 'P': return 44;  // trade, non-cross
        case 'Q': return 40;  // cross trade
        case 'B': return 19;  // broken trade
        case 'I': return 50;  // noii
        case 'N': return 20;  // rpii

        default:  return 0;
    }
}

bool ItchParser::is_book_message(uint8_t type) noexcept {
    switch (type) {
        case 'A': case 'F': case 'E': case 'C':
        case 'X': case 'D': case 'U': case 'P':
            return true;
        default:
            return false;
    }
}

bool ItchParser::parse(const uint8_t* data, uint32_t len, Order& out) noexcept {
    if (!data || len < 11) { ++counts_.malformed; return false; }

    const uint8_t  type   = data[0];
    const uint16_t expect = expected_length(type);

    if (expect == 0)            { ++counts_.unknown;   return false; }
    if (len != expect)          { ++counts_.malformed; return false; }
    if (!is_book_message(type)) { ++counts_.ignored;   return false; }

    out = Order{};
    out.stock_locate = be16(data + 1);
    out.timestamp_ns = be48(data + 5);

    switch (type) {
    // ref[11] side[19] shares[20] stock[24] price[32]
    // F adds attribution at [36], which we don't use
    case 'A':
    case 'F':
        out.action    = Action::Add;
        out.order_ref = be64(data + 11);
        out.side      = (data[19] == 'B') ? Side::Buy : Side::Sell;
        out.quantity  = be32(data + 20);
        std::memcpy(out.stock, data + 24, 8);
        out.price     = be32(data + 32);
        break;

    // ref[11] shares[19] match[23]
    // executes at the resting price, so no price on the wire
    case 'E':
        out.action       = Action::Execute;
        out.order_ref    = be64(data + 11);
        out.quantity     = be32(data + 19);
        out.match_number = be64(data + 23);
        break;

    // ref[11] shares[19] match[23] printable[31] price[32]
    case 'C':
        out.action       = Action::Execute;
        out.order_ref    = be64(data + 11);
        out.quantity     = be32(data + 19);
        out.match_number = be64(data + 23);
        out.price        = be32(data + 32);
        // non-printable still reduces the order but must not print a trade
        if (data[31] != 'Y') out.match_number = 0;
        break;

    // ref[11] shares[19]
    case 'X':
        out.action    = Action::Cancel;
        out.order_ref = be64(data + 11);
        out.quantity  = be32(data + 19);
        break;

    // ref[11]
    case 'D':
        out.action    = Action::Delete;
        out.order_ref = be64(data + 11);
        break;

    // old ref[11] new ref[19] shares[27] price[31]
    // side and stock carry over from the original
    case 'U':
        out.action        = Action::Replace;
        out.order_ref     = be64(data + 11);
        out.new_order_ref = be64(data + 19);
        out.quantity      = be32(data + 27);
        out.price         = be32(data + 31);
        break;

    // ref[11] side[19] shares[20] stock[24] price[32] match[36]
    // hidden liquidity — counts as volume, nothing in the book to touch
    case 'P':
        out.action       = Action::Trade;
        out.order_ref    = be64(data + 11);
        out.side         = (data[19] == 'B') ? Side::Buy : Side::Sell;
        out.quantity     = be32(data + 20);
        std::memcpy(out.stock, data + 24, 8);
        out.price        = be32(data + 32);
        out.match_number = be64(data + 36);
        break;

    default:
        ++counts_.ignored;
        return false;
    }

    out.valid = 1;
    ++counts_.parsed;
    return true;
}