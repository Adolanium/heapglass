# Native validation and measurements

Validation ran on November 14, 2026, on Windows 11 Pro 10.0.26200, an Intel Core i7-10700K with 16 logical processors, MSVC 19.51.36260, Windows SDK 10.0.28000, and CMake's Visual Studio 2026 x64 generator.

## Native tests

All application targets compile as C++20 x64 Release binaries. Reproduce the build and nine CTest groups with:

```powershell
cmake --build build --config Release --parallel 8
ctest --test-dir build -C Release --output-on-failure
```

The full local run passed all nine groups in 53.94 seconds. A subsequent UI regression also verifies that Stack and History remain accessible for allocations with no captured bytes.

| Group | Exercised behavior |
| --- | --- |
| strings | ASCII, validated UTF-8, both UTF-16LE alignments, multilingual starts, surrogate pairs, exact offsets, malformed input, scalar minimum lengths, rejected controls and private use |
| retention | Rolling byte eviction, stop-at-capacity status, immutable presentation bytes, immediate budget reduction, bounded record eviction after event loss |
| collector-handoff | A latch pauses the actual source-to-inbox transfer. The source remains non-idle until destination publication; the transferred free retains its literal address and result |
| bounded-stacks | Eight-frame native capture, committed frame addresses, settings toggles, saved stacks, and forced allocator reentry that disables capture, counts loss, and leaves the target operational |
| core | Allocation/reallocation/free semantics, heap generations, delayed reuse, uncertain lifetimes, recording watermarks, transport reservations, concurrent producers, forced overflow, snapshots, findings, session formats 1 and 2, malformed imports, and CSV neutralization |
| live | Real injection, independently logged fixture address and API semantics, caller last-error and exception behavior, exact multilingual strings, executable pages, snapshots, save/load, recording gaps, disconnect, resident reattach, overflow, and target exit |
| desktop-ui | Real queued ImGui input for filtering and clearing, ascending/descending size sorting, allocation/string/snapshot navigation, keyboard focus traversal, and dock splitter dragging |
| native-dialogs-dpi | Real Unicode Open/Save selection and cancellation on an unshown private Windows desktop, TEMP cleanup, and synthetic WM_DPICHANGED at 100/150/200 percent and back to 100 percent |
| failures | Restricted-process access denial, mapping identity mismatch, partial hook-install rollback, bounded reads across inaccessible and guard pages, reconnect, controller crash, and incompatible resident agent |

Failure tests create their own processes and mappings. Crash validation terminates only its capture helper, waits for heartbeat expiry, and independently counts target allocation batches to prove continued progress. The test-only fault DLLs are excluded from the binary package.

The fixture also produces compressed and seeded random buffers, CNG AES changes, multiple heaps, concurrent threads, failed and in-place reallocations, heap destruction, and sampled protection transitions. Not every fixture branch has an independent live assertion. No tests send global desktop input or switch the user's input desktop.

Two concurrency regressions have dedicated coverage. A paused collector must not make both queues appear empty before publication. Delayed operations from an earlier recording interval must not receive newly certain birth times. Allocation eviction uses an ordered retirement index so overflow recovery does not repeatedly scan every retained uncertain record.

## Instrumentation benchmark

The benchmark runs 200,000 HeapAlloc/HeapFree pairs on one private heap and one thread. Sizes cycle through 64..1087 bytes. Each measurement has three baseline and three instrumented trials, without fixture ground-truth logging. Optional stacks are disabled. The 1.2-second startup delay is excluded from throughput timing. CPU measurements include startup, initialization, and controller processing.

The raw pre-change trials are in [performance-before-v2.json](performance-before-v2.json). Final trials are in [performance.json](performance.json). The larger bounded queues exchange memory for lower burst loss. Finite queues still permit loss under sustained load or a stalled producer, and forced-overflow tests verify explicit uncertainty.

| Median instrumented measurement | Before | Version 0.2 |
| --- | ---: | ---: |
| Target heap operations per second | 6,543,175 | 6,918,907 |
| Events observed | 98,384 | 400,119 |
| Events dropped | 301,735 | 0 |
| Target CPU seconds | 0.265625 | 0.250000 |
| Controller CPU seconds | 0.078125 | 0.218750 |
| Target observed peak working bytes | 9,388,032 | 11,501,568 |
| Controller sampled peak working bytes | 25,403,392 | 132,308,992 |

All three final instrumented trials observed 400,119 events with zero target or controller losses. Median target throughput rose 5.7 percent against the pre-change measurement. The controller analyzed about four times as many events and used more CPU and memory. Its final sampled working set was about 126 MiB. This result does not establish lossless capture at arbitrary event rates.

Uninstrumented median throughput was 22.05 million operations per second before and 23.01 million in the final run. Final instrumented throughput was 30.1 percent of its uninstrumented baseline, so heap instrumentation still imposes substantial target overhead.

Final median pipeline times were 18.94 ms for collection, 177.18 ms for event application, 11.54 ms for publication, 0.017 ms for sampling, and 2.19 ms for symbols. The median peak inbox held 269,047 events. These timings accumulate work on different threads and must not be summed as end-to-end latency.

These are short microbenchmarks on a shared desktop, not isolated laboratory measurements. CPU accounting has coarse time resolution, and 10 ms process-memory polling can miss brief peaks. CPU percentages use one core as 100 percent; divide by 16 for machine-wide percentages on this host. Results do not establish overhead for arbitrary applications, enabled stacks, or high thread counts.

## Desktop evidence

The PNGs under [screenshots](screenshots) are actual DX11 back-buffer captures against the controlled target. They contain no fabricated rows. The capture run explicitly enables eight stack frames and exports the allocation table, separate process memory map, and allocation Stack tab. Raw CPU draw-time and frame-time values are in [ui-profile.json](ui-profile.json). Draw time measures interface construction, not GPU execution or end-to-end input latency.

The final run measured 402 frames after warmup, mean CPU draw time 1.59 ms, p95 3.99 ms, maximum 22.91 ms, and mean frame time including presentation 32.70 ms. See the [allocation view](screenshots/allocations.png), [memory map](screenshots/memory-map.png), and [stack view](screenshots/stack.png).

Native interaction tests use an offscreen ImGui context and an unshown private desktop. They establish the documented gestures and dialog behavior without physical desktop interaction. Synthetic DPI messages validate the shared application handler, suggested window bounds, font scaling, and style scaling. They do not establish physical multi-monitor behavior.

## Remaining limits

Actual protected-process policy combinations, different-user permissions, unsupported x86 and ARM targets, forced initialization timeouts, hostile target tampering, actual PID reuse, physical multi-monitor transitions, and screen-reader behavior remain untested. Access-denial and identity-mismatch fixtures exercise the corresponding controller failures without claiming every Windows policy combination.

The interface has no native UI Automation accessibility provider or full RTL shaping. Unicode extraction uses an explicit candidate repertoire; installed fonts may not display every accepted scalar. A pre-attach baseline and safe agent unloading remain unimplemented. Restart the target to replace an incompatible resident DLL.

The tests validate deterministic observations and conservative terminology on a known fixture. They do not measure detector precision, recall, encryption classification, or arbitrary malware detection. Sampling remains non-atomic and can miss ordinary writes between captures.
