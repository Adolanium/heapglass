# Sustained investigation validation

The workflow harness exercises a controlled investigation over minutes. It runs four private-heap worker threads, each performing 16 allocation/free pairs and a reallocation every third allocation, then sleeping 25 ms. A separate heap retains a new 4 KiB buffer every five seconds until target exit. This is paced application activity, not a maximum allocator-throughput benchmark.

Run the local sustained pair with:

```powershell
.\build\bin\Release\HeapglassWorkflowTests.exe --sustained 120 > sustained.json
```

The first trial runs without instrumentation. The second enables eight-frame stacks, ordinary one-second sampling, and timed metadata observations every ten seconds. It pins the initial baseline, saves a session halfway through capture, and verifies that timed capture stops on target exit. The pair takes about four minutes, including fixture startup. Each trial is one run on a shared desktop; it does not establish statistical performance bounds or compatibility with arbitrary applications.

The harness reads a separate fixture log to verify retained addresses, heap identity, requested sizes, and counts in each unambiguous snapshot. It matches the growth row to that exact allocation site and verifies its byte totals and surviving generations. Logs within 50 ms of an observation cutoff are excluded from these independent comparisons. The controller and analysis engine never read the fixture log.

A controlled 300 ms collector stall tests snapshot cutoff handling without forcing overflow. A snapshot requested during that backlog must be incomplete while the session's allocation coverage remains intact. Ordinary timed observations can become comparable again after the backlog clears. The short CI variant also simulates recording stopping while the initial baseline waits for a cutoff; the schedule must remain stopped and the fixture must continue running.

The raw results in [sustained.json](sustained.json) include target operations and CPU time, controller CPU time, event loss, published queue backlog, published event age, attributed and unsafe captures observed by the harness, and one-second controller working/private memory samples. Published event age includes publication delay and idle time; it is not a direct per-event ingestion latency. Capture counts reflect observations made by the harness, not an exhaustive read audit. Controller memory includes the effects of constructing and saving a large session.

The ordinary CTest workflow uses an eight-second instrumented fixture, faster retained growth, and 1.3-second timed observations. The longer paired measurement is run separately rather than adding four minutes to every CI build.

## Measured pair

On November 14, 2026, on the Windows 11 host documented in [validation](validation.md), both trials completed their 120-second workload. The instrumented trial observed 577,051 events with zero drops, verified four snapshots against the independent fixture log, and verified repeated growth for the retained-buffer site. Both trials retained 24 buffers.

| Measurement | Baseline | Instrumented investigation |
| --- | ---: | ---: |
| Fixture heap operations | 578,330 | 576,724 |
| Paced operations per second | 4,818 | 4,805 |
| Target CPU seconds | 0.234375 | 2.015625 |
| Controller test-host CPU seconds | 0.140625 | 23.093750 |
| Sampled peak controller working bytes during capture | 6,606,848 | 369,881,088 |

Pacing dominates throughput in this workload. Similar operation rates do not imply negligible instrumentation overhead; target CPU increased substantially with stacks enabled. Controller test-host CPU includes report publication, saving, and measurement polling.

The harness observed 10,542 attributed captures and 454 unsafe captures, an attributed fraction of 95.87 percent. Peak published pending events were 4,632. Maximum published event age was 1,254 ms, including publication and save delay. The controlled backlog probe was detected without event loss.

During the last 30 seconds, controller working memory ranged from 171,368,448 to 172,052,480 bytes, a spread below 0.7 MiB around 164 MiB. Private memory ranged from 196,861,952 to 197,341,184 bytes. The temporary working-set peak during capture was about 353 MiB. These samples establish a plateau for this run; they do not bound every workload or include GPU/UI memory and the later offline reload verification.
