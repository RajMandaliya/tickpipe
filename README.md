# tickpipe

A hardware-accurate electronic exchange, written in C++20, that replays a full
day of real NASDAQ TotalView-ITCH 5.0 market data and reconstructs the order
book message by message.

The matching engine is modelled as a 5-stage FPGA pipeline: fixed-width
registers between stages, a price-indexed ladder instead of a searched table,
no heap allocation on the hot path, and fixed-point arithmetic throughout. A
conventional STL engine runs alongside it as a control.

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
  ITCH bytes
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
| `risk_engine` | Pre-trade checks: fat finger, price bands, zero quantity |
| `wal` | Write-ahead log and crash recovery |
| `ouch` | OUCH binary order entry serialisation |
| `numa` | Core pinning for the matching thread |
| `visualizer` | Terminal book display, reads a depth snapshot |
| `udp_server` / `udp_receiver` | Feed transport, replay server and receiver |

## Build

Requires CMake, Ninja, and a C++20 compiler. Developed on MSYS2 UCRT64.

```bash
mkdir build && cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ..
ninja
```

Binaries:

```
exchange_main    all demos, including the full-day replay
benchmark        latency and throughput suite
replay_server    reads an ITCH file, sends over UDP
live_exchange    binds UDP, runs the full pipeline
```

## Data

NASDAQ publishes TotalView-ITCH sample files at `emi.nasdaq.com/ITCH`. Place
the uncompressed file at the repository root as `itch_data`, or pass a path.

The file is not included here — it is 8.7 GB.

## Program output

```
[ 7 ] Real NASDAQ Market Data Replay
  ------------------------------------------------------------
  File     : 07302019.NASDAQ_ITCH50 (July 30 2019)
  Symbol   : AAPL
  ...paste the whole demo 7 block...
```

Live book during replay, paced to the ITCH clock at 300x:

```
  tickpipe  AAPL    19:24:13.233   300x
  ============================================================
    ORDERS    BID QTY      BID | ASK      ASK QTY    ORDERS
  ------------------------------------------------------------
         1        100   217.66 | 217.74   281        4
  ...paste the rest...
```

## Known limitations

Stated deliberately, because they are the interesting part.

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

**Order references are not recycled across days.** `stock_locate` and order
reference numbers are valid for one trading session only.

**No gap detection.** The UDP path does not yet track sequence discontinuities.
Real market data feeds drop packets, which is why exchanges publish redundant
A/B feeds and periodic book snapshots.

## Next

- Cross trade (`Q`) handling for true auction prices
- Sequence gap detection and A/B feed arbitration on the UDP path
- Live socket feed — the file replay and the socket feed the same consumer, so
  only the source changes
- Per-symbol books keyed on `stock_locate`