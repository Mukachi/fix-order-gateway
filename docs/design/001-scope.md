# 001 — Scope and plan

**Status:** Draft · **Updated:** 2026-10-21

## Proposal

A FIX 4.4 order entry gateway, written from the public specification. Its hard core is the
session layer and recovery: logon and session state, sequence numbers, gap fill and resend,
heartbeats and test requests, partial and coalesced socket reads, out-of-spec messages, and
sequence numbers that survive a crash. Order entry messages ride on top of it.

Many FIX engines exist. This one is meant to be judged on three things few of them publish:

1. **Conformance, shown case by case.** Every applicable case of the FIX Trading Community's
   *FIX Session Layer Test Cases* is an automated test, with its result listed; the cases that do
   not apply say why.
2. **Recovery under failure.** The process is killed at chosen points (mid-write, mid-resend,
   between store and send) and restarted; sequence numbers and resends must come out right every
   time.
3. **Measured like `lockfree-ipc`.** Latency of the order path as percentiles, open loop, with the
   machine's conditions and noise floor in every run, on the same harness against QuickFIX, the
   usual open-source baseline. Where it loses, the table says so.

This note fixes the scope, the order of the component notes and the decisions that cut across
them. Each component then gets its own note, agreed before its code.

It does:

- FIX 4.4 tag=value over TCP, both roles: initiator (the gateway connecting to a venue) and
  acceptor (for tests, and for a venue simulator to measure against);
- the session layer per the *FIX Session Layer Technical Standard* and FIX 4.4 Volume 2;
- order entry: NewOrderSingle, OrderCancelRequest, OrderCancelReplaceRequest, ExecutionReport,
  OrderCancelReject; an outbound throttle; cancel-on-disconnect in the simulator;
- a message store and sequence persistence that are crash-safe, with the cost of each fsync
  policy measured.

It does not:

- implement FIX 5.0/FIXT, a full data dictionary, or any message beyond those named;
- do TLS, market data, or native binary protocols (OUCH, iLink3);
- use kernel bypass: dev-lnx01 has no such network card. The kernel TCP path is measured and its
  floor stated, not hidden;
- do pre-trade risk. That is the integration repo's, where it sits between this gateway and the
  strategy.

## Design

**Sources.** FIX 4.4 specification with errata (20030618), Volume 2 for the session protocol;
*FIX Session Layer Technical Standard* (2020 errata); *FIX Session Layer Test Cases* (2020), the
conformance basis. All free from fixtrading.org. ISO 3531-3:2022 holds the same test cases; it is
not used.

**Sans-I/O core.** The codec and the session are pure: bytes and timer ticks in, bytes and actions
out, no sockets, clocks or threads inside. The transport is a thin layer around them. This makes
every conformance case and every recovery scenario a deterministic, scripted test, lets a session
be replayed from its journal, and lets the codec and session be measured apart from the kernel.

**One thread per session.** A session's socket, state and store belong to one pinned thread, with
no locks. The application reaches it through `lockfree-ipc`'s SPSC rings.

**Reuse of `lockfree-ipc`.** Its queues and its whole measurement harness (clock, histogram,
open-loop generator, environment, noise floor, `run.py` report) are fetched by CMake at a pinned
commit into `build/`, as the baselines are. One source of truth, and every run records which
commit it measured with. The alternatives: a git submodule (the same pinning, more friction for
anyone cloning) or copying (drift between two copies); not taken. If the harness needs changing,
it changes in `lockfree-ipc` first.

**Baseline.** QuickFIX (C++, v1.16.0, 2026-05-09) fetched and pinned likewise, never committed.
Two uses: a latency baseline on the same harness, and interoperability (our initiator against its
acceptor, and the reverse). Its own acceptance-test scripts are a cross-check, not the conformance
basis. Its licence (the QuickFIX Software License) is to be read before it is fetched.

**Component notes, in order** (each a Draft for agreement before code):

| Note | Component | The hard part |
|---|---|---|
| 002 | Codec | Framing a byte stream into messages across partial and coalesced reads; BodyLength and CheckSum; garbled input (ignored) against invalid messages (rejected); zero-copy field access; no allocation |
| 003 | Session | The state machine: logon and logout, heartbeat and TestRequest, sequence rules (too low, too high, PossDup), ResendRequest, SequenceReset (gap fill and reset), Reject |
| 004 | Store and recovery | Append-only journal of outbound messages and sequence numbers; crash-safety under `kill -9` at chosen points; fsync policies measured |
| 005 | Transport | Non-blocking TCP, initiator and acceptor, reconnect, `TCP_NODELAY`, busy polling; the kernel path's latency floor |
| 006 | Conformance | The *Session Layer Test Cases* as scripted tests against the sans-I/O session; interoperability with QuickFIX |
| 007 | Order entry | The application messages, outbound throttle, a venue simulator with matching sufficient to fill, and cancel-on-disconnect |
| 008 | Measurement | Order-to-ExecutionReport latency, open loop, against QuickFIX; codec and session cost alone |

## Spec

The repo as a whole, checked as the notes reach Measured:

- S1. Every *Session Layer Test Case* that applies to FIX 4.4 is an automated test that passes;
  each case not implemented is listed with the reason.
- S2. Killed at any of the chosen points and restarted, a session resumes with correct sequence
  numbers and answers resends from its store, with no message lost or sent twice under a new
  sequence number.
- S3. Our initiator and QuickFIX's acceptor, and the reverse, log on, exchange orders and
  execution reports, recover from a dropped connection, and log out cleanly.
- S4. The codec and session hot paths make no allocation or system call (checked in the compiled
  code, as in `lockfree-ipc`).
- S5. Latency is published as percentiles over at least three runs, via `run.py report`, beside
  QuickFIX's on the same harness and machine.

## Tasks

- [ ] Shep: empty GitHub repo `Mukachi/fix-order-gateway`
- [ ] Read the QuickFIX licence; record the decision here
- [ ] Notes 002–008, one at a time, each agreed before its code

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-21 | Draft | Written for review |
