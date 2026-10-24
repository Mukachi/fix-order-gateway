# 008 — Measurement

**Status:** Built · **Updated:** 2026-10-24

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
- [x] `net_bench`'s order bodies made valid FIX 4.4 (they lack TransactTime, 60, and note 007's
      outbound check now refuses them)
- [x] The venue process (session and simulator), its turnaround measured
- [x] `bench/qf_bench.cpp` (QuickFIX, to its API) and ours on the same schedule
- [x] Per-message cost by stage (D6), ours and QuickFIX's parse
- [x] `run.py` sets and README blocks; check runs, then the published set (Shep's bench-mode runs)

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-24 | Draft | Written for review. QuickFIX's store observed: `executor` under an `LD_PRELOAD` shim counting `fsync`, `fdatasync`, `msync` and `fflush`: 0, 0, 0 and 1,626 for 200 orders and their reports (config as documented, `PersistMessages` default Y per its shipped `doc/html/configuration.html`; the web page says N, the shipped copy Y); no sync setting documented. `net_bench`'s order bodies found invalid under note 007's outbound check (no TransactTime): fixed in this note's first task |
| 2026-10-24 | Agreed | Approved by Shep, every decision as proposed (option A each): D1 application to application; D2 our venue simulator in its own process for both, its turnaround reported; D3 two guarantee levels (process crash: ours C against QuickFIX's file store; power loss: ours A, QuickFIX's row stating it has no documented option) and QuickFIX with `PersistMessages=N` for reference; D4 ours pinned per thread, QuickFIX's process confined to as many reserved CPUs as it has threads; D5 1,000 and 10,000 a second and the highest sustained, 100,000 orders a point, loopback; D6 our cost per message by stage beside QuickFIX's parse and validation |
| 2026-10-24 | Agreed (amendments) | **Check runs, bench mode** (scratchpad, `064fa52`, quick: 10,000 orders a point; not published). At 10,000/s, p50 and p99: ours, direct loop, journal C, busy-poll 24.8 and 33.0 µs; QuickFIX, `PersistMessages=N`, 31.3 and 34.3; QuickFIX's file store 45.6 and 52.0; ours, pipeline, journal A 146.3 and 208.7; ours, direct, A, blocking 341 µs and 58 ms (two `fdatasync` calls an order near its limit). At 1,000/s QuickFIX 89–102 µs p50 against ours (C) 25.6. Highest sustained: ours C 50,000/s, pipeline A 50,000, direct A 10,000; QuickFIX's file store 20,000, `PersistMessages=N` 50,000. The venue's own turnaround ~9.5 µs p50 in every point. Per message (p50): our framing and decoding 306 ns, the dictionary check 1,261, the order book inbound 285, the session's whole inbound path 1,691, the journaled session's inbound 2,205; outbound, the body checks 1,562, the book's check and record 283 (p99 3,617), the session's send 153, the journaled send 1,818; QuickFIX's parse and validation of the same ExecutionReport 2,059. Two harness faults found and fixed before these runs: the blocking direct loop was never rung (it slept out its 1 ms timeout: 686 µs p50 at 1,000/s, 226 once rung), and venue figures were read for the wrong point after a point that fell behind and reconnected. **Two amendments, agreed with Shep before the published set** (option A each; these figures stay here as the before): (1) the dictionary's lookups, two binary searches per field (a message's table, then the field table), replaced by tables indexed by tag number; (2) the order book's tables faulted in at construction (they were lazily mapped, so a first write to a page took a fault on the order path: the outbound p99), at the cost of their memory resident for the session's life |
| 2026-10-24 | Agreed (amendment) | **First published set** (`92817c9`, three runs each, all clean, bench mode, every point complete but the direct loop with A at 20,000/s): QuickFIX's and the per-stage sets kept (the per-stage set's two journaled rows measured C before its pre-fill: their tails, inbound p99.9 3.3 µs and send p99 4.1 µs, include C's page faults, so stated); ours superseded by the next entry. Before them, a set at `9367905` was discarded unpublished: every point's maximum (3–8 ms, ours and QuickFIX's alike) was the venue's own, its simulator moving all its orders when its vector grew past 65,000 (`92817c9` reserves its capacity; the venue's maximum then 86–316 µs). Two findings in the published set, decided with Shep (the recommendation each): (1) our journal C's p99.9 (123–126 µs at every rate, against QuickFIX's 40–61 µs at 10,000/s) traced to ext4 allocating blocks for C's sparse mapped file on the order path, one ~150 µs stall every 873–874 orders (latencies dumped and their spacing read; on tmpfs the stalls vanish, p99.9 43 µs; a pre-filled, faulted-in prototype on ext4 48 µs): C now pre-filled at creation (note 004 amended), and our order set re-measured; (2) the dictionary check's p50 differs between runs (786 ns in two, 1,316 in the third; every other stage within a few percent): published as a range, open, cause unknown (perhaps note 009's two predictor states; not measured) |
| 2026-10-24 | Measured (runs) | **Our order set re-measured** at `140d81b` (journal C pre-filled), three runs, clean, bench mode, published beside QuickFIX's and the per-stage sets of `92817c9`: `results/order-dev-lnx01-20261024T180036Z.json`, `180710Z`, `181345Z`. Journal C at 10,000/s: p50 18.4–19.2 µs, p99 30.9–31.8, p99.9 33.8–35.0, max 62.7–68.6 (before the pre-fill 18.2–18.7, 22.5–22.9, 123.4–124.3, 142.6–146.9); against QuickFIX at 10,000/s, `PersistMessages=N` 31.2–31.7, 37.6–39.1, 40.4–42.3, 45.6–48.1, its file store 46.5–48.1, 55.2–57.7, 58.9–61.5, 157.0–174.9: ours ahead at every percentile. At 1,000/s ours (C) 19.7–20.3 µs p50 against QuickFIX's 90.4–105.6. Highest sustained in every run: ours, C and the pipeline with A 100,000/s; QuickFIX's file store 20,000, `PersistMessages=N` 50,000; the direct loop with A, blocking, 1,000/s (one of three runs did not sustain 10,000: near its journal's limit, two `fdatasync` calls an order, it falls either side between runs; the first set sustained 10,000 in all three). **Open:** C's p99 rose ~8 µs with the pre-fill (22.7 to 31 µs at every rate), while p99.9 and the maximum fell; cause not measured (perhaps the write-protect fault on each clean page of the mapping's first write); a bench-mode diagnostic is next, agreed with Shep before the note goes to Measured |
| 2026-10-24 | Measured (diagnostic) | **C's p99 rise explained** (agreed with Shep: published first, then diagnosed). A diagnostic (scratchpad, bench mode, journal C at 10,000/s, 100,000 orders a variant, minor faults counted during the schedule): sparse (as before) p99 26.2 µs, p99.9 125.4, 4,581 faults; pre-filled and populated (as published) 32.4 and 36.5, 12,063 faults; again 32.2 and 35.5; **pre-filled, not populated 24.1 and 27.2, 4,410 faults**; sparse and populated 26.9 and 50.8, 12,063. The pre-fill removes the allocation stalls; `MAP_POPULATE` adds the ~8 µs at p99 and nearly triples the faults: populating a shared file mapping maps its pages read-only, so each first write still takes a write-protect fault, now on the order path, where without it the write faults the page in writable at once (already in the page cache from the pre-fill). C now pre-filled and not populated; our order set to be re-measured |
| 2026-10-24 | Built (runs) | **Our order set re-measured** at `137d08e` (journal C pre-filled, not populated), three runs, clean, bench mode: `results/order-dev-lnx01-20261024T185707Z.json`, `190352Z`, `191026Z`, replacing the `140d81b` runs in the manifest (those stay in `results/`, superseded). Journal C at 10,000/s: p50 18.4–20.9 µs, p99 22.7–25.7, p99.9 25.6–28.9, max 65.4–81.2; at 1,000/s p50 19.9–22.1, max 694–728 µs (one stall a point, not studied). Against QuickFIX at 10,000/s (`92817c9` runs): `PersistMessages=N` p50 31.2–31.7, p99 37.6–39.1, p99.9 40.4–42.3; file store 46.5–48.1, 55.2–57.7, 58.9–61.5. Highest rate sustained in every run: ours, the pipeline with A and the direct loop with C, 50,000/s (the previous set sustained 100,000 in every run: at the edge, either side between sets); the direct loop with A, blocking, 1,000/s; QuickFIX's file store 20,000, `PersistMessages=N` 50,000. Against the Spec: S1 (ours beside QuickFIX at matching guarantees, three runs each, through `run.py report`), S2 (every run's conditions and the venue's turnaround recorded; points published only if sustained), S3 (cost by stage, the dictionary 786–1,316 ns and the order book 299–305 inbound, 322–323 outbound, p50), S4 (QuickFIX's store observed: 0 `fsync`, 0 `fdatasync`, stated in RESULTS), S5 (open loop; unsustained points shown as such). Open: the dictionary check's run-to-run p50 spread; the per-stage set's journaled rows predate C's pre-fill; C's 1,000/s maximum |
| 2026-10-24 | Built (amendment) | **The venue limited ours at 100,000/s.** Every point of ours that missed 100,000/s (3 of 6 runs for the pipeline with A, 3 of 6 for C) had the venue's own turnaround p50 at 3.9–4.9 ms, against 9–45 µs where it held; at QuickFIX's failing points the venue stayed at 8.7–9.8 µs (QuickFIX's ceilings are its own). In process the venue costs ~3.5 µs an order (session ~1 µs, simulator ~2 µs) and sent each report in its own `send()` (~3.2 µs on loopback): ~7–10 µs an order against 10 µs between orders, and once behind it never caught up. Amendment (agreed): `order_venue` sends what one read produces in one `send()`. Check run (outside bench mode): the venue's p50 9–17 µs at every point to 100,000/s. Every order set (ours and QuickFIX's) to be re-measured with it |
