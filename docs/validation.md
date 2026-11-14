# Native validation and measurements

Validation ran on November 14, 2026, on Windows 11 Pro 10.0.26200, an Intel Core i7-10700K with 16 logical processors, MSVC 19.51.36260, Windows SDK 10.0.28000, and CMake's Visual Studio 2026 x64 generator.

## Native tests

All application targets compile as C++20 x64 Release binaries. Reproduce the build and ten CTest groups with:

```powershell
cmake --build build --config Release --parallel 8
ctest --test-dir build -C Release --output-on-failure
```

The full local version 0.3 run passed all ten groups in 55.49 seconds. Focused follow-up checks cover programmatic tab selection, group filtering, historical navigation, and exported reports. Stack and History remain accessible for allocations with no captured bytes.

| Group | Exercised behavior |
| --- | --- |
| strings | ASCII, validated UTF-8, both UTF-16LE alignments, multilingual starts, surrogate pairs, exact offsets, malformed input, scalar minimum lengths, rejected controls and private use |
| retention | Rolling byte eviction, stop-at-capacity status, immutable presentation bytes, immediate budget reduction, bounded record eviction after event loss |
| allocation-profiles | Full bounded-stack and caller-only grouping, live/freed/uncertain totals, offline age provenance, repeated growth, generation-based survivors, uncertain intervals, session mismatch, overflow, CSV escaping, and saved-session reconstruction |
| collector-handoff | A latch pauses the actual source-to-inbox transfer. The source remains non-idle until destination publication; the transferred free retains its literal address and result |
| bounded-stacks | Eight-frame native capture, committed frame addresses, settings toggles, saved stacks, and forced allocator reentry that disables capture, counts loss, and leaves the target operational |
| core | Allocation/reallocation/free semantics, heap generations, delayed reuse, uncertain lifetimes, recording watermarks, transport reservations, concurrent producers, forced overflow, snapshots, findings, session formats 1 and 2, malformed imports, and CSV neutralization |
| live | Real injection, independently logged fixture address and API semantics, caller last-error and exception behavior, exact multilingual strings, executable pages, snapshots, save/load, recording gaps, disconnect, resident reattach, overflow, and target exit |
| desktop-ui | Real queued ImGui input for filtering and clearing, ascending/descending size sorting, allocation/string/snapshot navigation, site filtering and clearing, profile-tab selection, repeated-growth evidence, report exports, keyboard focus traversal, and dock splitter dragging |
| native-dialogs-dpi | Real Unicode Open/Save selection and cancellation on an unshown private Windows desktop, TEMP cleanup, and synthetic WM_DPICHANGED at 100/150/200 percent and back to 100 percent |
| failures | Restricted-process access denial, mapping identity mismatch, partial hook-install rollback, bounded reads across inaccessible and guard pages, reconnect, controller crash, and incompatible resident agent |

Failure tests create their own processes and mappings. Crash validation terminates only its capture helper, waits for heartbeat expiry, and independently counts target allocation batches to prove continued progress. The test-only fault DLLs are excluded from the binary package.

The fixture also produces compressed and seeded random buffers, CNG AES changes, multiple heaps, concurrent threads, failed and in-place reallocations, heap destruction, and sampled protection transitions. Not every fixture branch has an independent live assertion. No tests send global desktop input or switch the user's input desktop.

Two concurrency regressions have dedicated coverage. A paused collector must not make both queues appear empty before publication. Delayed operations from an earlier recording interval must not receive newly certain birth times. Allocation eviction uses an ordered retirement index so overflow recovery does not repeatedly scan every retained uncertain record.

## Instrumentation benchmark

The benchmark runs 200,000 HeapAlloc/HeapFree pairs on one private heap and one thread. Sizes cycle through 64..1087 bytes. Each measurement has three baseline and three instrumented trials, without fixture ground-truth logging. Optional stacks are disabled. The 1.2-second startup delay is excluded from throughput timing. CPU measurements include startup, initialization, and controller processing.

The raw pre-change trials are in [performance-before-v2.json](performance-before-v2.json). Version 0.2 trials are in [performance-v0.2.json](performance-v0.2.json), and version 0.3 trials are in [performance.json](performance.json). The larger bounded queues exchange memory for lower burst loss. Finite queues still permit loss under sustained load or a stalled producer, and forced-overflow tests verify explicit uncertainty.

| Median instrumented measurement | Before | Version 0.2 |
| --- | ---: | ---: |
| Target heap operations per second | 6,543,175 | 6,918,907 |
| Events observed | 98,384 | 400,119 |
| Events dropped | 301,735 | 0 |
| Target CPU seconds | 0.265625 | 0.250000 |
| Controller CPU seconds | 0.078125 | 0.218750 |
| Target observed peak working bytes | 9,388,032 | 11,501,568 |
| Controller sampled peak working bytes | 25,403,392 | 132,308,992 |

All three version 0.2 instrumented trials observed 400,119 events with zero target or controller losses. Median target throughput rose 5.7 percent against the pre-change measurement. The controller analyzed about four times as many events and used more CPU and memory. Its sampled working set was about 126 MiB. This result does not establish lossless capture at arbitrary event rates.

Uninstrumented median throughput was 22.05 million operations per second before and 23.01 million in the version 0.2 run. Version 0.2 instrumented throughput was 30.1 percent of its uninstrumented baseline, so heap instrumentation still imposes substantial target overhead.

Version 0.2 median pipeline times were 18.94 ms for collection, 177.18 ms for event application, 11.54 ms for publication, 0.017 ms for sampling, and 2.19 ms for symbols. The median peak inbox held 269,047 events. These timings accumulate work on different threads and must not be summed as end-to-end latency.

Version 0.3 retained zero losses and 400,119 observed events in each of its three instrumented trials. Median target throughput was 6.62 million operations per second. Controller CPU time was 0.3125 seconds, sampled peak working memory was 133,885,952 bytes, event application took 182.97 ms, and publication including allocation-site aggregation took 39.86 ms. This fixture has no snapshots, so it does not measure growth-report construction. Aggregation adds analysis work while the independent collector continues draining events.

These are short microbenchmarks on a shared desktop, not isolated laboratory measurements. CPU accounting has coarse time resolution, and 10 ms process-memory polling can miss brief peaks. CPU percentages use one core as 100 percent; divide by 16 for machine-wide percentages on this host. Results do not establish overhead for arbitrary applications, enabled stacks, or high thread counts.

## Desktop evidence

The PNGs under [screenshots](screenshots) are actual DX11 back-buffer captures against the controlled target. They contain no fabricated rows. The capture run explicitly enables eight stack frames and exports allocations, the separate process memory map, the Stack tab, allocation sites, and snapshot growth after acquiring three real snapshots. Raw CPU draw-time and frame-time values are in [ui-profile.json](ui-profile.json). Draw time measures interface construction, not GPU execution or end-to-end input latency.

See the [allocation view](screenshots/allocations.png), [memory map](screenshots/memory-map.png), [stack view](screenshots/stack.png), [allocation sites](screenshots/sites.png), and [snapshot growth](screenshots/growth.png). Growth in this fixture is not an independently verified leak; deterministic positive and uncertainty cases are covered by the profiling tests.

The final hidden-window run measured 2,547 frames after warmup, mean CPU draw time 1.73 ms, p95 2.90 ms, maximum 31.25 ms, and mean frame time including presentation 8.87 ms. Hidden-window presentation timing is not a visible-window refresh-rate guarantee.

Native interaction tests use an offscreen ImGui context and an unshown private desktop. They establish the documented gestures and dialog behavior without physical desktop interaction. Synthetic DPI messages validate the shared application handler, suggested window bounds, font scaling, and style scaling. They do not establish physical multi-monitor behavior.

## Remaining limits

Actual protected-process policy combinations, different-user permissions, unsupported x86 and ARM targets, forced initialization timeouts, hostile target tampering, actual PID reuse, physical multi-monitor transitions, and screen-reader behavior remain untested. Access-denial and identity-mismatch fixtures exercise the corresponding controller failures without claiming every Windows policy combination.

The interface has no native UI Automation accessibility provider or full RTL shaping. Unicode extraction uses an explicit candidate repertoire; installed fonts may not display every accepted scalar. A pre-attach baseline and safe agent unloading remain unimplemented. Restart the target to replace an incompatible resident DLL.

The tests validate deterministic observations and conservative terminology on a known fixture. They do not measure detector precision, recall, encryption classification, or arbitrary malware detection. Sampling remains non-atomic and can miss ordinary writes between captures.
