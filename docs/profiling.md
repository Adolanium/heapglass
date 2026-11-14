# Investigate allocation sites and growth

## Find sites holding memory

Open **Investigation > Allocation sites** during capture or after opening a saved session. The view groups retained allocation generations by their last observed allocation or reallocation site. Groups with the most known live requested bytes appear first.

With optional stacks enabled, a group uses the entire bounded address sequence. Different outer frames remain separate even when the immediate caller matches. With stacks disabled or unavailable, records fall back to caller-only groups. Missing callers receive an explicit unavailable group. Addresses are session-local; groups do not identify the same function across separate runs or different ASLR layouts.

Select a group to filter the allocation table to that exact site and inspect an example record. Bounded-stack groups also open the Inspector's Stack tab. Use **Clear site filter** above the allocation table to restore the other records. Hover a site to inspect its complete saved frame sequence and available live symbols.

The columns distinguish known live blocks, requested live bytes, retained freed generations, uncertain blocks and bytes, and the oldest known observed age. Freed totals describe retained records, not cumulative allocation traffic. Requested bytes are not usable heap size, physical residency, or the process working set.

Age uses a known allocation-event return timestamp. Records with unknown birth do not contribute to the oldest-age value; the age tooltip reports their count. Offline age ends at the latest retained event, capture, size-history, or snapshot observation. It never advances using the current machine's QPC clock. Lost and evicted records remain outside these totals.

## Compare growth across snapshots

Capture at least two named snapshots, then open **Investigation > Snapshot growth**. The view uses all retained snapshots in acquisition order, up to the existing four-snapshot limit. A is the first retained observation and B is the last. It shows live counts and requested bytes, byte changes, allocations present in every observation, and the number of intervals with increasing bytes.

Three or more observations can produce a **Repeated growth** lead when a site's known live requested bytes strictly increase in every interval. Every snapshot must belong to the same observation session, have complete history, and have a non-overlapping acquisition interval. Any uncertain record in that site's observations or an overflowing byte total suppresses the lead. Two observations show the difference without claiming repeated growth.

The survivor count requires the same session, heap generation, and allocation generation to be known live in every retained snapshot. An address reused by another generation is not a survivor. Survivor bytes use the final requested size and belong to the final observed site. Incomplete or incompatible histories display unknown survivor values.

Select a growth row to inspect an example in its final snapshot, or its first snapshot when the site no longer exists in the final observation. Existing byte-level snapshot comparisons remain under **Compare snapshots**.

Growth is an investigation lead, not proof of a leak. Legitimate caches and workload changes can retain memory. Reallocation can change a record's site and redistribute totals. Snapshots summarize observed retained allocation state over acquisition intervals; they do not freeze the target or establish an atomic heap census. Missing records, pre-attach allocations, and retention eviction limit the conclusion.

## Export the evidence

Use **Session > Export allocation sites CSV** or **Export snapshot growth CSV**. Both exports work offline. Site exports include observation time, coverage, frame addresses, counts, byte totals, and birth provenance. Growth exports include snapshot names, intervals, coverage, baseline and final totals, survivors, and trend evidence. Frame addresses use decimal integers separated by semicolons. Potential spreadsheet formulas in text fields are neutralized.

Byte sums saturate at the unsigned 64-bit limit and carry an explicit overflow flag. Overflowing totals cannot produce a repeated-growth lead. These derived reports require no IPC or saved-session format change; session formats 1 and 2 remain readable.
