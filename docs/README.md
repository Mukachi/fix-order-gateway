# Documentation

Every component gets a design note before any code is written. Decisions with real alternatives
get an ADR.

- **Design notes** (`design/`): one file per component, sections Proposal, Design, Spec, Tasks,
  Review. Status moves Draft → Agreed → Built → Measured. Two review points: Draft → Agreed
  (approved before code) and Built → Measured (results checked against the Spec). The Review log
  keeps every finding, including the wrong turns, in the order they happened.
- **ADRs** (`adr/`): Context, Decision, Consequences, Status. Only for choices a reviewer might
  question; none so far: each note's decisions (D1, D2, ...) record their alternatives.

Only Spec and Review are mandatory. Any section can be one line.

## Design notes

| # | Component | Status |
|---|---|---|
| [001](design/001-scope.md) | Scope and plan | Agreed |
| [002](design/002-codec.md) | Codec | Measured |
| [003](design/003-session.md) | Session | Measured |
| [004](design/004-store.md) | Store and recovery | Measured |
| [005](design/005-engine.md) | Transport and engine loop | Measured |
| [006](design/006-conformance.md) | Conformance | Measured |
| [007](design/007-order-entry.md) | Order entry | Measured |
| [008](design/008-measurement.md) | Measurement | Measured |
| [009](design/009-codec-simd.md) | Codec: SIMD scanning and two-stage indexing | Measured |
