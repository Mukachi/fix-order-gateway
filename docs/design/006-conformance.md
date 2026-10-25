# 006 — Conformance

**Status:** Measured · **Updated:** 2026-10-25

## Proposal

Note 001 promised conformance shown case by case: every applicable case of the *FIX Session Layer
Test Cases* an automated test with its result listed, and the cases not implemented listed with
the reason (note 001, S1). Note 003 built the session to be scripted against them and tested one
case per scenario, from a published HTML copy; it left the remaining sub-cases, and a check
against the official document, to this note. Note 001 also asked for interoperability with
QuickFIX: our initiator against its acceptor and the reverse (note 001, S3).

The basis is now the official document: *FIX Session Layer Test Cases*, Version 1.0, June 2020,
FIX Protocol Limited (19 pages; free from fixtrading.org, the package "FIX Session Layer – Test
Cases"; CC BY-ND 4.0). Read in full for this note. Against the copy note 003 used, it differs in
ways that matter here:

- *Receive any message other than Logon* at the acceptor is a scenario of its own, **2S**, not
  part of 1S;
- **2r** (MsgType valid but not supported) answers with BusinessMessageReject, reason 3; **14m**
  (conditionally required field missing) with BusinessMessageReject, reason 5;
- **15** (messages with fields in different orders) is *Optional*;
- **14k** (a field containing SOH) allows either a Reject, reason 17, or treating the message as
  garbled;
- 2i and 12 wait *LogoutAckThreshold*, a profile value, where the copy said 2 s and 10 s.

It does:

- one scripted test per sub-case of every mandatory scenario (1B, 1S, 2S, 2, 3, 4, 5, 6, 7, 8,
  10, 11, 12, 13, 14, 16, 19, 20), against the sans-I/O session, each check labelled with its
  case (e.g. `14e`), and the optional ones that cost little (9, 15);
- the session checks that scenario 14 and 2q/2r need and the session lacks today: every
  non-application MsgType goes to the application at present, and nothing checks fields
  against a dictionary;
- a generated conformance table, `CONFORMANCE.md`, built from the test's own output, so the
  table cannot claim what the test did not show;
- interoperability with QuickFIX v1.16.0, both roles, over loopback.

It does not:

- validate the body of application messages (NewOrderSingle and the rest): note 007's, with the
  mechanism this note builds. The scenario 14 cases are shown on session-layer messages, which
  the session owns; note 007 adds its five messages to the same dictionary and the same table;
- implement encryption (17), third-party addressing (18) or application-not-available (14l):
  optional, and listed as not implemented with the reason;
- measure anything. The cost of the added checks belongs to note 008 (codec and session alone).

## Design

**The test.** `tests/conformance_test.cpp`, one function per scenario, each sub-case a script of
bytes and clock ticks into a session and checks on what comes out (bytes, disconnects, log
entries, the expected inbound number), as `session_test` does today. Each check prints one line:
case, result, and what was checked. 1B a (transport established) and 1S b (duplicate identity)
need the engine and the acceptor gate; they run over loopback in the same program. The
scenario checks in `session_test` stay where they are (they test the design's decisions); the
conformance test repeats what it needs, so each program can be read alone.

**The table.** `python run.py conformance` runs the test and writes `CONFORMANCE.md`: every
scenario and sub-case of the document in its order, its title shortened, and one of *pass* (with
the test's line), *not implemented* (with the reason), or *optional, not tested*; the counts by
status at the top. `--check` regenerates it in memory and fails if the committed file differs,
and runs in CTest, like `report --check`. A case the test does not print is a failure, not a
gap: the list of cases is written once, in `run.py`, from the document.

**D1. The dictionary for scenario 14 and 2q/2r.** The session needs to know, per message type,
which tags it allows, which are required, and each one's type and enumerated values.
(a) *Proposed:* generated at build time from the FIX Trading Community's orchestration of the
FIX 4.4 session layer, `FIX44Session.xml` (repository `FIXTradingCommunity/orchestrations`,
Apache-2.0), fetched by CMake at a pinned commit into `build/` like the baselines, and turned
into a constant table (a header) by a short script; nothing third-party committed. It lists the
seven session messages with their fields, presence and types; note 007 adds its five
application messages the same way (from the full FIX 4.4 orchestration, or by hand from
Volume 4, decided there). (b) Written by hand from FIX 4.4 Volume 2: no fetch, but a second
copy of facts that a machine-readable source already holds. (c) A full FIX 4.4 dictionary now:
note 001 ruled out a full data dictionary. Where the orchestration and the 2003 specification disagree, the specification wins and the
generator says which entries it overrode.

**D2. Undefined against not for this message (14a against 14c).** 14a needs to know whether a
tag is defined anywhere in FIX 4.4, which a session-only dictionary cannot say. Proposed: a tag
is *defined in the specification* if it is a FIX 4.4 tag number, read from the full FIX 4.4
field list (a list of numbers only, generated as in D1), or in the user-defined range
5000–9999; any other is reason 0 (invalid tag number), and a defined tag not allowed in this
message is reason 2. The full field list is the FIX 4.4 orchestration's (`OrchestraFIX44.xml`,
same repository and commit as D1): 912 tags, the highest 956, 44 numbers below it unused (20
and 24, for example, left FIX before 4.4), so a plain range would have been wrong.

**D3. Where the checks run.** After the codec has framed and decoded the message and the session
has checked the header and sequence number, and before the message is acted on: one pass over
the decoded fields against the message type's entry, giving the first failure in the
document's order (14a, b, c, d, e, f, g, h, i, j). A failing message is rejected and its number
consumed, as every 14 case requires; it is not acted on and not delivered. The pass is a table
lookup per field and allocates nothing; note 003's hot-path check must still pass. Note 004's
journal is unaffected: the input was journaled, and replay makes the same decision.

**D4. MsgType (2q, 2r).** *Valid* means a FIX 4.4 MsgType (the list generated with D2's) or a
user-defined one, beginning with "U" (the orchestration's definition of MsgType says so); *supported* means a session message or one the application has
declared, configured per session (note 007 declares its five). Invalid: Reject, reason 11.
Valid but not supported: BusinessMessageReject, reason 3, with RefSeqNum and RefMsgType; the
session sends it, because it alone knows the number to reference, and it is an application-level
message for resends (resent, not gap-filled).

**D5. A field containing SOH (14k).** The framer splits fields at SOH, so the session sees the
rest of the value as a field that is not *tag=value*; the codec rejects that today with reason 0
(invalid tag number), which is neither answer the document allows. Proposed: when the text after
a complete field does not begin with a tag and `=`, the codec reports reason 17 (*Non "data"
value includes field delimiter (SOH character)*, FIX 4.3 on), with RefTagID the field before it;
the message is rejected and its number consumed. (b) Treat it as garbled: also allowed, but the
counterparty is then never told, and its next message looks like a gap.

**D6. Interoperability with QuickFIX.** QuickFIX v1.16.0 (tag commit 2ce8a60, licence read in note
001) fetched by CMake into `build/` and built there, with its examples. One test program,
`interop/interop_test.cpp`, over loopback, labelled `interop` (CI's x64 job runs it; the others
skip it, as the QuickFIX build would add minutes to each), in two parts:

- our initiator against QuickFIX's `executor` example as acceptor, which fills each limit
  NewOrderSingle with an ExecutionReport;
- our acceptor (a venue answering each order) against a small initiator written against
  QuickFIX's documented API (`interop/qf_initiator.cpp`), because its `tradeclient` example is
  interactive.

Our side is the session (note 003) over a `tcp_connection`, driven by the test, not the engine:
the engines have no logout for the application, and the test must drop the connection itself;
the engines' transport is note 005's, tested there.

Each: log on, 2,000 orders and their reports in order, the connection dropped by our side after
700 reports with orders in flight and restored with a resend of what was missed, log out. Configuration files written for this repository. Rule
kept (note 001): QuickFIX's source is not read; what we use is its documentation, its example
programs as run, and its public API for the one initiator. Its acceptance-test suite (Ruby
scripts, a cross-check in note 001) is not run here: it needs Ruby, which is not installed, and
reading its scripts to drive it against our acceptor comes close to the rule. A decision for
Shep (see Tasks).

**Note 003's labels.** Its checks use the copy's numbering; where the official document differs
(2S), the labels are corrected and note 003's Review says so.

## Spec

- S1. Every scenario and sub-case of the *FIX Session Layer Test Cases* V1.0 is listed in
  `CONFORMANCE.md`; every mandatory one passes, except those stated not to apply with the reason;
  each optional one is passed or listed as not implemented with the reason (note 001, S1).
- S2. `CONFORMANCE.md` is generated from the test's output; `run.py conformance --check` passes
  in CTest; a case missing from the output fails it.
- S3. Each new check (D1–D5) is shown to catch a fault: a mutant per group (dictionary presence,
  types and enumerations, MsgType, the BusinessMessageReject's references) fails the test.
- S4. The session's hot path still makes no allocation or system call (`check_hot_path.py`), with
  the checks in it.
- S5. The session fuzzer, re-run with the checks, a stated number of executions, no breach and no
  sanitizer report; the corpus updated.
- S6. Against QuickFIX v1.16.0, both roles: logon, 2,000 orders and reports in order across a
  dropped connection with a resend, logout; no Reject and no Logout for an error in either log
  (note 001, S3).

## Tasks

- [x] Shep: agree D1–D6; decide whether QuickFIX's acceptance-test suite is run (D6)
- [x] Dictionary generator and the fetched orchestration (D1, D2)
- [x] Session checks: scenario 14, 2q, 2r, 14k (D3–D5)
- [x] `tests/conformance_test.cpp`, every case; note 003's labels corrected
- [x] `run.py conformance` and `CONFORMANCE.md`; `--check` in CTest (S1, S2)
- [x] Mutants, hot-path check, fuzzer (S3–S5)
- [x] QuickFIX fetched and built; both interop programs (S6)

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-23 | Draft | Written for review. Sources: *FIX Session Layer Test Cases* V1.0, June 2020 (official PDF, read in full; its applicability section still says last revised 20 September 2002); `FIX44Session.xml` from `FIXTradingCommunity/orchestrations` (Apache-2.0; the seven session messages, their fields and presence, read for D1). Today's session against the document: 2q and 2r not handled (every non-session MsgType is delivered), and of scenario 14 only what the codec establishes alone (14d, 14g, 14h for header and trailer tags, 14f for data lengths); the rest needs D1's dictionary |
| 2026-10-23 | Agreed | Approved by Shep, every decision as proposed (option A each): D1 the dictionary generated at build time from `FIX44Session.xml` fetched at a pinned commit; D2 *defined* means in FIX 4.4's full tag list or 5000–9999; D3 the checks in the session, after the header and sequence checks; D4 the session sends BusinessMessageReject for a valid unsupported MsgType, from a per-session list; D5 a field containing SOH rejected with reason 17; D6 both roles against QuickFIX, our acceptor against an initiator written to QuickFIX's documented API, and QuickFIX's acceptance-test suite not run |
| 2026-10-23 | Built | **Dictionary (D1, D2):** `tools/gen_dictionary.py` turns `OrchestraFIX44.xml` and `FIX44Session.xml` (orchestrations commit `cd24169a2abd`, each fetched by CMake into `build/_deps` and checked by SHA-256) into `fix44_dictionary.hpp` at build time: 912 FIX 4.4 tags (highest 956), 93 MsgTypes, the header, the trailer and the seven session messages with their 57 fields, types, enumerations and two groups (NoHops, NoMsgTypes). The two files must agree on these, or the build fails; they differ in two places, both overridden and stated in the header: the session file's MsgType code set lists only session types, and it types RefMsgType (372) as String; both 35 and 372 are checked as strings (MsgType's validity is D4's own check). The Draft's D1 wrongly read 2098 on Logon as a field added after 2003: it is the orchestration's id for the NoMsgTypes group; corrected. **Session (D3–D5):** `include/fixgw/validate.hpp`, one pass over a session message's fields against its header, body and trailer, the first failure giving the SessionRejectReason (0, 1, 2, 5, 6, 13, 15, 16), run after the CompID and SendingTime checks and under the same sequence rules as a message the codec rejects; MsgType checked against the session's `app_msg_types` (default `D F G 8 9 j`) and the FIX 4.4 list (Reject 11, or BusinessMessageReject 3 sent by the session, resent rather than gap-filled); a Logon failing the dictionary is an invalid Logon. The codec reports 14k as reason 17 when the text after a complete field has no `=` before its SOH (a fragment of a value); with an `=` it stays reason 0; scalar, SIMD and indexed decoders agree (100,000 mutated messages, and the differential fuzzer below). **Defect found in note 003's session** by 2t: a FIX.4.4 frame whose second field is not BodyLength was taken for a wrong BeginString and ended the session; fixed (note 003's Review). **Conformance (S1, S2):** `tests/conformance_test.cpp`, every case, each check labelled; `python run.py conformance` writes `CONFORMANCE.md` from its output, the case list written once from the document; `--check` in CTest fails on a stale table, a case not shown, a case shown that the list says is not, or a FAIL. 85 cases: 67 mandatory pass; 2 mandatory to note 007 (14m conditionally required fields, 19a duplicate detection by ClOrdID or ExecID: application-message rules); 1 mandatory not applicable (14n, encryption); 2 optional pass (9 by ResetSeqNumFlag, 15); 13 optional not implemented (14l, 17a–j, 18a–b), each with its reason. **S3:** mutants, each failing the test: required fields not checked, values not checked, every MsgType valid, BusinessMessageReject referencing the wrong number, undefined tags read as defined, group entry counts never compared. Two gaps found on the way and closed: the first required-field mutant passed, because the session's older checks also reject a TestRequest without TestReqID (a Reject without RefSeqNum added, which only the dictionary catches); a group mutant on the too-many check alone was redundant (the end-of-group check catches a surplus too), replaced by one that never compares counts. **S4:** `check_hot_path.py`: inbound 14 functions and 12,475 instructions (11 and 9,876 before), no allocation, no system call. **S5:** `fuzz_session` 623,822 executions in 601 s, no breach, no sanitizer report, 815 coverage edges (724 at note 003); `fuzz_diff` 3,500,472 executions, the decoders agree; corpora merged (164 and 50 new inputs). **S6:** QuickFIX v1.16.0 built in `build-*/_deps` (its header-copy targets first: nothing depends on them, so a clean parallel build compiled its examples before the headers existed); `interop/interop_test.cpp`, both roles, 2,000 limit orders each: our initiator against `executor`, the connection dropped by us after 700 reports with 20 orders sent unread, 1 ResendRequest, 21 reports resent with PossDup; QuickFIX's initiator (`interop/qf_initiator.cpp`, written to its documented API) against our acceptor, dropped by us after 700 answers with 50 ms unread, 1 ResendRequest, 64 orders resent with PossDup; every order answered once, in order, logged out, no Reject and no Logout for an error on either side (QuickFIX's logs scanned too); 8 runs alike. The drops were made deterministic after a first version could leave nothing in flight. Full suite clean; TSan, ASan and UBSan clean; aarch64 compiles. CI: the x64 job builds QuickFIX and runs the interop test (`ci/test.sh release interop`) |
| 2026-10-23 | Measured | Reviewed and approved by Shep against the Spec: S1–S6 met as recorded above (CI green on all four jobs at `f065f99`, the interop test in the x64 job). Carried forward: 14m and 19a to note 007 (application-message rules); interoperability runs the session directly, not the engine (the engines have no logout for the application); the cost of the dictionary checks on the inbound path to note 008. A functional component: no benchmark here |
