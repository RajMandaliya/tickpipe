# tickpipe

A hardware-accurate electronic exchange, written in C++20, that replays a full
day of real NASDAQ TotalView-ITCH 5.0 market data and reconstructs the order
book message by message — from a file, or over a UDP feed.

The matching engine is modelled as a 5-stage FPGA pipeline: fixed-width
registers between stages, a price-indexed ladder instead of a searched table,
no heap allocation on the hot path, and fixed-point arithmetic throughout. A
conventional STL engine runs alongside it as a control.

The feed handler speaks MoldUDP64, the same framing NASDAQ wraps ITCH in, with
sequence-level gap accounting and a lock-free ring between the socket thread
and the book thread.

## Verified against a real trading day

Input is the NASDAQ TotalView-ITCH 5.0 file for **30 July 2019** — 8.7 GB,
282,229,684 messages, 8,849 symbols.

Every figure in this section is a property of the data, not the machine. Any
run on any hardware reproduces them exactly.

### Feed

| | |
|---|---|
| Messages parsed | 282,229,684 |
| Symbols in directory | 8,849 |
| Filtered (not subscribed) | 280,904,519 |
| AAPL book messages | 1,315,839 |
| Malformed | 0 |
| Unknown message type | 0 |

### AAPL book operations

| Operation | Count |
|---|---|
| Add | 611,614 |
| Execute | 59,822 |
| Cancel | 1,596 |
| Delete | 568,438 |
| Replace | 57,717 |
| Hidden trade | 16,652 |
| Peak live orders | 32,854 |
| Unresolved references | 0 |
| Orphan operations | 0 |

Deletes outnumber cancels 356:1. That ratio is real — NASDAQ overwhelmingly
withdraws liquidity with a full delete rather than a partial cancel.

### Book at the closing bell, 16:00:00

```
Best bid    $208.8500 x 3600
Best ask    $208.8900 x 1317
Spread      $0.0400
Levels      2777 bid / 1934 ask
Last trade  $208.8800
```

### Full session, through the 20:00 close

```
Last trade      $218.1000
Shares traded   7,218,530
```

Apple reported Q3 earnings after the bell that day and the stock moved in
after-hours trading. The last print reconstructed from the raw feed lands
within a few cents of the figure reported in contemporaneous coverage — an
external check the system was never tuned for.

### Determinism

```
Operation digest    0x2b5fee61633edf1a
Book digest @ close 0xdc379fe006137934
```

FNV-1a over every resolved book operation, and over the full ladder at the
closing bell. Same input file produces the same two values on any machine, any
compiler, any optimisation level. This is the conformance check: two engines
that agree on these agree on everything.

## The UDP feed handler

The file replay and the socket feed drive the same consumer. Only the source
changes.

```
[replay server] --MoldUDP64/UDP--> [feed handler] --> book
 reads ITCH file                    same pipeline as the file path
 paces to the ITCH clock
```

Replaying the entire day over loopback at 300x:

| | |
|---|---|
| Messages sent | 282,229,684 |
| Packets sent | 6,259,367 (45.1 msgs/packet) |
| Packets received | 6,259,367 |
| Messages lost | 0 |
| Gaps detected | 0 |
| Ring high water | 23 MB of 128 MB |
| Send rate | 1.38M msg/s |

Every book counter matches the file replay exactly — 611,614 adds, 568,438
deletes, 7,218,530 shares, zero unresolved references. The reconstructed book
closes uncrossed at $218.01 / $218.12.

### Why it is built this way

**MoldUDP64 framing.** Each datagram carries a session id, the sequence number
of its first message, and a message count. Without sequencing a receiver cannot
tell a complete stream from a lossy one — it silently builds a corrupt book and
reports success. With it, every missing message is counted.

**Batching.** Roughly 45 ITCH messages per datagram. One message per packet
meant 282M datagrams and a receiver bound by syscall rate rather than
bandwidth.

**A lock-free SPSC ring between socket and book.** The receive thread does
nothing but `recv()` and a memcpy; the book thread parses, resolves and renders
at its own pace. Sharing one thread meant every microsecond spent resolving an
order reference was a microsecond the kernel buffer spent filling, and bursts
were lost. Moving the buffer into userspace turns an 8 MB kernel limit into a
128 MB one.

**Symbol filtering at the receiver.** The exchange multicasts every symbol; the
handler subscribes and discards the rest, dropping 99.5% of messages on a
`stock_locate` lookup before the parser runs. This is what a real feed handler
does, and it is also the only way one book means anything — 8,849 symbols in a
single ladder produces bids above asks and nothing else.

### What loss actually costs

An earlier build lost 24% of messages. Tuning packet size and replay speed
moved that to 23% and then to 62%; none of it was the real problem. The
bottleneck turned out to be the terminal — a full-screen ANSI redraw costs
~30 ms on a Windows console, and at 40 ms intervals the book thread spent 85%
of its time drawing. Assembling each frame in memory and writing it with a
single `fwrite` took that to 10%, and loss went to zero.

The intermediate result is worth recording. At 99.93% delivery the book was
still visibly wrong: 453 missing deletes strand 453 orders in the book
permanently, and one stale ask from mid-morning is enough to cross against an
after-hours bid. Market data does not degrade gracefully — errors are
permanent and cumulative. That is why exchanges run redundant A/B feeds,
publish periodic snapshots, and support retransmission. Detection is not
recovery.

## Performance

Machine dependent, unlike everything above. Measured on Windows, MSYS2 UCRT64,
GCC 16.1, `-O3 -march=native`.

`steady_clock` on this platform ticks at 100 ns, so timing a single sub-100 ns
operation measures the clock rather than the code. Figures below are batch
means over 1000 batches of 1000 operations. The benchmark reports the measured
timer resolution before anything else so the reader can judge the numbers.

| | mean | p50 | p99 | p999 | throughput |
|---|---|---|---|---|---|
| ITCH parse | 15.9 ns | 15.8 | 19.6 | 26.2 | 62.7 M msg/s |
| FPGA pipeline | 40.1 ns | 39.6 | 55.3 | 143.2 | 24.9 M ops/s |
| STL engine | 81.9 ns | 78.4 | 162.0 | 211.1 | 12.2 M ops/s |

The mean is not the interesting column. The pipeline moves 0.3 ns between p50
and p90; the STL engine moves 12 ns, because `std::map` node depth and
allocator behaviour vary with book state. Predictability is what the hardware
model buys, and it holds at p99: 55 ns against 162 ns.

Full-file replay is I/O bound, not CPU bound — 74 s for 8.7 GB, about
117 MB/s cold. Warm cache reaches roughly 1.5 GB/s, which is a measurement of
the page cache and is not quoted as a result.

## Architecture

```
  ITCH bytes  (file, or MoldUDP64 over UDP)
      |
      v
  ItchParser ......... dispatch on message type, per-type length validation
      |
      v
  SymbolDirectory .... 'R' messages map stock_locate -> ticker
      |                drops unsubscribed symbols before the parse
      v
  BookState .......... resolves bare order references into full operations
      |                fixed-capacity open-addressing table, zero allocation
      v
  +---------------------------+
  |                           |
  v                           v
FpgaPipeline            SoftwareEngine
5 stages, price          std::map control
ladder, bitmap           implementation
priority encoder
```

### Why the reference resolution layer exists

Five of the eight book-affecting ITCH messages — Execute, Execute-with-price,
Cancel, Delete and Replace — carry only an order reference. No side, no price,
no ticker. The exchange assumes the receiver remembered the order from its Add.

Without that layer a parser sees adds and nothing else: the book grows without
bound and no liquidity is ever withdrawn.

### Why the price ladder

A price tick maps directly to a slot: `slot = (price - base) / tick`. One
subtract, one divide, no search — the same address arithmetic an FPGA performs
into block RAM. Best bid and best ask come from a priority encode over an
occupancy bitmap, which is a single cycle in hardware and a handful of
`clz`/`ctz` instructions here.

An earlier version scanned a table of occupied levels, which was O(levels) per
order and made the "hardware" model slower than `std::map`. The ladder is both
faster and more faithful to what it claims to model.

## Components

| Component | Purpose |
|---|---|
| `itch_parser` | ITCH 5.0 decoder, all 21 message types, per-type length validation |
| `symbol_directory` | Stock directory, flat array indexed by `stock_locate` |
| `book_state` | Order reference resolution, fixed-capacity hash table |
| `pipeline` | 5-stage FPGA model with price ladder |
| `software_engine` | STL matching engine, used as a control |
| `itch_replay` | Memory-mapped file replay with symbol filtering |
| `udp_server` | MoldUDP64 replay server, batching and ITCH-clock pacing |
| `udp_receiver` | Feed handler: socket thread, gap detection, symbol filter |
| `spsc_ring` | Lock-free single-producer/single-consumer byte ring |
| `risk_engine` | Pre-trade checks: fat finger, price bands, zero quantity |
| `wal` | Write-ahead log and crash recovery |
| `ouch` | OUCH binary order entry serialisation |
| `numa` | Core pinning for the matching and socket threads |
| `visualizer` | Terminal book display, single buffered write per frame |

## Build

Requires CMake, Ninja, and a C++20 compiler. Developed on MSYS2 UCRT64.

```bash
mkdir build && cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ..
ninja
```

Binaries:

```
exchange_main    all demos, including the full-day file replay
benchmark        latency and throughput suite
live_exchange    binds UDP, runs the full pipeline
replay_server    reads an ITCH file, sends MoldUDP64
```

Run the feed end to end in two terminals:

```bash
./live_exchange.exe
./replay_server.exe /path/to/itch_data 300
```

Launch order does not matter — the receiver blocks until the first packet and
uses an idle timeout to detect end of stream.

## Data

NASDAQ publishes TotalView-ITCH sample files at `emi.nasdaq.com/ITCH`. Place
the uncompressed file at the repository root as `itch_data`, or pass a path.

The file is not included here — it is 8.7 GB.

## Known limitations

Stated deliberately, because they are the interesting part.

**No gap recovery.** Losses are detected and counted precisely, but nothing
repairs them. The complete answer is redundant A/B feeds plus a retransmission
request channel, which is what NASDAQ actually operates.

**Single symbol at a time.** One book per process. Real handlers either filter
to a subscribed universe, as this does, or run per-symbol books keyed on
`stock_locate`. At NASDAQ's peak, all 8,849 symbols together hold tens of
millions of resting orders — well beyond the current table capacity.

**The price ladder is a fixed window.** 2048 slots at a penny tick spans
$20.48. AAPL's book on this day spanned wider, so a real replay through the
pipeline reports non-zero `window_misses`. Production systems slide the window
or fall back to a slow path; the count is reported rather than hidden.

**Cross trades are ignored.** The `Q` message carries the opening and closing
auction prints. The official closing price comes from the closing cross, so the
16:00 figure above is the last continuous-session trade, not the official
close.

**Risk gating on replayed data is questionable.** The live path runs incoming
adds through the risk engine, which rejects a few thousand orders that the real
exchange accepted. Their later deletes then have nothing to remove. Risk checks
belong on order entry, not on a market data replay.

**Order references are not recycled across days.** `stock_locate` and order
reference numbers are valid for one trading session only.

## Next

- Gap recovery: A/B feed arbitration and retransmission requests
- Cross trade (`Q`) handling for true auction prices
- Per-symbol books keyed on `stock_locate`
- Live socket feed — the file and UDP paths already share the consumer, so
  only the source changes