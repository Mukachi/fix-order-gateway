# fix-order-gateway

A FIX 4.4 order entry gateway: session layer, recovery and order entry, written from the public
specification. Work in progress: see [docs/design/001-scope.md](docs/design/001-scope.md).

## Results

<!-- results:codec-summary -->
| Operation | Message | Repr. | Bytes | State | Mispred./msg | Episodes | Mixed | p50 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|---|---|
| frame and decode | NewOrderSingle | A | 173 | fast | 13.05–13.94 | 15–131 | 2–4 | 987–1,008 | 1,212–1,242 | 1,301–1,329 | 2,255–2,866 |
| frame and decode | NewOrderSingle | A | 173 | slow | 42.68–42.72 | 65–182 | 2–4 | 1,210–1,216 | 1,459–1,467 | 1,548–1,559 | 2,624–2,900 |
| frame and decode | NewOrderSingle | scalar | 173 | fast | 6.87–6.91 | 92–119 | 2–7 | 365–373 | 461–481 | 641–650 | 1,859–17.6 µs |
| frame and decode | NewOrderSingle | scalar | 173 | slow | 24.41–24.84 | 75–106 | 2–7 | 499–501 | 601–609 | 756–757 | 1,729–1,987 |
| frame and decode | NewOrderSingle | SIMD | 173 | fast | 4.17 | 17–147 | 1–6 | 280–281 | 402 | 624–625 | 1,548–2,071 |
| frame and decode | NewOrderSingle | SIMD | 173 | slow | 16.32–16.80 | 49–182 | 1–6 | 365–368 | 451–454 | 695–697 | 1,826–2,204 |
| frame and decode | NewOrderSingle | SIMD, padded | 173 | fast | 2.11–2.13 | 104–107 | 6–12 | 248–249 | 366–383 | 592–595 | 1,700–1,960 |
| frame and decode | NewOrderSingle | SIMD, padded | 173 | slow | 11.14–11.50 | 84–90 | 6–12 | 313–315 | 387–391 | 654–656 | 1,739–1,818 |
| frame and decode | NewOrderSingle | two-stage | 173 | fast | 2.35–2.39 | 71–99 | 3–7 | 282–283 | 399–413 | 623–627 | 1,728–2,049 |
| frame and decode | NewOrderSingle | two-stage | 173 | slow | 13.91–14.60 | 94–126 | 3–7 | 364–366 | 448–449 | 691–694 | 1,839–1,942 |
| encode | NewOrderSingle | A | 173 | fast | 0.89 | 5–8 | 0–1 | 878–881 | 917–920 | 937–942 | 1,310–2,152 |
| encode | NewOrderSingle | A | 173 | slow | 8.81–11.36 | 191–195 | 0–1 | 937–951 | 1,041–1,062 | 1,070–1,092 | 2,392–2,411 |
| encode | NewOrderSingle | B | 173 | fast | 0.19 | 24–99 | 0–1 | 260 | 283 | 302–306 | 1,635–1,697 |
| encode | NewOrderSingle | B | 173 | slow | 1.57–1.62 | 100–176 | 0–1 | 262 | 291 | 308–309 | 1,654–1,704 |
| encode | NewOrderSingle | C | 190 | one | 0.00 | 200 | 0 | 154 | 158 | 161–168 | 1,535–1,542 |

Ns per call, range over 3 runs, clock cost included (about the empty window's p50). The branch predictor has two stable states for this code; figures are per state, never pooled, and how many episodes fall in each is not reproducible (note 009). Full tables and conditions: [RESULTS.md](RESULTS.md).
<!-- /results -->

<!-- results:store-summary -->
Store figures: not yet measured (design note 004).
<!-- /results -->
