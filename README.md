# Heapglass

Heapglass is a native Windows desktop app for inspecting heap allocations, tracking memory growth, and investigating changing buffers.

![Live allocation inspection](docs/screenshots/allocations.png)

## Features

- Live allocation, reallocation, and free events with sampled hex, extracted strings, size history, and findings.
- Allocation-site reports grouped by caller or optional bounded stacks, plus snapshot growth and survivor counts.
- Named content or metadata-only snapshots, one pinned baseline, and timed metadata observations.
- A process memory map with region protections and available module and mapped-file associations.
- Saved investigations for offline inspection, cached symbol and module evidence, and allocation, site, and growth CSV exports.

## Platform

Windows 11 x64 with native x64 target processes. Instrumentation coverage and compatibility details are documented in [coverage and limitations](docs/coverage.md).

## Documentation

| Document | Contents |
| --- | --- |
| [Getting started](docs/getting-started.md) | Run the app, inspect a process, capture snapshots, and save an investigation |
| [Build and package](docs/building.md) | Prerequisites, source builds, validation commands, and binary packages |
| [Allocation sites and growth](docs/profiling.md) | Site grouping, pinned baselines, growth evidence, and report exports |
| [Coverage and limitations](docs/coverage.md) | Collection semantics, resource limits, compatibility, and agent lifecycle |
| [Validation and performance](docs/validation.md) | Native test coverage, measured overhead, screenshots, and remaining limits |
| [Sustained validation](docs/sustained-validation.md) | A paired investigation with independent growth checks and resource measurements |
| [API references](docs/references.md) | Platform APIs and analysis references |

## Third-party notices

The source checkout includes dependency license notices and pinned checksums under `third_party`. Binary packages retain the notices under `licenses`. The Win32/DX11 lifecycle adapts Dear ImGui's licensed example.
