# 007 — Order entry

**Status:** Draft · **Updated:** 2026-10-23

## Proposal

Notes 002 to 006 made the message stream reliable: framed, checked, numbered, journaled before
anything leaves, recovered by replay, conformant case by case. This note puts orders on it: the
five order-entry messages, the order state machine that turns ExecutionReports into each order's
state, the outbound throttle, a venue simulator to trade against, and the cases note 001 put at the
centre of the design, where it is not known whether an order exists (note 001, S7).

Sources: FIX 4.4 with Errata 20030618, Volume 4 (*Orders and Executions*, read for this note:
ExecutionReport usage, OrdStatus precedence, the *Order State Change Matrices* A to L); Volume 2
for resend; the FIX 4.4 orchestration already fetched by note 006 (`OrchestraFIX44.xml`, which
the FIX Trading Community corrected in EP311, August 2026; our pinned commit is from 8 September
2026, the day EP311 was last updated, so it likely carries it: to be confirmed when built). Carried here from earlier notes: 14m and 19a of the *Session Layer Test Cases*
(note 006), the stale-order hook (note 003), write-ahead in both directions (note 005: an inbound
message is durable before the application sees it), and a logout for the application (note 006:
the engines have none).

It does:

- NewOrderSingle (D), OrderCancelRequest (F), OrderCancelReplaceRequest (G) out; ExecutionReport
  (8), OrderCancelReject (9) in; BusinessMessageReject (j) both ways;
- their bodies checked against the FIX 4.4 dictionary, as note 006 checks session messages, with
  the conditional rules Volume 4 states in prose (14m: BusinessMessageReject, reason 5);
- the order state machine, one per ClOrdID chain, on the session's thread (D1), its state
  rebuilt by journal replay like the session's;
- duplicate ExecutionReports discarded by ExecID (19a);
- an outbound throttle (D4) and the stale-order policy for resends and for orders queued while
  disconnected (D5);
- the in-doubt cases as tests that inject the failure (D6, note 001 S7);
- a venue simulator: the acceptor engine with a price-time book per symbol, enough to fill,
  part-fill, rest, cancel and replace, and cancel-on-disconnect (D7);
- the *Order State Change Matrices* in scope (D2), each a scripted test, listed in
  `CONFORMANCE.md` beside the session cases, generated the same way;
- a logout and a stop for the application, through the ring, in both engines.

It does not:

- send or take OrderStatusRequest (H), mass cancel, lists, quotes or allocations: not among note
  001's five messages; the in-doubt cases are resolved without status requests (D6);
- pre-trade risk (the integration repo's, between strategy and gateway);
- measure anything: order-to-ExecutionReport latency is note 008's.

## Design

**Where orders live.** The application submits D, F and G through the ring as today; the session
thread passes each through the order state machine before the session sends it, and each inbound
8 or 9 through it before it reaches the application. The application receives the message and
the order's state after it (OrdStatus as the gateway understands it, CumQty, LeavesQty, AvgPx),
so it does not have to rebuild the chain itself. Everything the machine decides follows from
journaled inputs, so replay rebuilds it exactly, as it rebuilds the session (note 004).

```mermaid
stateDiagram-v2
    [*] --> PendingNew: D sent
    PendingNew --> New: 8 ExecType=New
    PendingNew --> Rejected: 8 ExecType=Rejected
    New --> PartiallyFilled: 8 Trade
    PartiallyFilled --> Filled: 8 Trade, LeavesQty 0
    New --> PendingCancel: F sent
    PartiallyFilled --> PendingCancel: F sent
    PendingCancel --> Canceled: 8 Canceled
    PendingCancel --> New: 9 (cancel rejected)
    New --> PendingReplace: G sent
    PendingReplace --> New: 8 Replaced (new ClOrdID)
    PendingReplace --> New: 9 (replace rejected)
    New --> Canceled: 8 Canceled, unsolicited (E.1.b, cancel-on-disconnect)
```

(A simplification: a fill may arrive in any pending state, with the original order's
parameters, as Volume 4 requires; OrdStatus then follows its precedence table.)

**Decisions for agreement:**

- **D1. Who keeps order state.** (a) *Proposed:* the gateway, on the session thread, as above: one
  copy, rebuilt by replay, checked against every ExecutionReport (OrderQty = CumQty + LeavesQty
  while active, CumQty never falling except by a trade bust, ExecID unique), and requests for
  unknown or finished orders refused before they leave. (b) The application keeps it and the
  gateway passes messages through: less here, but every application rebuilds the same chain,
  and replay would not cover it.

- **D2. Which matrices.** (a) *Proposed:* A (vanilla), B (cancel, all six), C (replace quantity
  changes), D (replace sequencing and chaining), E.1.b (unsolicited cancel: what cancel-on-disconnect
  produces), F (duplicate ClOrdID, PossResend), I (IOC and FOK), J (trade cancel and correct: a
  bust lowers CumQty, which the state machine must accept). Out, with the reason in the table:
  E.1.a, c–e (telephoned orders, unsolicited replaces), G (needs OrderStatusRequest), H (GT orders
  over days, corporate actions), K (trading halts), L (stopped orders, CashOrderQty). (b) A–D
  only: fewer tests, but busts and IOC/FOK left unhandled.

- **D3. Duplicate ExecutionReports (19a).** An ExecutionReport whose ExecID was seen is discarded
  (logged, not delivered). (a) *Proposed:* a fixed-capacity hash set of ExecIDs for the session's
  life (configured, default 1 million, no allocation after start; full is an error logged and
  the session refuses new orders until the next reset, never a silent miss). (b) ExecIDs kept per
  order until it is finished: smaller, but a late resend for a finished order would not be
  recognised.

- **D4. Throttle.** At most N application messages per rolling window T (venue rules of
  engagement set both; a ring of the last N send times, no allocation). What happens to one over
  the limit: (a) *Proposed:* refused back to the application at once, with the time it may retry;
  an order delayed in a queue is a stale order in the making, and the application should decide.
  (b) Queued in order and sent when the window allows: simpler for the application, but latency
  becomes unbounded under load. Cancels: (i) *proposed:* counted like everything else (venues
  count them) but never refused while an order of theirs is live: they may exceed N by the number
  of live orders, because a refused cancel is a risk the throttle must not create; (ii) counted
  and refused like the rest.

- **D5. Stale orders on resend.** When the venue asks for a range we sent (its ResendRequest), a
  message in it never reached the venue. (a) *Proposed:* a NewOrderSingle or replace older than a
  configured age (default 2 s) is gap-filled, not resent, and the application is told it was never
  sent (the order becomes Rejected locally, with the reason); a cancel is always resent, unless
  its order is already finished. Gap-filling is safe exactly because the venue asked: it did not
  get the message, so the order cannot exist there. The same rule applies to orders queued while
  disconnected (note 003, D2). (b) Resend everything (FIX's default): a minutes-old order may
  trade at a price no one wants. (c) Never resend orders: simple, but loses orders that were
  only a moment late.

- **D6. The in-doubt cases (S7)**, each a test that injects the failure, then checks order and
  session state after recovery:
  1. *crash before recording:* the order is not in the journal, so it was never sent (note 004's
     D1); after restart the application is told every order the journal knows, and an order it
     submitted that is missing was not sent;
  2. *crash after sending, before the ExecutionReport:* the order is journaled and sent; after
     restart, logon and sequence recovery bring the report (resent by the venue), or the venue's
     ResendRequest shows it never arrived (then D5);
  3. *connection lost before the report:* as 2 without the crash; with cancel-on-disconnect, the
     venue's unsolicited cancels arrive after logon and are applied (E.1.b);
  4. *stale resend:* D5, both outcomes.
  No OrderStatusRequest is needed: the session's sequence recovery is complete for what the
  venue sent, and D5 is complete for what we sent.

- **D7. The venue simulator.** (a) *Proposed:* the acceptor engine with a price-time book per
  symbol (limit and market orders, IOC and FOK, part fills, cancel, replace keeping priority only
  for a quantity decrease), liquidity from resting orders other sessions placed or the test
  seeded, cancel-on-disconnect configurable per session; plus scripted responses where a matrix
  needs an order of events a book will not produce (B.1.e, the acknowledgement and the cancel
  crossing). (b) Fill everything at once, as QuickFIX's executor does: not enough for B to D.

- **D8. The application messages' dictionary.** Note 006's generator extended to components and
  nested groups. (a) *Proposed:* the five messages as FIX 4.4 defines them (any valid field in
  its place accepted), plus Volume 4's conditional rules written by hand for the fields the
  state machine uses (Price when OrdType is limit, StopPx for stops, OrigClOrdID on F and G and on
  the reports answering them, and the rest as listed in the tests): failure is
  BusinessMessageReject 5 (14m). (b) A profile: only the fields this gateway uses, others
  rejected (as venues' rules of engagement do): stricter, but a venue adding a field would break
  us.

**The application's controls.** Two new ring entries: *logout* (the engine calls the session's
logout and stops reconnecting) and *stop after logout*. Both engines. The interop test of note
006 then runs through the engine as well.

## Spec

- S1. Each matrix in scope (D2) is a scripted test against the state machine and the venue
  simulator, listed in `CONFORMANCE.md` with its result; each out of scope says why.
- S2. 14m and 19a pass and move to *pass* in `CONFORMANCE.md`.
- S3. Each in-doubt case of D6 is a test that injects the failure (SIGKILL at the point, or the
  connection dropped) and checks every order's state and the session's numbers after recovery;
  no order is lost, duplicated, or sent stale.
- S4. The order state machine and the throttle make no allocation or system call in steady
  state (the hot-path check and the allocation test extended).
- S5. Replay: the state of every order after replaying a journal equals the state before the
  crash (a property test over random scripts).
- S6. Fuzzing: the session fuzzer extended with application messages; stated executions, no
  breach of the state machine's invariants (D1), no sanitizer report.
- S7. Interoperability (note 006, S6) repeated through the engine, with cancels and replaces,
  against QuickFIX's `executor` (fills only) and its `ordermatch` example (a book).

## Tasks

- [ ] Shep: agree D1–D8
- [ ] Dictionary for the five messages and their conditional rules (D8, S2)
- [ ] Order state machine and ExecID set (D1, D3)
- [ ] Throttle and stale-order policy (D4, D5)
- [ ] Application controls: logout, stop (both engines)
- [ ] Venue simulator (D7)
- [ ] Matrices as tests, in `CONFORMANCE.md` (D2, S1)
- [ ] In-doubt tests (D6, S3); replay property test (S5)
- [ ] Hot path, allocation, fuzzing (S4, S6); interop through the engine (S7)

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-23 | Draft | Written for review. Sources: FIX 4.4 with Errata 20030618 (the specification package from fixtrading.org, read: Volume 4's ExecutionReport usage, OrdStatus precedence and the Order State Change Matrices' list, A.1.a to L.1.b); `OrchestraFIX44.xml` at the pinned commit (the five messages: NewOrderSingle 78 members, OrderCancelRequest 23, OrderCancelReplaceRequest 77, ExecutionReport 140, OrderCancelReject 23; the orchestration has no machine-readable conditional rules, hence D8's hand-written ones); EP311 (FIX 4.4 Errors and Omissions, August 2026) noted: it corrects the orchestration, and our pinned commit is from the day it was published |
