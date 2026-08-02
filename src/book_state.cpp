#include "book_state.hpp"
#include <cstring>
#include <new>

BookState::BookState(uint32_t capacity_pow2)
    : slots_(nullptr)
    , state_(nullptr)
    , capacity_(uint64_t{1} << capacity_pow2)
    , mask_((uint64_t{1} << capacity_pow2) - 1)
{
    slots_ = new RestingOrder[capacity_];
    state_ = new uint8_t[capacity_];
    std::memset(state_, EMPTY, capacity_);
}

BookState::~BookState() {
    delete[] slots_;
    delete[] state_;
}

void BookState::reset() noexcept {
    std::memset(state_, EMPTY, capacity_);
    stats_  = Stats{};
    digest_ = 0xcbf29ce484222325ULL;
}

void BookState::fold_op(const ResolvedOp& op) noexcept {
    fold(static_cast<uint64_t>(op.action));
    fold(op.order_ref);
    fold(op.quantity);
    fold(op.price);
    fold(static_cast<uint64_t>(op.side));
}

// probing stops at EMPTY but steps over TOMBSTONE, so erasing never breaks the
// chain for keys inserted behind it
RestingOrder* BookState::find(uint64_t ref) noexcept {
    uint64_t idx    = hash_ref(ref) & mask_;
    uint64_t probes = 0;

    while (state_[idx] != EMPTY) {
        if (state_[idx] == OCCUPIED && slots_[idx].order_ref == ref) {
            if (probes > stats_.max_probe) stats_.max_probe = probes;
            return &slots_[idx];
        }
        idx = (idx + 1) & mask_;
        if (++probes >= capacity_) break;
    }
    return nullptr;
}

bool BookState::insert(const RestingOrder& ro) noexcept {
    uint64_t idx    = hash_ref(ro.order_ref) & mask_;
    uint64_t probes = 0;
    int64_t  reuse  = -1;

    while (state_[idx] != EMPTY) {
        if (state_[idx] == OCCUPIED && slots_[idx].order_ref == ro.order_ref) {
            slots_[idx] = ro;              // exchange reused the ref
            return true;
        }
        if (state_[idx] == TOMBSTONE && reuse < 0)
            reuse = static_cast<int64_t>(idx);

        idx = (idx + 1) & mask_;
        if (++probes >= capacity_) {
            ++stats_.table_full;
            return false;
        }
    }

    if (reuse >= 0) idx = static_cast<uint64_t>(reuse);
    if (probes > stats_.max_probe) stats_.max_probe = probes;

    slots_[idx] = ro;
    state_[idx] = OCCUPIED;
    ++stats_.live;
    if (stats_.live > stats_.peak_live) stats_.peak_live = stats_.live;
    return true;
}

bool BookState::erase(uint64_t ref) noexcept {
    uint64_t idx    = hash_ref(ref) & mask_;
    uint64_t probes = 0;

    while (state_[idx] != EMPTY) {
        if (state_[idx] == OCCUPIED && slots_[idx].order_ref == ref) {
            state_[idx] = TOMBSTONE;
            --stats_.live;
            return true;
        }
        idx = (idx + 1) & mask_;
        if (++probes >= capacity_) break;
    }
    return false;
}

bool BookState::resolve(const Order& msg, ResolveResult& out) noexcept {
    const bool ok = resolve_impl(msg, out);
    if (ok)
        for (uint8_t i = 0; i < out.count; ++i) fold_op(out.ops[i]);
    return ok;
}

bool BookState::resolve_impl(const Order& msg, ResolveResult& out) noexcept {
    out.count = 0;

    switch (msg.action) {

    // only message carrying full detail — remember it
    case Action::Add: {
        RestingOrder ro{};
        ro.order_ref = msg.order_ref;
        ro.quantity  = msg.quantity;
        ro.price     = msg.price;
        ro.side      = msg.side;
        std::memcpy(ro.stock, msg.stock, 8);

        if (!insert(ro)) return false;
        ++stats_.adds;

        ResolvedOp& op  = out.ops[0];
        op.action       = Action::Add;
        op.order_ref    = msg.order_ref;
        op.quantity     = msg.quantity;
        op.price        = msg.price;
        op.match_number = 0;
        op.timestamp_ns = msg.timestamp_ns;
        op.side         = msg.side;
        std::memcpy(op.stock, msg.stock, 8);
        out.count = 1;
        return true;
    }

    case Action::Execute: {
        RestingOrder* ro = find(msg.order_ref);
        if (!ro) { ++stats_.unresolved; return false; }

        uint32_t filled = (msg.quantity < ro->quantity) ? msg.quantity
                                                        : ro->quantity;

        ResolvedOp& op  = out.ops[0];
        op.action       = Action::Execute;
        op.order_ref    = msg.order_ref;
        op.quantity     = filled;
        // C carries its own price, E executes where the order rests
        op.price        = (msg.price != 0) ? msg.price : ro->price;
        op.match_number = msg.match_number;
        op.timestamp_ns = msg.timestamp_ns;
        op.side         = ro->side;
        std::memcpy(op.stock, ro->stock, 8);
        out.count = 1;

        ro->quantity -= filled;
        if (ro->quantity == 0) erase(msg.order_ref);

        ++stats_.executes;
        return true;
    }

    // partial pull, order keeps its place in the queue
    case Action::Cancel: {
        RestingOrder* ro = find(msg.order_ref);
        if (!ro) { ++stats_.unresolved; return false; }

        uint32_t pulled = (msg.quantity < ro->quantity) ? msg.quantity
                                                        : ro->quantity;

        ResolvedOp& op  = out.ops[0];
        op.action       = Action::Cancel;
        op.order_ref    = msg.order_ref;
        op.quantity     = pulled;
        op.price        = ro->price;
        op.match_number = 0;
        op.timestamp_ns = msg.timestamp_ns;
        op.side         = ro->side;
        std::memcpy(op.stock, ro->stock, 8);
        out.count = 1;

        ro->quantity -= pulled;
        if (ro->quantity == 0) erase(msg.order_ref);

        ++stats_.cancels;
        return true;
    }

    case Action::Delete: {
        RestingOrder* ro = find(msg.order_ref);
        if (!ro) { ++stats_.unresolved; return false; }

        ResolvedOp& op  = out.ops[0];
        op.action       = Action::Delete;
        op.order_ref    = msg.order_ref;
        op.quantity     = ro->quantity;
        op.price        = ro->price;
        op.match_number = 0;
        op.timestamp_ns = msg.timestamp_ns;
        op.side         = ro->side;
        std::memcpy(op.stock, ro->stock, 8);
        out.count = 1;

        erase(msg.order_ref);
        ++stats_.deletes;
        return true;
    }

    // queue priority is lost, so this has to be remove + insert rather than an
    // in-place edit
    case Action::Replace: {
        RestingOrder* ro = find(msg.order_ref);
        if (!ro) { ++stats_.unresolved; return false; }

        const Side side = ro->side;
        char stock[8];
        std::memcpy(stock, ro->stock, 8);

        ResolvedOp& del  = out.ops[0];
        del.action       = Action::Delete;
        del.order_ref    = msg.order_ref;
        del.quantity     = ro->quantity;
        del.price        = ro->price;
        del.match_number = 0;
        del.timestamp_ns = msg.timestamp_ns;
        del.side         = side;
        std::memcpy(del.stock, stock, 8);

        erase(msg.order_ref);

        RestingOrder nro{};
        nro.order_ref = msg.new_order_ref;
        nro.quantity  = msg.quantity;
        nro.price     = msg.price;
        nro.side      = side;
        std::memcpy(nro.stock, stock, 8);

        if (!insert(nro)) { out.count = 1; return true; }

        ResolvedOp& add  = out.ops[1];
        add.action       = Action::Add;
        add.order_ref    = msg.new_order_ref;
        add.quantity     = msg.quantity;
        add.price        = msg.price;
        add.match_number = 0;
        add.timestamp_ns = msg.timestamp_ns;
        add.side         = side;
        std::memcpy(add.stock, stock, 8);

        out.count = 2;
        ++stats_.replaces;
        return true;
    }

    // hidden liquidity, nothing resting to touch
    case Action::Trade: {
        ResolvedOp& op  = out.ops[0];
        op.action       = Action::Trade;
        op.order_ref    = msg.order_ref;
        op.quantity     = msg.quantity;
        op.price        = msg.price;
        op.match_number = msg.match_number;
        op.timestamp_ns = msg.timestamp_ns;
        op.side         = msg.side;
        std::memcpy(op.stock, msg.stock, 8);
        out.count = 1;

        ++stats_.trades;
        return true;
    }

    default:
        return false;
    }
}