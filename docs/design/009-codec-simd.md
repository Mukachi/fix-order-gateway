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

- [ ] `x86-64-v3` in the build (scalar elsewhere); padded receive-buffer contract
- [ ] Step 1: SIMD framer and SOH scan; equivalence tests; S3
- [ ] Step 2: two-stage index decoder, single message and burst; equivalence tests
- [ ] Differential fuzz target; S2
- [ ] Benchmark cases; three bench-mode runs; review

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-21 | Draft | Written for review |
| 2026-10-21 | Agreed | Approved by Shep: two measured steps (SIMD scanning; two-stage indexing, single message and burst); `x86-64-v3` at compile time, scalar elsewhere; 64-byte padded receive buffer, scalar tail when unpadded; correctness by equivalence, differential fuzzing; encoding and AVX-512 out of scope; three runs published beside note 002's |
