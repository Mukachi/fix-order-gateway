# 009 — Codec: SIMD scanning and two-stage indexing

**Status:** Agreed · **Updated:** 2026-10-21

## Proposal

Note 002 measured the codec's first implementation: framing and decoding a NewOrderSingle (B)
costs ~185 ns net of the clock, about 1.1 ns a byte, most of it in two byte-at-a-time loops: the
CheckSum sum over the whole message, and the scan for each field's SOH. This note replaces them in
two measured steps, each kept beside the one before, as `lockfree-ipc` did with its queue variants.

1. **SIMD scanning.** The CheckSum summed 32 bytes per instruction (`vpsadbw` against zero), and the
   next SOH found 32 bytes at a time (`vpcmpeqb`, `vpmovmskb`, `tzcnt`) instead of byte by byte.
   The decoder's structure is unchanged; only its inner loops are.
2. **Two-stage indexing** (the technique of simdjson, and of vectorised engines like DuckDB). Stage
   one passes over the bytes once, 64 at a time, and writes bitmaps of where every `=` and SOH
   is; stage two walks those bits (`tzcnt`, `blsr`) to produce the fields, with no per-byte
   branches. Stage one can run over a whole burst of coalesced messages at once.

The expectation, to be tested: step 1 helps every message; step 2 helps bursts (the index is
built once for many messages) and may or may not help a single small message on the latency path,
where building the index is a fixed cost. Both are measured on both.

It does:

- add the two decoders and a SIMD framer beside the scalar ones, same interface, same verdicts;
- prove each equal to the scalar one on every test and under a differential fuzzer;
- measure all three on single messages and on bursts, on the note 002 harness.

It does not:

- change encoding. Template C's own costs (reformatting the whole timestamp per order, digits by
  repeated division, re-summing slots for the CheckSum) are a separate step;
- use AVX-512: Broadwell has none. The design keeps it open (wider masks, same stages).

## Design

**Instruction set: `x86-64-v3`, chosen at compile time.** That level is AVX2, BMI1/2, LZCNT,
MOVBE and FMA: everything here, on Broadwell and every x86-64 CPU since Haswell. The alternatives:

- *Run-time dispatch* (detect the CPU, pick a function): portable binaries, but an indirect call or
  a branch per message, and every variant compiled side by side;
- *`-march=native`*: the build depends on the machine that compiled it, and the runs record a
  target nobody else can name.

`x86-64-v3` is a named, documented level; the build records it in its flags (`lockfree-ipc` note
004 captures them), and CI's x86-64 runners support it. Elsewhere (aarch64 in CI) the scalar code
is used: same results, checked by the same tests.

**Reading past the end.** A 32-byte load near the end of a message can read bytes beyond it, and
beyond the buffer. The receive buffer is given 64 bytes of padding past its capacity, so a vector
load that starts inside the buffer never crosses into an unmapped page; bytes past the frame are
masked off and never affect a result. Where the caller cannot pad (a decoder handed an arbitrary
view), the last partial block is read scalar. Both paths are tested, under ASan, which sees a read
past an allocation even when the page is mapped.

**Equivalence is the correctness test.** Each new path must give exactly the scalar path's result:
the same frame verdicts and lengths, the same fields (tag, offset, length), the same reject reason
and RefTagID. Every existing test runs against each path; a second fuzz target feeds the same input
to the scalar and SIMD paths and aborts on any difference.

**Measurement.** `codec_bench` gains cases for each path: frame alone, frame and decode, on
NewOrderSingle, ExecutionReport and Heartbeat; and throughput over 100,000 coalesced messages per
path, where two-stage indexing builds its index over the whole burst. Three bench-mode runs,
published beside note 002's. Note 002's open item (ExecutionReport decoding bimodal across runs) is
checked again in these runs.

## Spec

- S1. The SIMD framer gives the scalar framer's verdict and length on every framing test and every
  fuzz input.
- S2. Each new decoder gives the scalar decoder's fields, verdict, reject reason and RefTagID on
  every decoding test and every fuzz input (differential fuzzing, a stated number of executions).
- S3. No read outside the buffer: ASan-clean with and without padding, including messages ending
  at every offset within a 32-byte block.
- S4. The new paths make no allocation, call or system call (note 002's compiled-code check, extended).
- S5. aarch64 builds and passes with the scalar paths (CI).
- S6. All three decoders and both framers measured, single messages and bursts, three runs,
  published; each step's gain or loss stated against the one before.

## Tasks

- [x] `x86-64-v3` in the build (scalar elsewhere); padded receive-buffer contract
- [x] Step 1: SIMD framer and SOH scan; equivalence tests; S3
- [x] Step 2: two-stage index decoder, single message and burst; equivalence tests
- [x] Differential fuzz target; S2
- [ ] Benchmark cases; three bench-mode runs; review

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-21 | Draft | Written for review |
| 2026-10-21 | Agreed | Approved by Shep: two measured steps (SIMD scanning; two-stage indexing, single message and burst); `x86-64-v3` at compile time, scalar elsewhere; 64-byte padded receive buffer, scalar tail when unpadded; correctness by equivalence, differential fuzzing; encoding and AVX-512 out of scope; three runs published beside note 002's |
| 2026-10-21 | Built (partial) | Step 1. `checksum_simd`: `vpsadbw` against zero over whole 32-byte blocks, the rest a byte at a time, so it never reads past its length and needs no padding. `frame_with<Simd>`: the framer with either CheckSum; `frame_scalar`, `frame_simd`, and `frame` (SIMD where the build has it). `scan::simd<Padded>`: the next SOH 32 bytes at a time (`vpcmpeqb`, `vpmovmskb`, `tzcnt`); padded, every load is whole and bits past the end are masked; unpadded, whole blocks then a byte at a time. The decoder takes the scan as a parameter, `scan::best` by default. GCC already auto-vectorises the plain CheckSum loop, so the comparison measured is plain code as the compiler builds it against hand-written SIMD. S6 check extended to every implementation (each behind its own `flatten` symbol, so the check sees the inlined path): no calls but the stack protector's abort. Tests: every framing and decoding check runs against scalar, SIMD and SIMD-padded; the SIMD CheckSum equals the scalar one at every length 0–300 and alignment 0–31; S3, at 64 consecutive message lengths, SIMD reads nothing past an exact-size allocation and the padded path nothing past its 32 bytes (ASan), SOH bytes past the end ignored; 100,000 mutated messages framed and decoded identically. Clean under ASan and UBSan; the aarch64 build compiles the scalar paths |
| 2026-10-21 | Built (partial) | Step 2. The per-field rules (header, body and trailer order, repeated header tags, data fields, the trailer last) moved into one `detail::field_rules`, used by every decoder, so the decoders differ only in how they find `=` and SOH and agree by construction. A side effect for the record: the scalar decoder now reads a tag's digits twice (to find `=`, then to convert), so it is not byte for byte note 002's; note 009's runs measure every path in one build. `structural_index<Words>`: stage one writes bitmaps of `=` and SOH, 64 bytes per step (two 32-byte compares), padded or with a byte-at-a-time tail; `next()` finds the first set bit at or after a position, capped at the message's end. `decode_indexed`: stage two over a message at any offset in an indexed buffer; the tag's digits are still read, to validate and convert. Tests: every decoding check also runs through the index, unpadded and padded; the bounds test and the 100,000 mutations include it; one index over 1,000 coalesced messages of three kinds decodes each as the scalar decoder does. Mutants: the bit search skipping its start position is caught; dropping bit 63 of the byte-at-a-time tail is caught on a build without AVX2 (the path aarch64 takes; on AVX2 builds that loop never reaches bit 63); `>` for `>=` at a word boundary, and the padded build not masking bits past the end, turned out equivalent, since every lookup is capped at the end. S6 (compiled code): index build 75–103 instructions, no calls; indexed decode no calls but the stack protector's abort. Clean under ASan and UBSan; the aarch64 build compiles |
| 2026-10-21 | Built | `fuzz/fuzz_diff.cpp`: every input decoded whole by the scalar, SIMD, SIMD-padded, indexed and indexed-padded decoders (the unpadded ones reading the input in place, so ASan sees any over-read), then framed as a stream by both framers, each frame decoded from one shared index; any disagreement with the scalar path aborts. Run: 6,084,521 executions in 601 s, no disagreement, no sanitizer report; 454 coverage edges. The inputs that add coverage merged into `fuzz/corpus`. Benchmark: frame alone and frame and decode for each path on NewOrderSingle, ExecutionReport and Heartbeat, each message in its own 512-byte slot; throughput per path over 100,000 messages in batches of whole messages up to 16 KB, two-stage indexing each batch inside the timing. A first version indexed the whole 16 MB stream at once, which measures the memory system rather than the method (a 4 MB index and the stream both far beyond the caches); batched as a socket read would deliver them instead. Off-fence smoke runs (ratios only): SIMD halves framing; decoding gains less, because scanning was never the main cost: about two-thirds of decoding goes to the per-field rules (classifying each tag); two-stage is slower than SIMD on single messages, as expected, and also on batches, contrary to the Draft's expectation. The likely reason: FIX fields are short (~10 bytes), so the SIMD scan finds the next SOH in its first 32-byte load almost every time, and the index has nothing to amortise. The bench-mode runs decide. All tasks but the reviewed runs done |
| 2026-10-21 | Built | **First measured runs not published: they exposed three faults of mine.** `results/codec-dev-lnx01-20261021T212032Z.json`, `...212057Z.json`, `...212121Z.json` (kept) put scalar frame and decode of a NewOrderSingle at 642–665 ns, against 203–213 ns for note 002's decoder: a 3× regression in the baseline path. Bisected with small probes (same inputs, same CPU, note 002's headers beside the current ones): (1) `x86-64-v3` let GCC auto-vectorise the short byte loops (tag digits, the scalar SOH scan, BodyLength), which costs: note 002's decoder 424 ticks on `x86-64`, 478 on `x86-64-v3`, 396 with `-fno-tree-vectorize`. Fixed with `FIXGW_NOVECTOR` (`#pragma GCC novector`, GCC 14 and later) on those loops; the whole-build target stays, as agreed. (2) The shared `field_rules::add` was not inlined: a real call per field, and the field count kept in memory. Fixed: the hot-path functions are `always_inline`, tags are read and converted in one pass again, the count is a local. The S6 check had hidden this: its subjects were marked `flatten`, which forces inlining; that was a check made to pass, not to tell the truth. `flatten` is removed, and every path still passes S6 as an ordinary build compiles it. (3) The benchmark inlined each decoder into its timing loop, where the same code's cost moved by 1.5× with unrelated code elsewhere in the file (adding two unused functions moved it from ~620 to ~420 ticks; inputs and code alignment ruled out). Every measured operation is now called through its own non-inlined entry point, as the session will call it; one call (~1–2 ns) is included. After the fixes, in the probe (off the fence, full turbo, ticks): frame and decode scalar 418–420 (note 002's decoder 426), SIMD 386, SIMD padded 354–374, two-stage 468. Off-fence `codec_bench` ratios are not comparable with the probes: its calibration sleeps leave tickless CPU 2 at a low frequency, which inflates decode against the fence-bound empty window; only bench-mode runs decide |
| 2026-10-21 | Built | **Second set not published: two speeds per case, found to be the branch predictor memorising the benchmark.** `results/codec-dev-lnx01-20261021T222811Z.json`, `...222834Z.json`, `...222857Z.json` (kept) agreed on the ranking within each run but split between runs (scalar NewOrderSingle 518–519 against 368 ns). Narrowed in steps, each in bench mode where needed: (a) not address-space layout (randomisation off, same layout, still split; six-run diagnostic); (b) the modes belong to individual cases, decided as each starts and held for its whole million samples; (c) not the L2 or 4K aliasing of the sample array (off the fence, every page offset the same); (d) not frequency: `turbostat` shows CPU 2 at 2,800 MHz throughout, the package at 65–72 W of 145 W (an earlier best-of probe, reading 2.09 GHz, included the clock's own overhead and was wrong; the machine is not power-limited); (e) not the hyper-thread sibling: CPU 46 in C1 99.99%, POLL 0.00%, with POLL enabled or disabled. The clue: in the slow mode a pure 500-add chain, alternated with decoding, also lost ~18 ticks, one branch misprediction. The cause: the benchmark visited its 1,024 inputs in the same cycle a million times, and the predictor sometimes memorised it (fast mode) and sometimes did not. Off the fence, cyclic order reproduces the modes (scalar 525, 741, 519 ticks over three repeats); random order removes them (756, 756, 758) at the slow mode's level. The fast mode is an artefact; real order flow does not repeat in a fixed cycle. The benchmark now visits inputs in a fixed random order (read outside the timed window), and the burst is a random sequence. Off the fence, random order: frame and decode scalar ~757 ticks, SIMD ~611, SIMD padded 571, two-stage ~776 |
| 2026-10-22 | Built | **Third set not published: the cause was the machine setup.** `results/codec-dev-lnx01-20261021T225648Z.json`, `...225710Z.json`, `...225733Z.json` (kept), taken with random order, still split per case in bench mode while off the fence they were steady. Counters per case (`perf stat -I 100` on CPU 2, aligned with each case's timestamps): fast cases 0.0–0.1 branch mispredictions per 1,000 instructions at IPC ~2.9, slow cases 3.6–5.5 at IPC ~2.2; machine clears, 4K aliasing and L1 refills flat; front-end delivery barely different. With realistic input variety (below), still two speeds, now first fast then slow a few seconds in (mispredictions 2.8 → 9.4 per 1,000 for scalar), with no timer interrupt on CPU 2 at any point. Bisecting `bench-on`, each setting alone off the fence: only the idle-state cap at C1 reproduced it (scalar 232–241 ns uncapped, 351–353 capped); governor, pinned uncore and NUMA balancing alone did not. Likely mechanism (inferred): an idle hyper-thread sibling in C1 keeps its share of the core's partitioned resources, branch prediction among them; in a deep state it gives them up. `bench-on` now leaves every idle state on for the reserved CPUs and caps only the housekeeping ones, and `lockfree-ipc`'s check requires it (`lockfree-ipc` note 004, Review 2026-10-22; harness pinned at `561f393`). Also: the decode inputs now vary as real order flow does (symbols of 1–6 characters, ClOrdID 6–20, Account in 80% of orders, quantities of 1–7 digits, prices with 0–4 decimals or none for market orders, TimeInForce in 70%, Text in 10%; ExecutionReports and Heartbeats likewise), seeded; uniform messages made every branch outcome repeat. Encoding keeps its inputs (template C fixes symbol and account by design) |
