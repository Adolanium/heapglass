# API and analysis references

These references informed the implementation. The Rtl declarations are verified against the user-mode interface, not inferred from the names of Heap wrappers.

- [Microsoft Detours usage](https://github.com/microsoft/Detours/wiki/Using-Detours): matching signatures, transactions, trampolines, and thread enlistment.
- [Dear ImGui Win32/DX11 example](https://github.com/ocornut/imgui/blob/v1.91.9b-docking/examples/example_win32_directx11/main.cpp): native window and renderer lifecycle.
- [Microsoft x64 calling convention](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention): register arguments, stack alignment, shadow space, and return values. `NTAPI` maps to the Windows x64 ABI on this build.
- [Microsoft RtlAllocateHeap documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-rtlallocateheap): flag, size, and result semantics.
- [PHNT user-mode Rtl declarations](https://github.com/winsiderss/phnt/blob/master/ntrtl.h): all five native heap signatures. This is a maintained third-party declaration source, not a Microsoft ABI guarantee.
- [HeapReAlloc](https://learn.microsoft.com/en-us/windows/win32/api/heapapi/nf-heapapi-heaprealloc): failed reallocation preserves the original allocation, and the in-place flag has specific semantics. A zero-size request is not treated as C `realloc`.
- [VirtualQueryEx](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualqueryex) and [ReadProcessMemory](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-readprocessmemory): page query, protection, readable spans, and partial reads.
- [GetProcessHeaps](https://learn.microsoft.com/en-us/windows/win32/api/heapapi/nf-heapapi-getprocessheaps) and [HeapWalk](https://learn.microsoft.com/en-us/windows/win32/api/heapapi/nf-heapapi-heapwalk): these operate on the calling process. Heapglass does not use them against a remote process.
- [DLL lifecycle guidance](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-best-practices): loader-lock constraints. Initialization is an explicit exported function called outside DllMain.
- [Entropy classification limitations](https://arxiv.org/abs/2010.07754): compression and encryption can both produce high-entropy observations.

## User-mode ABI check

The agent declares allocation and reallocation returns as `PVOID`, heap creation as six arguments returning `PVOID`, and destruction as a pointer return where NULL means success. Windows 8 and later user-mode `RtlFreeHeap` uses `LOGICAL`, an unsigned 32-bit return, rather than the `BOOLEAN` declaration associated with some kernel headers. The interceptor preserves the full `ULONG` result.

On Windows 11 build 26200, debugger inspection of the actual `ntdll!RtlFreeHeap` export confirmed register arguments RCX, EDX, and R8 and a 32-bit result path. The live fixture calls the user-mode exports directly and checks last-error preservation across successful allocate, shrink, and free operations, plus propagation of `STATUS_NO_MEMORY` under `HEAP_GENERATE_EXCEPTIONS`. These checks do not guarantee ABI compatibility with future Windows releases.
