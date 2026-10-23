# 006 — Conformance

**Status:** Draft · **Updated:** 2026-10-23

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
note 001 ruled out a full data dictionary. One wrinkle in (a): the orchestration is the session
layer as maintained today, and lists fields added after FIX 4.4's 2003 errata (e.g. 2098 on
Logon). Where it and the 2003 specification disagree, the specification wins and the generator
says which fields it dropped.

**D2. Undefined against not for this message (14a against 14c).** 14a needs to know whether a
tag is defined anywhere in FIX 4.4, which a session-only dictionary cannot say. Proposed: a tag
is *defined in the specification* if it is a FIX 4.4 tag number, read from the full FIX 4.4
field list (a list of numbers only, generated as in D1), or in the user-defined range
5000–9999; any other is reason 0 (invalid tag number), and a defined tag not allowed in this
message is reason 2. The full field list's source is to be confirmed when built (the full FIX
4.4 orchestration, or Volume 6), and stated here.

**D3. Where the checks run.** After the codec has framed and decoded the message and the session
has checked the header and sequence number, and before the message is acted on: one pass over
the decoded fields against the message type's entry, giving the first failure in the
document's order (14a, b, c, d, e, f, g, h, i, j). A failing message is rejected and its number
consumed, as every 14 case requires; it is not acted on and not delivered. The pass is a table
lookup per field and allocates nothing; note 003's hot-path check must still pass. Note 004's
journal is unaffected: the input was journaled, and replay makes the same decision.

**D4. MsgType (2q, 2r).** *Valid* means a FIX 4.4 MsgType (the list generated with D2's) or one
configured as user-defined; *supported* means a session message or one the application has
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
001) fetched by CMake into `build/` and built there, with its examples. Two programs, over
loopback, each a test labelled `interop` (CI's x64 job runs it; the others skip it, as the
QuickFIX build would add minutes to each):

- our initiator (the engine, journaled) against QuickFIX's `executor` example as acceptor, which
  answers each NewOrderSingle with an ExecutionReport;
- our acceptor (the test venue) against a small initiator written against QuickFIX's documented
  API (`interop/qf_initiator.cpp`), because its `tradeclient` example is interactive.

Each: log on, 1,000 orders and their reports in order, the connection dropped by our side and
restored with a resend of what was missed, 1,000 more, log out. Configuration files ours. Rule
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

- [ ] Shep: agree D1–D6; decide whether QuickFIX's acceptance-test suite is run (D6)
- [ ] Dictionary generator and the fetched orchestration (D1, D2)
- [ ] Session checks: scenario 14, 2q, 2r, 14k (D3–D5)
- [ ] `tests/conformance_test.cpp`, every case; note 003's labels corrected
- [ ] `run.py conformance` and `CONFORMANCE.md`; `--check` in CTest (S1, S2)
- [ ] Mutants, hot-path check, fuzzer (S3–S5)
- [ ] QuickFIX fetched and built; both interop programs (S6)

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-23 | Draft | Written for review. Sources: *FIX Session Layer Test Cases* V1.0, June 2020 (official PDF, read in full; its applicability section still says last revised 20 September 2002); `FIX44Session.xml` from `FIXTradingCommunity/orchestrations` (Apache-2.0; the seven session messages, their fields and presence, read for D1). Today's session against the document: 2q and 2r not handled (every non-session MsgType is delivered), and of scenario 14 only what the codec establishes alone (14d, 14g, 14h for header and trailer tags, 14f for data lengths); the rest needs D1's dictionary |
