# fix-order-gateway

A FIX 4.4 order entry gateway: session layer, recovery and order entry, written from the public
specification. Work in progress: see [docs/design/001-scope.md](docs/design/001-scope.md).

## Conformance

Every case of the *FIX Session Layer Test Cases* (V1.0, June 2020), and every *Order State Change
Matrix* of FIX 4.4 Volume 4, with what the tests show or why a case is not shown:
[CONFORMANCE.md](CONFORMANCE.md), generated from the tests' output.
Interoperability with QuickFIX v1.16.0, both roles, across a dropped connection: `interop/`
([note 006](docs/design/006-conformance.md)).

## Results

<!-- results:codec-summary -->
| Operation | Message | Repr. | Bytes | State | Mispred./msg | Episodes | Mixed | p50 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|---|---|
| frame and decode | NewOrderSingle | A | 173 | fast | 13.05–13.94 | 15–131 | 2–4 | 987–1,008 | 1,212–1,242 | 1,301–1,329 | 2,255–2,866 |
| frame and decode | NewOrderSingle | A | 173 | slow | 42.68–42.72 | 65–182 | 2–4 | 1,210–1,216 | 1,459–1,467 | 1,548–1,559 | 2,624–2,900 |
| frame and decode | NewOrderSingle | scalar | 173 | fast | 6.87–6.91 | 92–119 | 2–7 | 365–373 | 461–481 | 641–650 | 1.9–17.6 µs |
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
| Strategy | Bound | Offered /s | p50 | p99 | p99.9 | max | Sustained /s |
|---|---|---|---|---|---|---|---|
| A | - | 1,000 | 49.3 µs–49.5 µs | 55.6 µs–56.0 µs | 59.5 µs–60.3 µs | 730.8 µs–1,661.4 µs | 1,000 |
| B | 4 records or 20 us | 1,000 | 64.9 µs–65.3 µs | 68.5 µs–72.5 µs | 78.5 µs–175.5 µs | 1,698.6 µs–1,924.8 µs | 1,000 |
| B | 16 records or 100 us | 1,000 | 144.9 µs–145.4 µs | 148.9 µs–149.0 µs | 155.3 µs–162.2 µs | 1,644.5 µs–2,056.6 µs | 1,000 |
| B | 64 records or 500 us | 1,000 | 544.8 µs–545.4 µs | 548.5 µs–550.0 µs | 556.4 µs–577.3 µs | 1,476.1 µs–2,239.7 µs | 1,000 |
| C | - | 1,000 | 390–394 | 3,099–3,382 | 5,802–6,455 | 104.0 µs–106.5 µs | 1,000 |
| D | standby on CPU 4, loopback | 1,000 | 72.4 µs–73.0 µs | 79.4 µs–79.9 µs | 83.4 µs–83.9 µs | 110.6 µs–977.1 µs | 1,000 |

Order arrival to release of its bytes, ns, at 1,000 orders a second, range over 3 runs. Every rate, the guarantees and the conditions: [RESULTS.md](RESULTS.md).
<!-- /results -->

<!-- results:net-summary -->
| Measure | loopback p50 | card p50 | card-busypoll p50 |
|---|---|---|---|
| round trip, busy-poll | 7,431–7,538 | 39.9 µs–40.5 µs | 31.0 µs–31.6 µs |
| round trip, block | 13.7 µs–13.9 µs | 42.0 µs–45.0 µs | 31.5 µs–32.1 µs |
| round trip, spin then block | 7,271–7,549 | 37.2 µs–40.5 µs | 30.6 µs–31.5 µs |
| order path, direct, 1,000/s | 122.0 µs–123.9 µs | 150.2 µs–153.3 µs | 150.0 µs–151.3 µs |
| order path, pipeline, 1,000/s | 106.3 µs–106.8 µs | 135.8 µs–139.8 µs | 132.0 µs–133.5 µs |
| order path, direct, 10,000/s | 237.1 µs–238.3 µs | 234.5 µs–237.6 µs | 235.1 µs–235.4 µs |
| order path, pipeline, 10,000/s | 98.7 µs–99.2 µs | 145.6 µs–183.6 µs | 147.5 µs–147.7 µs |

Range over the runs of each path. Conditions, p99 and the method: [RESULTS.md](RESULTS.md).
<!-- /results -->

Against QuickFIX, order to ExecutionReport ([note 008](docs/design/008-measurement.md)):

<!-- results:order-summary -->
| Configuration | Survives | Offered /s | p50 | p99 | Venue p50 |
|---|---|---|---|---|---|
| ours, pipeline, journal A, busy-poll | power loss | 1,000 | 115.8 µs–116.1 µs | 126.0 µs–129.7 µs | 9,511–9,869 |
| ours, pipeline, journal A, busy-poll | power loss | 10,000 | 140.2 µs–141.0 µs | 193.4 µs–194.0 µs | 9,204–9,613 |
| ours, direct, journal A, block | power loss | 1,000 | 214.4 µs–216.5 µs | 230.3 µs–231.2 µs | 9.8–10.3 µs |
| ours, direct, journal A, block | power loss | 10,000 | 253.3 µs–254.1 µs | 353.7 µs–355.3 µs | 9,119–9,660 |
| ours, direct, journal C, busy-poll | process crash | 1,000 | 20.0 µs–20.9 µs | 28.4 µs–30.3 µs | 9,321–9,879 |
| ours, direct, journal C, busy-poll | process crash | 10,000 | 18.9 µs–19.6 µs | 23.1 µs–23.8 µs | 8,952–9,562 |
| QuickFIX 1.16.0, file store | process crash | 1,000 | 103.8 µs–107.6 µs | 114.4 µs–119.0 µs | 9.6–12.2 µs |
| QuickFIX 1.16.0, file store | process crash | 10,000 | 47.1 µs–50.4 µs | 55.7 µs–59.9 µs | 9.4–12.0 µs |
| QuickFIX 1.16.0, PersistMessages=N | nothing persisted | 1,000 | 90.4 µs–93.9 µs | 95.9 µs–99.9 µs | 9.5–12.2 µs |
| QuickFIX 1.16.0, PersistMessages=N | nothing persisted | 10,000 | 32.2 µs–34.8 µs | 37.9 µs–41.3 µs | 9.3–12.1 µs |

Order to ExecutionReport, application to application, open loop, ns, range over the runs. Highest rate sustained: ours, pipeline, journal A, busy-poll ≥ 100,000/s; ours, direct, journal A, block 10,000/s; ours, direct, journal C, busy-poll ≥ 100,000/s; QuickFIX 1.16.0, file store 20,000/s; QuickFIX 1.16.0, PersistMessages=N 50,000/s. The guarantees, the method and every rate: [RESULTS.md](RESULTS.md).
<!-- /results -->

The gateway's own cost per message, by stage:

<!-- results:stage-summary -->
| Gateway | Message | Stage | p50 |
|---|---|---|---|
| ours | ExecutionReport in | framing and decoding | 305–306 |
| ours | ExecutionReport in | dictionary check (note 006) | 786–1,316 |
| ours | ExecutionReport in | order book (note 007) | 299–305 |
| ours | ExecutionReport in | session, the whole inbound path | 1,273–1,279 |
| ours | ExecutionReport in | journaled session (book, journal C), inbound | 1,830–1,899 |
| ours | NewOrderSingle out | body checks (note 007) | 1,238–1,315 |
| ours | NewOrderSingle out | order book check and record (note 007) | 322–323 |
| ours | NewOrderSingle out | session send (encode, store) | 153–155 |
| ours | NewOrderSingle out | journaled session send (checks, book, encode, journal C) | 1,855–1,870 |
| QuickFIX 1.16.0 | ExecutionReport in | FIX::Message from the bytes, the FIX 4.4 dictionary validating | 2,057–2,060 |

Ns per call, in process, range over the runs, clock cost included. Method: [RESULTS.md](RESULTS.md).
<!-- /results -->
