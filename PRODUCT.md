# Heapglass

Heapglass is a local Windows heap and process-memory inspection tool for analysts. Its primary workflow is selecting a process, observing allocation events, sampling content, comparing snapshots, and investigating explainable findings.

## Platform and stack

Windows 11 x64, C++20, MSVC, Windows SDK, CMake, Dear ImGui, Win32, Direct3D 11, and Microsoft Detours. Native x64 targets only. No account or cloud service.

## Product constraints

The interface must distinguish allocation events from sampled content and identify incomplete history. Address reuse does not preserve allocation identity. Findings report evidence rather than definitive malware classifications. The compact dark interface puts navigation left, allocations in the center, evidence right, and investigation tabs below. These facts come from the project brief.
