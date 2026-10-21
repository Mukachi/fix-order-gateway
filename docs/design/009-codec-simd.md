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
- [ ] Differential fuzz target; S2
- [ ] Benchmark cases; three bench-mode runs; review

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-21 | Draft | Written for review |
| 2026-10-21 | Agreed | Approved by Shep: two measured steps (SIMD scanning; two-stage indexing, single message and burst); `x86-64-v3` at compile time, scalar elsewhere; 64-byte padded receive buffer, scalar tail when unpadded; correctness by equivalence, differential fuzzing; encoding and AVX-512 out of scope; three runs published beside note 002's |
| 2026-10-21 | Built (partial) | Step 1. `checksum_simd`: `vpsadbw` against zero over whole 32-byte blocks, the rest a byte at a time, so it never reads past its length and needs no padding. `frame_with<Simd>`: the framer with either CheckSum; `frame_scalar`, `frame_simd`, and `frame` (SIMD where the build has it). `scan::simd<Padded>`: the next SOH 32 bytes at a time (`vpcmpeqb`, `vpmovmskb`, `tzcnt`); padded, every load is whole and bits past the end are masked; unpadded, whole blocks then a byte at a time. The decoder takes the scan as a parameter, `scan::best` by default. GCC already auto-vectorises the plain CheckSum loop, so the comparison measured is plain code as the compiler builds it against hand-written SIMD. S6 check extended to every implementation (each behind its own `flatten` symbol, so the check sees the inlined path): no calls but the stack protector's abort. Tests: every framing and decoding check runs against scalar, SIMD and SIMD-padded; the SIMD CheckSum equals the scalar one at every length 0–300 and alignment 0–31; S3, at 64 consecutive message lengths, SIMD reads nothing past an exact-size allocation and the padded path nothing past its 32 bytes (ASan), SOH bytes past the end ignored; 100,000 mutated messages framed and decoded identically. Clean under ASan and UBSan; the aarch64 build compiles the scalar paths |
| 2026-10-21 | Built (partial) | Step 2. The per-field rules (header, body and trailer order, repeated header tags, data fields, the trailer last) moved into one `detail::field_rules`, used by every decoder, so the decoders differ only in how they find `=` and SOH and agree by construction. A side effect for the record: the scalar decoder now reads a tag's digits twice (to find `=`, then to convert), so it is not byte for byte note 002's; note 009's runs measure every path in one build. `structural_index<Words>`: stage one writes bitmaps of `=` and SOH, 64 bytes per step (two 32-byte compares), padded or with a byte-at-a-time tail; `next()` finds the first set bit at or after a position, capped at the message's end. `decode_indexed`: stage two over a message at any offset in an indexed buffer; the tag's digits are still read, to validate and convert. Tests: every decoding check also runs through the index, unpadded and padded; the bounds test and the 100,000 mutations include it; one index over 1,000 coalesced messages of three kinds decodes each as the scalar decoder does. Mutants: the bit search skipping its start position is caught; dropping bit 63 of the byte-at-a-time tail is caught on a build without AVX2 (the path aarch64 takes; on AVX2 builds that loop never reaches bit 63); `>` for `>=` at a word boundary, and the padded build not masking bits past the end, turned out equivalent, since every lookup is capped at the end. S6 (compiled code): index build 75–103 instructions, no calls; indexed decode no calls but the stack protector's abort. Clean under ASan and UBSan; the aarch64 build compiles |
