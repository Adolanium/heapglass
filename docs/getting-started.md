# Inspect a process with Heapglass

## Run a packaged build

Extract the Windows x64 ZIP and run `Heapglass.exe`. Keep the extracted files together, including `HeapglassAgent.dll`. The package includes the test target, documentation, and third-party license notices.

To build from source, follow [Build and package Heapglass](building.md).

## Start an investigation

Use **Session > Launch test target**, **Launch executable**, or **Attach to process**. Launch accepts arguments and a working directory. The test target waits 1.2 seconds before its workload to permit a controlled launch test.

For a longer demonstration, run `Heapglass.exe --test-target` to open the ordinary interface against a 180-second fixture.

Heapglass observes allocations from instrumentation onward. Launch allows the process loader to run before injection, so startup allocations can precede observation. See [coverage and limitations](coverage.md) before interpreting missing allocations or findings.

## Inspect allocations and sampled content

Select an allocation to inspect captured hex, strings, size history, caller symbols, and findings. Use the string filter to search captured text.

Use **Sample now** or prioritize selected allocations. Configure sample budgets, retention mode, interval, string length, optional stack frames, and rule thresholds under **Limits & rules**.

Rolling retention evicts the oldest captures at capacity. Stop-at-capacity mode skips new samples instead. The session panel reports evictions, skipped samples, queue backlogs, and event loss.

## Capture and compare snapshots

Enter a name and select **Capture named snapshot**. Enable **Metadata-only snapshot** to avoid an extra memory scan and exclude sampled content from that snapshot. Background sampling continues under its own limits.

Use **Pin** beside a snapshot to preserve one baseline while newer observations roll through the four-snapshot limit. To acquire observations automatically, set **Timed interval (s)** and enable **Timed metadata snapshots**. The interval is 1 to 600 seconds. Recording stop, disconnect, and target exit stop the timer.

Compare snapshots in the bottom panel. Counts distinguish samples acquired during content acquisition from older retained captures.

## Investigate allocation sites and growth

Open **Investigation > Allocation sites** to rank outstanding requested bytes by bounded stack or caller. Select a group to filter its allocations.

Capture multiple snapshots and open **Investigation > Snapshot growth** to compare counts, bytes, and surviving generations. Three clean observations can establish repeated growth as an investigation lead. Both reports support offline inspection and CSV exports. See [allocation sites and growth](profiling.md) for interpretation and export details.

## Inspect the process memory map

Open the separate process memory map from the left panel. It reports committed private, mapped, and image regions, their protections, and available module and mapped-file associations.

## Save and reopen an investigation

Extracted text remains in session JSON and may contain sensitive process data, even when raw sampled bytes are excluded.

Use **Session > Save session / export JSON** to save an investigation. Raw sampled bytes are excluded by default. Enable **Include sampled bytes in JSON** to retain them.

Use **Session > Open saved session** for offline inspection or **Export allocation CSV** for allocation metadata. Saved symbol evidence belongs to that session. See [session compatibility](coverage.md#session-compatibility) before opening new saves in older builds.

## Control recording and display

**Stop recording** stops event production while the target continues. **Pause display** freezes presentation while ingestion continues.

**Disconnect** leaves the target operational. Installed hooks and their transport remain resident until target exit. Disconnection does not unload the DLL.
