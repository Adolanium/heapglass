# Build and package Heapglass

## Prerequisites

Use Windows 11 x64 with:

- Visual Studio 2022 or newer with **Desktop development with C++** and a Windows SDK.
- CMake 3.24 or newer.
- Python 3 for dependency verification.

All dependency source files are vendored and pinned. Builds do not download libraries. The Release build statically links the MSVC runtime.

## Build and run from source

Run these commands from the repository root:

```powershell
python scripts/verify-dependencies.py
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
.\build\bin\Release\Heapglass.exe
```

Keep `HeapglassAgent.dll` beside `Heapglass.exe`. In the source checkout, `scripts/build.ps1` performs the same dependency, configuration, build, and test checks.

For the inspection workflow, see [getting started](getting-started.md).

## Run additional validation

Run these commands from the repository root after building Release binaries:

```powershell
.\build\bin\Release\HeapglassTests.exe --live
.\build\bin\Release\HeapglassTests.exe --benchmark > benchmark.json
.\build\bin\Release\Heapglass.exe --capture --profile
```

The `--live` check also runs in the CTest suite. Use the individual command to rerun that group. For the recorded results and their limits, see [validation and performance](validation.md).

`--capture` runs the controlled target with eight stack frames and renders a hidden window without desktop input. It exports allocation, memory-map, stack, allocation-site, and snapshot-growth PNGs beside the executable, then exits after about 24 seconds. It pins a metadata baseline and schedules observations every four seconds. The target continues until its 28-second workload finishes.

`--profile` writes CPU draw-time and frame-time measurements on clean UI exit. Hidden-window presentation timing is not a visible-window refresh-rate guarantee.

To run a paired sustained investigation:

```powershell
.\build\bin\Release\HeapglassWorkflowTests.exe --sustained 120 > sustained.json
```

This runs a 120-second baseline followed by a 120-second instrumented investigation with four allocating threads, eight-frame stacks, timed metadata snapshots, and a save during capture. See [sustained validation](sustained-validation.md) for the fixture, independent checks, and measured results.

## Package the binaries

Run these commands from the repository root:

```powershell
cmake --install build --config Release --prefix out/Heapglass
Compress-Archive -Path out/Heapglass/* -DestinationPath out/Heapglass-windows-x64.zip -Force
```

The package includes the app, agent, test target, core test executable, documentation, and dependency license notices. Test-only fault DLLs are excluded. Pinned dependency checksums and source license notices remain under `third_party` in the source checkout. Binary packages retain the notices under `licenses`.
