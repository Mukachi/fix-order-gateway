# 008 — Measurement

**Status:** Agreed · **Updated:** 2026-10-24

## Proposal

Note 001 promised the gateway measured like `lockfree-ipc`: order-to-ExecutionReport latency as
percentiles, open loop, beside QuickFIX's on the same harness and machine, and losses shown
(note 001, S5). Notes 002, 004 and 005 measured the parts (codec, journal, the kernel's network
path and the two engine loops); notes 006 and 007 added work to the inbound and outbound paths
(the dictionary checks, the order book, the throttle) and left its cost here. This note measures
the whole: an order from the application to the venue and its ExecutionReport back, ours and
QuickFIX's, and the gateway's own cost per message alone.

A fact found while drafting, which shapes the comparison: QuickFIX's file store, configured as
its documentation describes, never forces data to disk. Run as a black box with a shim counting
its calls (`LD_PRELOAD`), its `executor` example made 0 `fsync`, 0 `fdatasync` and 0 `msync`
calls, and 1,626 `fflush` calls, for 200 orders and their reports. Its documentation lists no
setting to sync. Its store therefore survives the process crashing (the page cache holds the
data) but not power loss: the guarantee of our journal C (note 004), not of our default A, which
makes each input durable with `fdatasync` before anything it caused leaves.

It does:

- order-to-ExecutionReport latency, application to application, open loop, ours and
  QuickFIX's, against the same venue process, at the rates note 005 used;
- the comparison at matching guarantees (D3), and ours at our default beside it;
- the gateway's own cost per message, in process, broken down: framing and decoding, the
  session's checks, the dictionary's checks (note 006), the order book (note 007), the journal;
  QuickFIX's parsing and validation of the same messages beside them;
- every run's conditions recorded as `lockfree-ipc`'s harness records them; figures published
  through `run.py report`, ranges over at least three runs.

It does not:

- measure wire to wire: the machine has no capture hardware; application to application includes
  the kernel's network path on both sides, which note 005 measured alone and states;
- tune QuickFIX beyond its documented settings, or read its source to do so (note 001's rule).

## Design

**The path measured.** An application thread submits a NewOrderSingle at its scheduled time
(open loop: latency counts from the schedule, so a stall shows), the gateway sends it, the venue
answers with an ExecutionReport (New), the gateway delivers it, and the application stamps its
arrival. The same TSC clock (`lockfree-ipc`'s) on the same CPU in both cases. Ours: the
application on the gateway's outbound ring, as `net_bench` does. QuickFIX's: a program of ours
written to its documented API (`bench/qf_bench.cpp`), stamping before `Session::sendToTarget` and
in `fromApp`, linking `lockfree-ipc`'s harness for the clock, histogram and environment.

**The venue.** One for both: our venue simulator (note 007) behind a plain acceptor session, in
its own process, pinned, busy-polling, answering each order at once; over loopback. Its own
turnaround (message in to report out) is measured inside it and reported, so the gateways'
shares can be read apart from it.

```mermaid
flowchart LR
  A[application] -->|ring or API call| G[gateway: ours or QuickFIX]
  G -->|TCP loopback| V[venue process: session + simulator]
  V -->|ExecutionReport| G
  G -->|ring or fromApp| A
```

**Decisions for agreement:**

- **D1. Where the clock starts and stops.** (a) *Proposed:* application to application, as
  above: what a strategy sees, and the same two points for both gateways. (b) Gateway in to
  gateway out (the order's bytes leaving to the report's bytes arriving): nearer the gateway's own
  cost, but QuickFIX exposes no such points without reading or instrumenting its code. The
  per-message cost (D6) covers the gateway's own share.

- **D2. The venue.** (a) *Proposed:* ours, the same process for both, so any difference is the
  gateways'. (b) QuickFIX's `executor` for both: unpinned and slower, its turnaround would
  dominate.

- **D3. Matching guarantees.** (a) *Proposed:* published at two levels. *Survives a process
  crash:* ours with journal C (mapped, no sync) against QuickFIX's file store as documented.
  *Survives power loss:* ours at its default (the pipeline, `fdatasync`); QuickFIX has no
  documented setting for it, which the table says rather than leaving the row empty without a
  reason. Also, for reference, both with nothing persisted (ours with no journal is not a
  supported configuration, so this row is QuickFIX's `PersistMessages=N` only, stated as such).
  (b) Only the like-for-like row: simpler, but hides what our default costs.

- **D4. Threads and waiting.** Ours: the pipeline's three stages and the application each on a
  reserved CPU of node 0, busy-polling (note 005's default), and the direct loop with blocking
  waits beside it (how QuickFIX waits is its own and not documented; its documented
  `SocketNodelay=Y` is set). QuickFIX's threads cannot be pinned one by one without its code: its
  process is confined to as many reserved CPUs of node 0 as it has threads (its count read from
  `/proc` during the run and recorded). (a) *Proposed:* as stated. (b) Ours confined the same way
  instead of pinned: equal treatment, but not how ours is meant to run.

- **D5. Rates and samples.** 1,000 and 10,000 orders a second (note 005's), and the highest
  sustained rate each reaches (ours is bound by the journal at its default: two `fdatasync` calls
  an order, note 005). 100,000 orders per point after warm-up. Loopback only: the card adds the
  same ~31 µs to both (note 005) and is not repeated.

- **D6. The gateway's cost per message, alone.** In process, no network: (a) *proposed:* ours
  broken down by stage, each measured as the difference from the stage before (framing and
  decoding; plus the session's header and sequence checks; plus the dictionary; plus the order
  book; plus the journal, C and A), on an ExecutionReport in and a NewOrderSingle out, with the
  harness's per-call method (note 002's); QuickFIX's parsing of the same ExecutionReport with its
  FIX 4.4 data dictionary validating (its documented `Message` constructor), as the reference
  for the codec and checks together. (b) Ours only.

## Spec

- S1. Order-to-ExecutionReport latency, p50, p99, p99.9 and maximum, ranges over at least three
  runs, published through `run.py report` beside QuickFIX's at the same rates and guarantees
  (note 001, S5); where ours loses, the table shows it.
- S2. Every run records the machine's conditions (`lockfree-ipc`'s environment block), the CPUs
  of every thread (QuickFIX's process mask and thread count), the wait mode, the durability, and
  the venue's own turnaround; a point is published only if complete.
- S3. The gateway's cost per message alone, by stage (D6), published; the dictionary's cost (note
  006) and the order book's (note 007) each stated in ns per message.
- S4. QuickFIX's store behaviour stated in RESULTS from observation (sync calls counted per
  message, the shim's method published), and the guarantee each row gives.
- S5. Open loop throughout: latency from the scheduled time; a point at a rate its gateway cannot
  sustain is reported as not sustained, not as a latency.

## Tasks

- [x] Shep: agree D1–D6
- [ ] `net_bench`'s order bodies made valid FIX 4.4 (they lack TransactTime, 60, and note 007's
      outbound check now refuses them)
- [ ] The venue process (session and simulator), its turnaround measured
- [ ] `bench/qf_bench.cpp` (QuickFIX, to its API) and ours on the same schedule
- [ ] Per-message cost by stage (D6), ours and QuickFIX's parse
- [ ] `run.py` sets and README blocks; check runs, then the published set (Shep's bench-mode runs)

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-24 | Draft | Written for review. QuickFIX's store observed: `executor` under an `LD_PRELOAD` shim counting `fsync`, `fdatasync`, `msync` and `fflush`: 0, 0, 0 and 1,626 for 200 orders and their reports (config as documented, `PersistMessages` default Y per its shipped `doc/html/configuration.html`; the web page says N, the shipped copy Y); no sync setting documented. `net_bench`'s order bodies found invalid under note 007's outbound check (no TransactTime): fixed in this note's first task |
| 2026-10-24 | Agreed | Approved by Shep, every decision as proposed (option A each): D1 application to application; D2 our venue simulator in its own process for both, its turnaround reported; D3 two guarantee levels (process crash: ours C against QuickFIX's file store; power loss: ours A, QuickFIX's row stating it has no documented option) and QuickFIX with `PersistMessages=N` for reference; D4 ours pinned per thread, QuickFIX's process confined to as many reserved CPUs as it has threads; D5 1,000 and 10,000 a second and the highest sustained, 100,000 orders a point, loopback; D6 our cost per message by stage beside QuickFIX's parse and validation |
