# Native validation and measurements

Validation ran on November 14, 2026, on Windows 11 Pro 10.0.26200, an Intel Core i7-10700K with 16 logical processors, MSVC 19.51.36260, Windows SDK 10.0.28000, and CMake's Visual Studio 2026 x64 generator.

## Implemented, compiled, and exercised

All application targets compile as C++20 x64 Release binaries. The local CTest run passed both `core` and `live`. The live suite took about 31 seconds. Build output contained no project warnings after explicit protocol padding was added.

The core suite exercises successful and failed operations, zero-size native result handling, in-place and moving reallocations, parent lineage, heap generation reuse, heap destruction, pre-existing allocations, delayed frees, overlapping reuse, loss watermarks, unpublished transport reservations, concurrent producers, forced overflow, exact ASCII and UTF-16LE offsets, empirical entropy, historical captured bytes, snapshot byte differences, conservative region findings, saved-byte opt-in, malformed imported offsets, retained rule thresholds, and CSV formula-prefix neutralization.

The live suite launches and instruments the real target, compares a recorded allocation against an independently logged fixture address, checks direct user-mode Rtl call semantics, verifies caller last-error preservation and exception propagation, observes actual ASCII content and private executable pages, captures and reloads snapshots, stops and restarts recording, disconnects without terminating the target, reattaches to the resident agent, forces transport overflow, and observes clean target exit. The fixture also produces UTF-16LE, seeded random and compressed content, CNG AES changes, multiple heaps, concurrent threads, failed and in-place reallocations, heap destruction, no-access pages, guard pages, and sampled protection transitions. Not every fixture branch has an independent live assertion.

The initial instrumentation fault occurred during loader TLS teardown on thread exit. Debugger evidence identified access through a cleared compiler TLS vector. The agent now uses a preallocated fixed Win32 TLS slot. The live test creates and exits target threads while hooks are active.

## Instrumentation benchmark

The benchmark runs 200,000 HeapAlloc/HeapFree pairs on one private heap and one thread. Sizes cycle through 64..1087 bytes. It runs three baseline and three instrumented trials without fixture ground-truth logging. The 1.2-second startup delay is excluded from throughput timing. CPU measurements include startup, initialization, and controller processing. CPU percentages use one core as 100 percent, so machine-wide percentages on this host are these values divided by 16.

| Median measurement | Baseline | Instrumented |
| --- | ---: | ---: |
| Target heap operations per second | 22,020,369 | 6,589,829 |
| Target CPU seconds | 0.0156 | 0.2812 |
| Controller CPU seconds | 0.0000 | 0.0781 |
| Target CPU percent over full wall interval, one core | 1.27% | 20.70% |
| Controller CPU percent over full wall interval, one core | 0.00% | 5.75% |
| Target observed peak working set | 4.58 MiB | 8.94 MiB |
| Target sampled peak private usage | 1.21 MiB | 1.36 MiB |
| Controller sampled peak working set | 8.21 MiB | 27.57 MiB |

Median instrumented throughput was 29.9% of baseline throughput. This burst lost 75.4% to 75.4% of produced events. The finite ring preserves target progress, but this workload cannot produce a complete allocation history. The normal multi-threaded fixture used for screenshots and live assertions reported zero loss before the overflow test.

The raw six trials are in [performance.json](performance.json). These are short microbenchmarks on a shared desktop, not isolated laboratory measurements. CPU accounting has coarse time resolution, and 10 ms process-memory polling can miss brief peaks. Results do not establish overhead for arbitrary applications or high thread counts.

## Desktop render evidence

The PNGs under [screenshots](screenshots) are actual DX11 back-buffer captures against the controlled target. They contain no fabricated rows. A capture run showed 596 measured frames after warmup, mean CPU draw time 1.74 ms, p95 3.86 ms, maximum 11.65 ms, and mean frame time including presentation 16.78 ms. Raw values are in [ui-profile.json](ui-profile.json). Draw time measures interface construction, not GPU execution or end-to-end input latency. The run rendered the application without desktop input and exited through its normal cleanup.

A source-and-image interface review found and corrected wrong Inspector selection after string or snapshot navigation, incomplete column sorting, proportional hex text, and missing region observation times. A follow-up review scored those four findings resolved. ASCII requires horizontal scrolling at the default narrow Inspector width.

## Remaining validation limits

Protected-process denial, different-user permissions, unsupported x86 and ARM targets, forced partial hook-installation failures, forced initialization timeouts, hostile target tampering, process-ID reuse, and multi-monitor DPI transitions were not runtime-tested. The architecture and validation checks handle these paths, but the current fixture does not prove every Windows policy combination.

No interactive UI automation continued after it was stopped. Keyboard traversal, native file dialogs, dock dragging, sorting gestures, and screen-reader behavior were not exercised interactively. Source inspection and render captures do not establish those behaviors. The implementation has no native UI Automation accessibility provider.

The test harness validates deterministic observations and conservative terminology on its known fixture. It does not measure detector precision, recall, encryption classification, or arbitrary malware detection. Stack traces beyond the caller, UTF-8 extraction, a pre-attach baseline, and safe agent unloading are not implemented.
