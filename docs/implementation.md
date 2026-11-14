# Implementation sequence

The work follows five dependent stages:

1. Establish CMake, pinned sources, a controlled target, and a validated controller-agent handshake.
2. Collect real Rtl events, preserve hook semantics, correlate allocation generations, and expose transport loss.
3. Add bounded remote reads, strings, snapshots, offline storage, and a separate memory map.
4. Connect explainable rules and the docked desktop workflow to collected data.
5. Run native validation, measure overhead and loss, capture real renders, package binaries, and integrate the source.

All five stages are implemented. Version 0.2 adds an independent collector, ordered queue handoff, rolling sample retention, immutable capture payloads, broader Unicode extraction, optional bounded native stacks, and failure-path and desktop interaction tests. Delivery evidence, performance results, and remaining validation limits are recorded in validation.md. This sequence is not a claim of exhaustive Windows compatibility or detector accuracy.
