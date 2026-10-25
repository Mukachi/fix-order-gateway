# fix-order-gateway

A FIX 4.4 order entry gateway in C++20 for Linux: the session layer, crash-safe recovery and order
entry, in both roles, written from the public specification. Its conformance is shown case by case
against the FIX Trading Community's own test cases, its recovery is tested by killing the process,
and its latency is measured open loop against QuickFIX on the harness of
[lockfree-ipc](https://github.com/Mukachi/lockfree-ipc), with what each configuration survives
stated beside its cost. Where it loses, the tables say so.

## Results

Order to ExecutionReport, application to application, this gateway (fixgw in the tables) against
QuickFIX v1.16.0 through the same venue process, as ranges over three runs on a dedicated, isolated machine (conditions below). Full
tables, every rate, the method and every run's conditions: [RESULTS.md](RESULTS.md). These blocks
are generated from the committed JSON in `results/` by `python run.py report`; the test suite
fails if they disagree.

<!-- results:order-summary -->
| Configuration | Survives | Offered /s | p50 | p99 | Venue p50 |
|---|---|---|---|---|---|
| fixgw, pipeline, journal A, busy-poll | power loss | 1,000 | 115.2 µs–116.1 µs | 125.7 µs–129.2 µs | 9,526–9,948 |
| fixgw, pipeline, journal A, busy-poll | power loss | 10,000 | 140.0 µs–140.3 µs | 193.3 µs–194.1 µs | 9,054–9,541 |
| fixgw, direct, journal A, block | power loss | 1,000 | 214.7 µs–217.9 µs | 230.3 µs–231.3 µs | 9.8–10.1 µs |
| fixgw, direct, journal A, block | power loss | 10,000 | 253.2 µs–257.2 µs | 353.7 µs–359.0 µs | 8,961–9,440 |
| fixgw, direct, journal C, busy-poll | process crash | 1,000 | 20.1 µs–20.6 µs | 25.2 µs–25.7 µs | 9,249–9,691 |
| fixgw, direct, journal C, busy-poll | process crash | 10,000 | 19.0 µs–19.1 µs | 23.9 µs–24.1 µs | 9,013–9,257 |
| QuickFIX 1.16.0, file store | process crash | 1,000 | 103.8 µs–107.6 µs | 114.4 µs–119.0 µs | 9.6–12.2 µs |
| QuickFIX 1.16.0, file store | process crash | 10,000 | 47.1 µs–50.4 µs | 55.7 µs–59.9 µs | 9.4–12.0 µs |
| QuickFIX 1.16.0, PersistMessages=N | nothing persisted | 1,000 | 90.4 µs–93.9 µs | 95.9 µs–99.9 µs | 9.5–12.2 µs |
| QuickFIX 1.16.0, PersistMessages=N | nothing persisted | 10,000 | 32.2 µs–34.8 µs | 37.9 µs–41.3 µs | 9.3–12.1 µs |

Order to ExecutionReport, application to application, open loop, ns, range over the runs. Highest rate sustained: fixgw, pipeline, journal A, busy-poll ≥ 100,000/s; fixgw, direct, journal A, block 10,000/s; fixgw, direct, journal C, busy-poll ≥ 100,000/s; QuickFIX 1.16.0, file store 20,000/s; QuickFIX 1.16.0, PersistMessages=N 50,000/s. The guarantees, the method and every rate: [RESULTS.md](RESULTS.md).
<!-- /results -->

The gateway's own cost per message, by stage, in process:

<!-- results:stage-summary -->
| Gateway | Message | Stage | p50 |
|---|---|---|---|
| fixgw | ExecutionReport in | framing and decoding | 305 |
| fixgw | ExecutionReport in | dictionary check (note 006) | 795–812 |
| fixgw | ExecutionReport in | order book (note 007) | 296–301 |
| fixgw | ExecutionReport in | session, the whole inbound path | 1,273–1,281 |
| fixgw | ExecutionReport in | journaled session (book, journal C), inbound | 1,940–1,959 |
| fixgw | NewOrderSingle out | body checks (note 007) | 1,243–1,367 |
| fixgw | NewOrderSingle out | order book check and record (note 007) | 324–329 |
| fixgw | NewOrderSingle out | session send (encode, store) | 151–152 |
| fixgw | NewOrderSingle out | journaled session send (checks, book, encode, journal C) | 1,888–1,895 |
| QuickFIX 1.16.0 | ExecutionReport in | FIX::Message from the bytes, the FIX 4.4 dictionary validating | 2,057–2,060 |

Ns per call, in process, range over the runs, clock cost included. Method: [RESULTS.md](RESULTS.md).
<!-- /results -->

The journal's durability strategies alone, in process ([note 004](docs/design/004-store.md)):

<!-- results:store-summary -->
| Strategy | Bound | Offered /s | p50 | p99 | p99.9 | max | Sustained /s |
|---|---|---|---|---|---|---|---|
| A | - | 1,000 | 49.3 µs–49.5 µs | 55.6 µs–56.0 µs | 59.5 µs–60.3 µs | 730.8 µs–1,661.4 µs | 1,000 |
| B | 4 records or 20 us | 1,000 | 64.9 µs–65.3 µs | 68.5 µs–72.5 µs | 78.5 µs–175.5 µs | 1,698.6 µs–1,924.8 µs | 1,000 |
| B | 16 records or 100 us | 1,000 | 144.9 µs–145.4 µs | 148.9 µs–149.0 µs | 155.3 µs–162.2 µs | 1,644.5 µs–2,056.6 µs | 1,000 |
| B | 64 records or 500 us | 1,000 | 544.8 µs–545.4 µs | 548.5 µs–550.0 µs | 556.4 µs–577.3 µs | 1,476.1 µs–2,239.7 µs | 1,000 |
| C | - | 1,000 | 390–394 | 3,099–3,382 | 5,802–6,455 | 104.0 µs–106.5 µs | 1,000 |
| D | standby on CPU 4, loopback | 1,000 | 72.4 µs–73.0 µs | 79.4 µs–79.9 µs | 83.4 µs–83.9 µs | 110.6 µs–977.1 µs | 1,000 |

Order arrival to release of its bytes, ns, at 1,000 orders a second, range over 3 runs. Every rate, the guarantees and the conditions: [RESULTS.md](RESULTS.md).
<!-- /results -->

The network path: loopback, a 10GBASE-T card and its busy-poll mode ([note 005](docs/design/005-engine.md)):

<!-- results:net-summary -->
| Measure | loopback p50 | card p50 | card-busypoll p50 |
|---|---|---|---|
| round trip, busy-poll | 7,431–7,538 | 39.9 µs–40.5 µs | 31.0 µs–31.6 µs |
| round trip, block | 13.7 µs–13.9 µs | 42.0 µs–45.0 µs | 31.5 µs–32.1 µs |
| round trip, spin then block | 7,271–7,549 | 37.2 µs–40.5 µs | 30.6 µs–31.5 µs |
| order path, direct, 1,000/s | 122.0 µs–123.9 µs | 150.2 µs–153.3 µs | 150.0 µs–151.3 µs |
| order path, pipeline, 1,000/s | 106.3 µs–106.8 µs | 135.8 µs–139.8 µs | 132.0 µs–133.5 µs |
| order path, direct, 10,000/s | 237.1 µs–238.3 µs | 234.5 µs–237.6 µs | 235.1 µs–235.4 µs |
| order path, pipeline, 10,000/s | 98.7 µs–99.2 µs | 145.6 µs–183.6 µs | 147.5 µs–147.7 µs |

Range over the runs of each path. Conditions, p99 and the method: [RESULTS.md](RESULTS.md).
<!-- /results -->

The codec, by representation and by branch-predictor state ([notes 002](docs/design/002-codec.md), [009](docs/design/009-codec-simd.md)):

<!-- results:codec-summary -->
| Operation | Message | Repr. | Bytes | State | Mispred./msg | Episodes | Mixed | p50 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|---|---|
| frame and decode | NewOrderSingle | A | 173 | fast | 13.05–13.94 | 15–131 | 2–4 | 987–1,008 | 1,212–1,242 | 1,301–1,329 | 2,255–2,866 |
| frame and decode | NewOrderSingle | A | 173 | slow | 42.68–42.72 | 65–182 | 2–4 | 1,210–1,216 | 1,459–1,467 | 1,548–1,559 | 2,624–2,900 |
| frame and decode | NewOrderSingle | scalar | 173 | fast | 6.87–6.91 | 92–119 | 2–7 | 365–373 | 461–481 | 641–650 | 1.9–17.6 µs |
| frame and decode | NewOrderSingle | scalar | 173 | slow | 24.41–24.84 | 75–106 | 2–7 | 499–501 | 601–609 | 756–757 | 1,729–1,987 |
| frame and decode | NewOrderSingle | SIMD | 173 | fast | 4.17 | 17–147 | 1–6 | 280–281 | 402 | 624–625 | 1,548–2,071 |
| frame and decode | NewOrderSingle | SIMD | 173 | slow | 16.32–16.80 | 49–182 | 1–6 | 365–368 | 451–454 | 695–697 | 1,826–2,204 |
| frame and decode | NewOrderSingle | SIMD, padded | 173 | fast | 2.11–2.13 | 104–107 | 6–12 | 248–249 | 366–383 | 592–595 | 1,700–1,960 |
| frame and decode | NewOrderSingle | SIMD, padded | 173 | slow | 11.14–11.50 | 84–90 | 6–12 | 313–315 | 387–391 | 654–656 | 1,739–1,818 |
| frame and decode | NewOrderSingle | two-stage | 173 | fast | 2.35–2.39 | 71–99 | 3–7 | 282–283 | 399–413 | 623–627 | 1,728–2,049 |
| frame and decode | NewOrderSingle | two-stage | 173 | slow | 13.91–14.60 | 94–126 | 3–7 | 364–366 | 448–449 | 691–694 | 1,839–1,942 |
| encode | NewOrderSingle | A | 173 | fast | 0.89 | 5–8 | 0–1 | 878–881 | 917–920 | 937–942 | 1,310–2,152 |
| encode | NewOrderSingle | A | 173 | slow | 8.81–11.36 | 191–195 | 0–1 | 937–951 | 1,041–1,062 | 1,070–1,092 | 2,392–2,411 |
| encode | NewOrderSingle | B | 173 | fast | 0.19 | 24–99 | 0–1 | 260 | 283 | 302–306 | 1,635–1,697 |
| encode | NewOrderSingle | B | 173 | slow | 1.57–1.62 | 100–176 | 0–1 | 262 | 291 | 308–309 | 1,654–1,704 |
| encode | NewOrderSingle | C | 190 | one | 0.00 | 200 | 0 | 154 | 158 | 161–168 | 1,535–1,542 |

Ns per call, range over 3 runs, clock cost included (about the empty window's p50). The branch predictor has two stable states for this code; figures are per state, never pooled, and how many episodes fall in each is not reproducible (note 009). Full tables and conditions: [RESULTS.md](RESULTS.md).
<!-- /results -->

## What is here

| | |
|---|---|
| [`session.hpp`](include/fixgw/session.hpp) | The session layer, sans I/O: logon and logout, sequence numbers, resend and gap fill, heartbeats and test requests, out-of-spec messages. [Note 003](docs/design/003-session.md) |
| [`framing.hpp`](include/fixgw/framing.hpp), [`decode.hpp`](include/fixgw/decode.hpp), [`encode.hpp`](include/fixgw/encode.hpp) | The codec: framing a byte stream, zero-copy decoding into views, encoding into a fixed buffer or from pre-encoded order templates, with SIMD scanning. [Notes 002](docs/design/002-codec.md), [009](docs/design/009-codec-simd.md) |
| [`validate.hpp`](include/fixgw/validate.hpp) | Checks against the FIX 4.4 dictionary, generated at build time from the FIX Trading Community's Orchestra files (pinned and hashed). [Note 006](docs/design/006-conformance.md) |
| [`journal.hpp`](include/fixgw/journal.hpp), [`durability.hpp`](include/fixgw/durability.hpp), [`recovery.hpp`](include/fixgw/recovery.hpp) | The session's inputs journaled and made durable before any output is released, four durability strategies with their guarantees, and recovery by replay. [Note 004](docs/design/004-store.md) |
| [`orders.hpp`](include/fixgw/orders.hpp) | The order state machine, per ClOrdID chain: in-doubt orders, duplicate ExecIDs, the stale-order rule, an outbound throttle. [Note 007](docs/design/007-order-entry.md) |
| [`transport.hpp`](include/fixgw/transport.hpp), [`acceptor_gate.hpp`](include/fixgw/acceptor_gate.hpp), [`engine.hpp`](include/fixgw/engine.hpp), [`pipeline.hpp`](include/fixgw/pipeline.hpp) | Non-blocking TCP, the acceptor's logon gate, and two ways to run a session: one pinned loop, or a Disruptor-style pipeline of three threads over lockfree-ipc's rings. [Note 005](docs/design/005-engine.md) |
| [`venue_sim.hpp`](include/fixgw/venue_sim.hpp) | A venue simulator (price-time book, the sell side of order entry) for the tests and the benchmarks. [Note 007](docs/design/007-order-entry.md) |
| [`bench/`](bench), [`run.py`](run.py) | The benchmarks, and the report that validates published runs and generates RESULTS.md, CONFORMANCE.md and the blocks above. [Note 008](docs/design/008-measurement.md) |

Header-only. In steady state the order path allocates nothing, and a test checks the compiled
hot path's call graph for system calls and allocation.

## Correctness and recovery

- **Conformance.** Every case of the *FIX Session Layer Test Cases* (V1.0, June 2020) is an
  automated test or says why it does not apply; every applicable mandatory case passes. Every
  *Order State Change Matrix* of FIX 4.4 Volume 4 in scope is a test, scripted and, where a book
  produces it, simulated; those out of scope say why. [CONFORMANCE.md](CONFORMANCE.md) is generated
  from the tests' output (notes 006, 007).
- **Recovery under failure.** The process is killed at chosen points and at random, and power loss
  is simulated by discarding what was not made durable; after a restart the sequence numbers,
  resends and order states must come out right every time. Each durability strategy is held to the
  guarantee it states, and the one that does not survive power loss is shown failing it (notes 004,
  005, 007).
- **Interoperability.** Against QuickFIX v1.16.0 in both roles, across a dropped connection, with
  cancels and replaces (`interop/`, note 006).
- **Fuzzing.** libFuzzer targets for the codec, the session, a differential check of the decoders,
  and order entry; their corpora are committed (`fuzz/`).
- **CI** on every push: x64, ThreadSanitizer, AddressSanitizer/UBSan and arm64.

## Findings

- **Against QuickFIX.** With the same guarantee (surviving a process crash, not power loss), this
  gateway's memory-mapped journal is faster than QuickFIX's file store, and faster than QuickFIX
  with nothing persisted at all. QuickFIX's file store, its documented default, never syncs to disk
  (counted: no `fsync` or `fdatasync` calls), so it survives a process crash but not power loss.
  Surviving power loss here (journal A, one `fdatasync` per input) costs several times more, and
  QuickFIX documents no setting that offers it (note 008).
- **Durability.** On this machine's battery-backed controller cache, `fdatasync` per input is the
  fastest strategy that survives power loss at every rate measured; grouping commits only adds its
  wait, since the disk is never the bottleneck it exists to relieve (note 004).
- **Threads.** The three-stage pipeline is faster than one loop doing everything at every rate
  measured, and of the configurations measured with journal A only the pipeline sustains the
  higher rates (notes 005, 008).
- **The branch predictor.** The decoding code runs in two stable predictor states from run to run,
  so codec figures are published per state, never pooled (note 009). The outbound order checks
  mispredict heavily, and a run with more mispredictions has a higher median (note 008).
- **The measurement found its own faults.** The venue simulator saturated first (one `send` per
  report); a gateway behind by more than a megabyte failed a journal write and ended the session
  (the bytes were journaled as one record); and ext4's writeback of the memory-mapped journal
  stalled the engine in the kernel once the file had been dirty for 30 seconds, so that journal now
  lives on tmpfs, which keeps its guarantee. Each was found from the data, fixed, and every set
  re-measured (note 008, Review).

## How it is measured

- **Open loop.** Orders are submitted on a fixed schedule and latency runs from each order's
  scheduled time, so a stall delays every order due during it. A rate is published only if every
  run sustained it; a rate sustained at the top of the ladder is written as a lower bound.
- **The harness** is lockfree-ipc's, fetched at a pinned commit: TSC clock, percentiles from a
  log-linear histogram after a discarded warm-up, the noise floor and the machine's conditions in
  every run, and a refusal to publish unless the machine is in bench mode and the build is clean.
- **The machine.** HPE ProLiant DL380 Gen9, 2 × Xeon E5-2699 v4, Ubuntu 26.04, kernel 7.0, set
  up as lockfree-ipc describes ([its machine scripts](https://github.com/Mukachi/lockfree-ipc/tree/main/tools/machine)):
  reserved, isolated CPUs on node 0, with services, kernel threads and interrupts fenced off them.
  Journal A on ext4 over a battery-backed RAID controller, journal C on tmpfs, both recorded in
  every run.
- **QuickFIX** is driven by a program written to its documented API, never from its source, on the
  same schedule as this gateway, its application thread on the same CPU, against the same venue.

## Build and test

Linux; C++20; tested with GCC 15.2, CMake 4.2 and Python 3.14.

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

CMake fetches lockfree-ipc at a pinned commit and the FIX 4.4 Orchestra files (pinned, SHA-256
checked) into `build/`. Options: `-DFIXGW_INTEROP=ON` also builds QuickFIX v1.16.0 (fetched into
`build/`) and the interoperability tests and benchmarks; `-DFIXGW_FUZZ=ON` (with clang) builds the
fuzzers. Checks that need a quiet, dedicated machine carry the CTest label `quiet`
(`ctest -LE quiet` skips them). The CI script, [ci/test.sh](ci/test.sh), runs the same builds
locally.

## Running the benchmarks

```sh
build/order_venue 9400 10 40 venue-dir &        # the venue: port, CPU, connections, a directory
build/order_bench 9400 venue-dir out.json        # this gateway, every configuration and rate
build/stage_bench out.json                       # cost per message by stage
build/store_bench out.json                       # the durability strategies alone
build-interop/qf_bench 9400 venue-dir FIX44.xml out.json   # QuickFIX (-DFIXGW_INTEROP=ON)
```

Each writes one JSON file: the environment, the noise floor, the clock, then the results. Run
without a path, a benchmark writes into `results/`, and refuses unless the machine is in bench
mode and the build is clean; given a path elsewhere, it runs anywhere and marks the file
unpublished. The CPU numbers are this machine's; another machine needs them changed in the
benchmark source. To publish runs: list them in `results/published.toml`, then
`python run.py report`.

## Design notes

Every component has a design note written and agreed before its code: proposal, design, spec,
tasks, and a review log with every finding, including the wrong turns. Index:
[docs/README.md](docs/README.md).

## Licence

[MIT](LICENSE). FIX is a trademark of FIX Protocol Limited; the *FIX Session Layer Test Cases* are
© FIX Protocol Limited (CC BY-ND 4.0).
