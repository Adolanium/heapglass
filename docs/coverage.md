# Collection, identities, and limits

## Components

`Heapglass.exe` contains the Win32/DX11 interface, a controller worker, and the analysis engine. `HeapglassAgent.dll` contains the Detours hooks and event producer. `HeapglassTarget.exe` generates controlled workloads. `HeapglassTests.exe` exercises the pure engine and the real controller-agent path.

The controller worker owns collection state, remote reads, memory-map polling, symbol resolution, and file operations. The interface receives immutable model copies at roughly 250 ms intervals. It never resolves symbols or reads the target from a hook or rendering callback. Tables clip rows; event application is incremental. Filtering and sorting run on the presentation thread, so very large retained sets can affect frame time.

## Instrumentation coverage

The canonical layer is `ntdll`'s `RtlAllocateHeap`, `RtlReAllocateHeap`, `RtlFreeHeap`, `RtlCreateHeap`, and `RtlDestroyHeap`. Heap wrappers are not hooked again. Nested calls within one intercepted operation are suppressed by a fixed Win32 TLS slot. This avoids counting the operation twice. The agent's initialization uses the same exclusion. Fixed TLS slots avoid allocation during callbacks and remain accessible during loader TLS teardown.

Hooks pass the original arguments unchanged and return the original result. Structured exception cleanup publishes the operation outcome while allowing the original exception to propagate. Caller-visible last-error values are saved before instrumentation and restored before the original call and after recording its outcome. Callbacks do not sample memory, format text, allocate buffers, perform synchronous IPC, write files, or resolve symbols. Stack capture is not implemented; the caller return address is recorded.

Custom allocators, direct virtual-memory allocation, allocations completed before instrumentation, and allocations during recording gaps are outside heap coverage. Heapglass does not parse undocumented heap layouts, run a remote HeapWalk, or claim a baseline of existing allocations. Unknown frees remain visible as events. A successful reallocation of a pre-existing allocation creates a record with unknown birth.

## Transport and concurrent ordering

Protocol version 1 uses a fixed-size shared mapping, 32,768 preallocated slots, and 88-byte event records. Each 128-byte slot has an Interlocked sequence counter. The mapping header validates its magic, layout, version, PID, process creation time, session identity, and QPC frequency. Addresses in records are integers used only with remote-memory APIs.

A producer reserves its slot before calling the original heap function and publishes the completed record afterward. The consumer cannot pass an unpublished reservation. This prevents a dependent free from overtaking an allocation record stalled before publication. Entry and completion sequence numbers independently describe each operation's interval. Completion timestamps or arrival order do not establish identity.

Calls can overlap while an address is freed internally and reused before the free hook returns. The engine compares entry and completion intervals. A delayed free whose interval overlaps a newer allocation marks that record uncertain and does not retire it. Reallocation and heap destruction use the same conservative rule. Ambiguous correlations remain uncertain instead of receiving an invented total order.

Reservation uses at most 16 CAS attempts. Full or contended buffers drop the event and let the target continue. At completion, a dropped operation advances the cumulative loss count. When the controller reads a changed count, it takes the current global operation clock as a conservative loss watermark. The engine invalidates existing live state and records beginning at or before the watermark. Later observed allocations can establish new lifetimes. A stalled or terminated producer can block consumption behind its reservation, but cannot block other target threads from executing their original heap calls; subsequent overflow is counted.

The control channel consists of recording, disconnected, readiness, error, and heartbeat fields. If the controller heartbeat expires after three seconds, a hook disables recording. The controller reports the resulting gap. Stops, reconnects, loss, and retention eviction mark history incomplete. Snapshot comparisons conservatively classify incomplete histories as uncertain.

## Allocation identity

An identity contains the observation session, heap generation, and allocation generation. A new controller observation gets a fresh random session identity, including reconnects to an already resident agent. Heap creation establishes a generation; a previously unseen heap receives an observed generation without a fabricated creation history.

In-place reallocation preserves identity and appends requested-size history. Moving reallocation retires the previous record and creates a new generation with a parent link and size history. Failed reallocations preserve the original record. Successful zero-size operations use the native result without C realloc assumptions. Successful heap destruction retires known outstanding records unless concurrent ordering is ambiguous. Requested sizes are not presented as usable sizes.

The displayed lifetime is the interval between observed allocation and retirement returns, when both are known. Exact internal heap mutation times are not measured. Open or pre-existing lifetimes display unknown. Retention can remove records and history; missing records are not inferred to have been freed.

## Content and snapshots

The controller uses VirtualQueryEx and ReadProcessMemory. It excludes uncommitted, no-access, and guard pages, bounds reads to the allocation budget and queried region, and preserves partial-read and truncation status. Global operation counters, active hook counts, transport drainage, loss accounting, and allocation identity are checked around a read. Concurrent heap activity invalidates attribution. This intentionally sacrifices samples under heavy activity.

These checks do not freeze ordinary writes. A capture is a non-atomic acquisition interval, and a brief change between samples can be missed. Freed records retain historical captures with their original timestamps and are never reread. ASCII and printable UTF-16LE candidates include exact offsets. UTF-16LE candidates start with a Latin character and validate surrogate pairs; full multilingual text discovery and UTF-8 extraction are not implemented.

Entropy is empirical Shannon entropy over the captured range. Findings require at least 256 bytes, where eight bits per byte is the maximum empirical entropy. Smaller captures display insufficient evidence. FNV-1a64 is a non-cryptographic change hint, not an integrity or encryption proof. Snapshot byte comparisons use actual bytes over equal retained ranges and allocation generations.

Snapshots retain acquisition intervals and record-level capture times. The interface reports how many records were sampled during acquisition. Other retained captures remain historical. Comparisons show added, freed, resized, modified, unchanged, and uncertain states, with changed offsets for compatible ranges. Missing, unretained, differently sized captured ranges and incomplete histories remain uncertain.

## Memory map and findings

The process map is separate from the heap table and retains committed private, mapped, and image regions even without heap association. VirtualQueryEx memory type, Toolhelp loaded-module association, and GetMappedFileName backing association remain separate facts. The map is polled about once a second. Protection transitions compare matching region ranges, allocation bases, and memory types; mapping reuse between polls remains possible.

Rules report high entropy, large byte or entropy changes, repeated sampled transformations, private executable regions, writable/executable regions, sampled writable-to-executable transitions, and executable regions without a known loaded image. A low-confidence crypto-related hypothesis requires repeated transformations temporally associated with high-entropy samples of the same buffer. No rule identifies an algorithm, key rotation, shellcode, or a malicious process. Rule versions, evidence, addresses, acquisition times, and qualitative confidence accompany findings.

## Resource limits

Defaults are 4 KiB per allocation, 1 MiB per scan, 32 MiB of live retained bytes, a one-second sample interval, and four-character strings. Selected watched allocations take priority over the round-robin scan. Repeatedly changing the watch list can reduce coverage of other allocations.

Retention caps are 50,000 allocation records, 50,000 heap entries, 10,000 events, eight captures and 64 size-history entries per allocation, and four snapshots with at most 8 MiB of bytes each. These limits bound data, not the exact resident set; model publication, parsed JSON, strings, snapshots, and graphics have additional memory cost. Byte retention stops at its session budget instead of silently evicting captures from unrelated allocations. Lowering a budget does not erase bytes already retained.

JSON sessions are versioned, limited to 128 MiB and 24 nesting levels, and validate counts, enum values, session identities, ranges, offsets, encoded string extents, and retained-byte hashes. Imports retain at most 64 MiB of raw bytes across live records and snapshots. Exports use a temporary file and replacement. CSV exports allocation metadata and capture provenance; the JSON session contains the richer history, strings, regions, and snapshots. Findings are recomputed from saved observations and saved thresholds.

## Permissions and lifecycle

Process enumeration requests limited query access. Attach requests query, memory read/write/operation, create-thread, and synchronization rights required for injection and inspection. No debug privilege is enabled automatically. Named transport and ownership objects grant access only to the controller's user and SYSTEM. One controller owns the consumer lease. This is local instrumentation, not a hardened trust boundary against an adversarial process running as the same user.

DllMain does no substantive work. The exported initializer opens and validates the mapping and installs all hooks in a Detours transaction. It enlists existing threads, then repeats enumeration until no unenlisted threads remain, with bounded retries and a thread-count limit. Failure aborts the transaction and reports a Windows error. Remote initialization timeout never terminates a thread that may hold loader locks. The DLL path allocation is retained on that rare timeout because the remote loader may still use it.

Disconnect disables recording and releases controller handles. The agent retains the mapping, TLS slot, DLL, and trampolines until target exit. No detach or FreeLibrary race is attempted. Reattachment reuses the resident agent and starts a new, explicitly incomplete observation session. An unavailable fixed TLS slot, denied thread access, or unsupported architecture produces an initialization error.

Keyboard navigation, resizable docking, initial DPI scaling, DPI-change handling, and persistent ImGui layouts are implemented. The current validation did not exercise a screen reader or a multi-monitor DPI transition; there is no native UI Automation accessibility provider.
