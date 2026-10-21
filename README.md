# fix-order-gateway

A FIX 4.4 order entry gateway: session layer, recovery and order entry, written from the public
specification. Work in progress: see [docs/design/001-scope.md](docs/design/001-scope.md).

## Results

<!-- results:codec-summary -->
| Operation | Message | Repr. | Bytes | p50 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|
| frame and decode | NewOrderSingle | A | 165 | 958–1,210 | 1,011–1,333 | 1,023–1,367 | 1,413–1,686 |
| encode | NewOrderSingle | A | 165 | 1,101–1,120 | 1,140–1,159 | 1,156–1,173 | 2,331–2,375 |
| encode | NewOrderSingle | B | 165 | 325 | 350–362 | 368–376 | 755–1,572 |
| encode | NewOrderSingle | C | 190 | 187–190 | 190–192 | 192–196 | 512–571 |
| frame and decode | NewOrderSingle | B | 165 | 203–213 | 222–242 | 260–276 | 579–675 |

Ns per call, range over 3 runs, clock cost included (about the empty window's p50). Full tables and conditions: [RESULTS.md](RESULTS.md).
<!-- /results -->
