# Open items

Every item a note's Review leaves open, one row each. Each gets one decision: **fix** (a task;
closed by its commit), **limit** (stated where it qualifies a figure; closed) or **parked**
(changes no published conclusion; closed, with what would reopen it). A note does not reach
Measured while an item it raised has no decision.

Decided with Shep 2026-10-30: every item below as written.

| # | Item | Note | Touches a published figure | Decision |
|---|---|---|---|---|
| G1 | Dictionary check p50 spread (786 vs 1,316 ns), seen once in six runs | 008 | Stage set | parked: reopen if it recurs |
| G2 | QuickFIX file store at 20,000/s: two p50 levels (48.6 vs 76–78 µs) | 008 | The baseline's figure | limit: published as a range |
| G3 | Outbound body checks ~80 branch mispredictions a message (~1.25 µs) | 008 | Stage set | parked: reopen when the gateway's send path is optimised |
| G4 | Scalar framing of ExecutionReport and Heartbeat withheld (S8: p50 6.8% / 3.4% apart across runs) | 009 | Withheld | limit: withheld, as the report states |
| G5 | ~24 µs of the card round trip is inside the card | 005 | Card round trip | limit: needs hardware timestamps to split |
| G6 | One journal file per session, replayed whole; a split per sequence epoch needs a session snapshot per file | 004 | No | parked: reopen if replay time matters (1.8M records/s today) |
