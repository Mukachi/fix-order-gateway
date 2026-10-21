# 002 — Codec

**Status:** Agreed · **Updated:** 2026-10-21

## Proposal

Turns bytes from a socket into FIX 4.4 messages, and messages into bytes. Everything above it (the
session, the order layer) sees whole, checked messages; everything below it (the transport) sees
bytes. It is pure (note 001): no sockets, clocks, threads or allocation.

The hard parts are not the tag=value syntax but the edges: a read that ends mid-message, a read
that holds three messages, input that is not FIX at all, and telling apart a message the spec
says to *ignore* (garbled) from one it says to *reject* (well-formed but invalid). And, measured:
how messages are represented costs more than parsing them, so three representations are built and
compared.

It does:

- frame a byte stream into messages, across partial and coalesced reads;
- validate the frame: BeginString, BodyLength, CheckSum, and the order of the first three fields;
- classify what it cannot accept: garbled (to be ignored, the session's sequence number untouched)
  or structurally invalid (to be rejected, with the SessionRejectReason the spec gives);
- give the fields of a message without copying them, in order, repeated tags included;
- encode messages with BodyLength and CheckSum computed;
- measure three representations (below) for decoding and encoding.

It does not:

- know message types or field types beyond the header and trailer. Whether a field's value is a
  valid price or a required field is present belongs to the typed layers above (notes 003, 007),
  which call the codec's accessors;
- interpret repeating groups. Their fields are returned in order; a message type that needs
  groups parses them above. None of the messages in scope (note 001) needs one on the hot path.

## Design

**Framing.** A FIX 4.4 message is `8=FIX.4.4|9=<len>|35=<type>|...|10=<sum>|`, where `|` is SOH
(0x01). BodyLength counts the bytes from after the SOH that ends field 9 up to and including the
SOH before `10=`; CheckSum is the sum of every byte before `10=`, modulo 256, as three digits. So a
frame's length is known once field 9 is read: the framer looks for `8=` at the start of the
unconsumed bytes, reads 9, waits until `prefix + BodyLength + 7` bytes are present, then checks
`10=ddd|` at the expected place. It never scans a body for SOH to find the end, so a body cannot
fool it.

**The receive buffer** is a fixed region the transport reads into; the framer reports how many
bytes it consumed; leftover bytes (the start of the next message) are moved to the front only when
the free space at the end runs short. No allocation; the buffer's size bounds the largest message,
and a frame longer than that is garbled.

**Garbled against invalid** (FIX Session Layer Technical Standard):

| Fault | Class | Effect |
|---|---|---|
| does not start with `8=FIX.4.4`, or 9 or 35 not second and third | garbled | discard to the next `8=`; ignore |
| BodyLength not a number, or `10=` not where it says | garbled | as above |
| CheckSum wrong | garbled | ignore the message |
| tag not a positive integer, `=` missing, value empty | invalid | Reject, SessionRejectReason 0 or 4 |
| a header or trailer tag out of place, or repeated | invalid | Reject, reason 13 or 14 |

The codec only classifies; the session decides what to send (note 003).

**Data fields.** A few fields carry raw bytes that may contain SOH, preceded by a length field:
SecureDataLen 90 / SecureData 91 and XmlDataLen 212 / XmlData 213 in the header, SignatureLength
93 / Signature 89 in the trailer, RawDataLength 95 / RawData 96 in Logon. The codec knows these
pairs and reads their values by length, not to the next SOH.

**Three representations, measured** (decode a message, read the fields an order path reads; encode
a NewOrderSingle):

| | Decode | Encode |
|---|---|---|
| A. Fields copied into a map | each field copied into a hash map from tag to string | build a map, then serialise it |
| B. Views over the buffer | a fixed array of (tag, offset, length), filled by one pass; values read in place | append fields to a fixed buffer |
| C. Pre-encoded templates | as B | the message is laid out once per session; only sequence number, sending time, ClOrdID, price, quantity, BodyLength and CheckSum are written per order |

A is the conventional design and the baseline: it allocates by nature, so it is measured, not
shipped. B is the default decoder. C is expected to win encoding by not re-emitting fields that
never change, at the price of fixed-width fields (integers with leading zeros, which FIX permits)
so the layout does not move. A counterparty that mishandles leading zeros would break it, so C is
also run against QuickFIX's acceptor (note 006). The measurements decide.

**Measurement.** Per-call latency of decode and encode, as percentiles (`lockfree-ipc`'s clock and
histogram, fetched per note 001), pinned to one reserved CPU, warm-up discarded, over a corpus of
realistic messages (NewOrderSingle, ExecutionReport, Heartbeat), plus throughput in messages per
second over a buffer of coalesced messages.

**Fuzzing.** The framer and decoder are the code that faces untrusted input. Two options:

- *libFuzzer* (coverage-guided): needs clang, not installed; in the Ubuntu 26.04 archive as
  clang 21. Finds deep paths quickly.
- *A mutation fuzzer of our own* under GCC with ASan and UBSan: no new tools, but blind to
  coverage, so far weaker.

Recommended: install clang for libFuzzer, used for fuzzing only (the gateway stays built and
measured with GCC).

## Spec

- S1. Any valid stream, split into reads at every possible point (byte by byte included) and
  coalesced arbitrarily, yields the same messages in the same order.
- S2. Each garbled fault in the table is detected and classified as garbled; framing recovers at
  the next message.
- S3. Each invalid fault in the table is classified with its SessionRejectReason.
- S4. Data fields containing SOH are read whole, by their length.
- S5. Decoding an encoded message gives back the same fields; encoded BodyLength and CheckSum are
  correct (checked against an independent calculation in the test).
- S6. Decoding (B) and encoding (B and C) make no allocation, call or system call on the hot path,
  checked in the compiled code.
- S7. Under the fuzzer, no crash, sanitizer report or read outside the buffer, for a stated number
  of executions; the corpus and any findings are committed.
- S8. A, B and C are measured with `lockfree-ipc`'s harness, at least three runs, published.

## Tasks

- [x] CMake skeleton; `lockfree-ipc` fetched at a pinned commit; warnings as errors; CTest
- [x] Framer and B decoder, S1–S4 tests (S5 with the encoders)
- [x] Encoders B and C; S5, S6
- [x] Representation A, for measurement only
- [x] Fuzzer (per the decision above); S7
- [ ] Codec benchmark; three bench-mode runs; S8; review

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-21 | Draft | Written for review |
| 2026-10-21 | Agreed | Approved by Shep: three representations measured (A map baseline, B views as the default decoder, C pre-encoded templates, C also run against QuickFIX's acceptor); the codec stays type-blind, groups returned uninterpreted; libFuzzer with clang for fuzzing only, GCC for the build and measurements |
| 2026-10-21 | Built (partial) | Skeleton: `lockfree-ipc` fetched at `efbccdc` (headers only; its own project not added, so no Boost or baselines here); its provenance script run against this repo, so a run records the gateway's commit, with the harness commit beside it (`FIXGW_LFIPC_COMMIT`). `framing.hpp`: `frame(p, n, max_frame)` reads BeginString and BodyLength, then checks MsgType third and `10=ddd` SOH exactly where BodyLength puts it, with the right sum; a body is never scanned for its end. Garbled input is skipped to the next possible `8=FIX.4.4` SOH `9=`. `decode.hpp`: one pass to (tag, offset, length) views; header, body and trailer order; header tags not repeated (the NoHops group excepted); data fields (90/91, 212/213, 93/89, 95/96) read by length. Tests: S1 every two-read split of three messages, byte by byte, and 2,000 random splits of six; S2 each garbled fault (wrong BeginString, BodyLength not a number or empty, MsgType not third, CheckSum wrong, BodyLength too short or too long, non-FIX bytes, a frame longer than the buffer), each followed by the next message framed; S3 each structural fault with its SessionRejectReason and RefTagID; S4 RawData containing SOH read whole. Found while testing: a data length can reach over the trailer the framer found, leaving a message with no CheckSum field; the decoder now requires the trailer to be the last field (test added). Clean under ASan and UBSan |
| 2026-10-21 | Built (partial) | `encode.hpp`. Encoder B: fields appended after room for the longest prefix; `finish` writes BeginString and BodyLength right before the body, then the CheckSum; overflow is a flag, never a write past the buffer. Template C (`new_order_template`): a NewOrderSingle laid out once through B with fixed-width slots (MsgSeqNum 9 digits, ClOrdID 16 digits, OrderQty 9, Price 12 characters, SendingTime and TransactTime 21), so BodyLength is constant and the prefix written once; per order only the slots and the CheckSum are written, the CheckSum being the constant bytes' sum plus the slots'. A value too wide for its slot is refused, never truncated. `format_utc_timestamp`: FIX 4.4's UTCTimestamp allows whole seconds or milliseconds only, so milliseconds (the test helper had used microseconds; corrected); days to date by Howard Hinnant's public algorithm, no library call. Tests: B's NewOrderSingle is byte-identical to the test helper's independently framed one; encode, frame and decode round trips; overflow; decimals; the timestamp against `gmtime_r` at the edges (1970, leap days 2000 and 2024, 2100, a year end) and at a million random instants; C over 100,000 random fills, each framing and decoding with an independently checked CheckSum, its fields back and its length constant. S6 in the compiled code (`check_hot_path.py`): framer, decoder and C make no call at all; B calls only `memcpy` (values of variable length) and the stack protector's abort; no subject contains a syscall instruction. S6's "no call" is read as that: no allocation, no system call, `memcpy` allowed. A mutant decoder calling `malloc` fails the check. Clean under ASan and UBSan |
| 2026-10-21 | Built (partial) | Representation A in `bench/representation_a.hpp` (measurement only, outside the library): decoding copies each field into an `unordered_map` from tag to `std::string`; encoding collects (tag, string) pairs and serialises them into a new string. Written from the FIX rules for this comparison, not from any engine's source. `representation_a_test`: A and B decode the same fields and values, and A encodes the same bytes as B, so the measurement compares equals |
| 2026-10-21 | Built (partial) | S7: `fuzz/fuzz_codec.cpp` (libFuzzer, clang 21.1.8 from the Ubuntu archive, ASan and UBSan, built only with `-DFIXGW_FUZZ=ON` in its own build directory; the gateway itself stays GCC). Input is a byte stream framed as the transport frames it, each accepted frame decoded; besides the sanitizers it aborts if a frame claims more bytes than it was given, a garbled verdict discards nothing, a decoded field lies outside its frame, or an accepted message does not end in CheckSum. Six hand-written seeds (NewOrderSingle, Heartbeat, Logon with RawData containing SOH, NoHops group, Signature, three coalesced). Run: 12,585,069 executions in 601 s, no crash, no sanitizer report, no invariant broken; 216 coverage edges. Corpus minimised by libFuzzer's merge to 730 inputs (3.2 MB), committed under `fuzz/corpus` beside the seeds |
