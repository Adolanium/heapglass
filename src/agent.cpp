#include "protocol.hpp"
#include <detours.h>
#include <intrin.h>
#include <tlhelp32.h>

namespace {
using Backtrace = USHORT(WINAPI *)(ULONG, ULONG, PVOID *, PULONG);
Backtrace real_backtrace = nullptr;
using Alloc = PVOID(NTAPI *)(PVOID, ULONG, SIZE_T);
using Realloc = PVOID(NTAPI *)(PVOID, ULONG, PVOID, SIZE_T);
// Windows 8+ user-mode RtlFreeHeap returns LOGICAL (ULONG), not the kernel BOOLEAN declaration.
using Free = ULONG(NTAPI *)(PVOID, ULONG, PVOID);
using Create = PVOID(NTAPI *)(ULONG, PVOID, SIZE_T, SIZE_T, PVOID, PVOID);
using Destroy = PVOID(NTAPI *)(PVOID);
Alloc real_alloc;
Realloc real_realloc;
Free real_free;
Create real_create;
Destroy real_destroy;
hg::Shared *shared = nullptr;
HANDLE mapping = nullptr;
DWORD depth_index = TLS_OUT_OF_INDEXES;
void set_depth(bool active) noexcept {
    TlsSetValue(depth_index, reinterpret_cast<void *>(static_cast<uintptr_t>(active)));
}
volatile LONG initialized = 0;
bool collecting() noexcept {
    if (shared && depth_index != TLS_OUT_OF_INDEXES) {
        auto guard = reinterpret_cast<uintptr_t>(TlsGetValue(depth_index));
        if (guard) {
            if (guard >= 2) {
                InterlockedIncrement64(&shared->stack_violations);
                if (guard == 3)
                    hg::dropped(shared);
            }
            return false;
        }
    }
    if (!shared || hg::atomic_read(&shared->ready) != 1 || hg::atomic_read(&shared->disconnected) ||
        !hg::atomic_read(&shared->recording))
        return false;
    if (GetTickCount64() - uint64_t(hg::atomic_read(&shared->heartbeat)) >= 3000) {
        if (InterlockedExchange(&shared->recording, 0))
            InterlockedIncrement64(&shared->epoch);
        return false;
    }
    return true;
}
struct Pending {
    hg::Event event;
    hg::Reservation reservation;
};
Pending begin(hg::Op op, void *heap, void *previous, SIZE_T size, ULONG flags, void *caller) noexcept {
    set_depth(true);
    hg::Event e{};
    e.begin = InterlockedIncrement64(&shared->clock);
    e.op = op;
    e.heap = reinterpret_cast<uint64_t>(heap);
    e.previous = reinterpret_cast<uint64_t>(previous);
    e.requested = size;
    e.thread = GetCurrentThreadId();
    e.flags = flags;
    e.caller = reinterpret_cast<uint64_t>(caller);
    auto reservation = hg::reserve(shared);
    if (!reservation.slot)
        InterlockedIncrement64(&shared->unbuffered_active);
    return {e, reservation};
}
void finish(Pending &pending, uint64_t result, bool abnormal) noexcept {
    DWORD error = GetLastError();
    auto &e = pending.event;
    e.result = result;
    e.address = e.op == hg::Op::free ? e.previous : e.op == hg::Op::destroy_heap ? e.heap : result;
    if (e.op == hg::Op::create_heap)
        e.heap = result;
    e.ticks = hg::ticks();
    if (abnormal)
        e.reserved = 1;
    auto count = hg::atomic_read(&shared->stack_frames);
    if (count > 0 && count <= hg::max_stack_frames && hg::atomic_read(&shared->stack_ready) == 1) {
        auto before = hg::atomic_read(&shared->stack_violations);
        TlsSetValue(depth_index, reinterpret_cast<void *>(3));
#if defined(HG_VALIDATE_UNSAFE_TRACE)
        auto unexpected = HeapAlloc(GetProcessHeap(), 0, 32);
        HeapFree(GetProcessHeap(), 0, unexpected);
#endif
        __try {
            PVOID frames[hg::max_stack_frames]{};
            e.stack_count = real_backtrace(2, static_cast<ULONG>(count), frames, nullptr);
            if (e.stack_count > static_cast<unsigned>(count))
                e.stack_count = 0;
            for (unsigned i = 0; i < e.stack_count; ++i)
                e.stack[i] = reinterpret_cast<uint64_t>(frames[i]);
            e.stack_status = e.stack_count ? 1 : 2;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            e.stack_count = 0;
            e.stack_status = 2;
            InterlockedExchange(&shared->stack_ready, -1);
        }
        if (hg::atomic_read(&shared->stack_violations) != before) {
            e.stack_count = 0;
            e.stack_status = 2;
            e.reserved |= 2;
            InterlockedExchange(&shared->stack_ready, -1);
            InterlockedExchange(&shared->stack_frames, 0);
        }
        set_depth(true);
    }
    e.end = InterlockedIncrement64(&shared->clock);
    if (pending.reservation.slot)
        hg::publish(pending.reservation, e);
    else
        hg::dropped(shared);
    if (!pending.reservation.slot)
        InterlockedDecrement64(&shared->unbuffered_active);
    set_depth(false);
    SetLastError(error);
}
PVOID NTAPI hook_alloc(PVOID heap, ULONG flags, SIZE_T size) {
    const DWORD error = GetLastError();
    if (!collecting()) {
        SetLastError(error);
        return real_alloc(heap, flags, size);
    }
    auto e = begin(hg::Op::allocate, heap, nullptr, size, flags, _ReturnAddress());
    PVOID result = nullptr;
    SetLastError(error);
    __try {
        result = real_alloc(heap, flags, size);
    } __finally {
        finish(e, reinterpret_cast<uint64_t>(result), AbnormalTermination() != FALSE);
    }
    return result;
}
PVOID NTAPI hook_realloc(PVOID heap, ULONG flags, PVOID ptr, SIZE_T size) {
    const DWORD error = GetLastError();
    if (!collecting()) {
        SetLastError(error);
        return real_realloc(heap, flags, ptr, size);
    }
    auto e = begin(hg::Op::reallocate, heap, ptr, size, flags, _ReturnAddress());
    PVOID result = nullptr;
    SetLastError(error);
    __try {
        result = real_realloc(heap, flags, ptr, size);
    } __finally {
        finish(e, reinterpret_cast<uint64_t>(result), AbnormalTermination() != FALSE);
    }
    return result;
}
ULONG NTAPI hook_free(PVOID heap, ULONG flags, PVOID ptr) {
    const DWORD error = GetLastError();
    if (!collecting()) {
        SetLastError(error);
        return real_free(heap, flags, ptr);
    }
    auto e = begin(hg::Op::free, heap, ptr, 0, flags, _ReturnAddress());
    ULONG result = 0;
    SetLastError(error);
    __try {
        result = real_free(heap, flags, ptr);
    } __finally {
        finish(e, result, AbnormalTermination() != FALSE);
    }
    return result;
}
PVOID NTAPI hook_create(ULONG flags, PVOID base, SIZE_T reserve, SIZE_T commit, PVOID lock,
                        PVOID parameters) {
    const DWORD error = GetLastError();
    if (!collecting()) {
        SetLastError(error);
        return real_create(flags, base, reserve, commit, lock, parameters);
    }
    auto e = begin(hg::Op::create_heap, nullptr, nullptr, reserve, flags, _ReturnAddress());
    PVOID result = nullptr;
    SetLastError(error);
    __try {
        result = real_create(flags, base, reserve, commit, lock, parameters);
    } __finally {
        finish(e, reinterpret_cast<uint64_t>(result), AbnormalTermination() != FALSE);
    }
    return result;
}
PVOID NTAPI hook_destroy(PVOID heap) {
    const DWORD error = GetLastError();
    if (!collecting()) {
        SetLastError(error);
        return real_destroy(heap);
    }
    auto e = begin(hg::Op::destroy_heap, heap, nullptr, 0, 0, _ReturnAddress());
    PVOID result = heap;
    SetLastError(error);
    __try {
        result = real_destroy(heap);
    } __finally {
        finish(e, reinterpret_cast<uint64_t>(result), AbnormalTermination() != FALSE);
    }
    return result;
}
LONG install() {
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    real_backtrace = reinterpret_cast<Backtrace>(GetProcAddress(nt, "RtlCaptureStackBackTrace"));
    if (real_backtrace) {
        PVOID warm[hg::max_stack_frames]{};
        real_backtrace(0, hg::max_stack_frames, warm, nullptr);
    }
    real_alloc = reinterpret_cast<Alloc>(GetProcAddress(nt, "RtlAllocateHeap"));
    real_realloc = reinterpret_cast<Realloc>(GetProcAddress(nt, "RtlReAllocateHeap"));
    real_free = reinterpret_cast<Free>(GetProcAddress(nt, "RtlFreeHeap"));
    real_create = reinterpret_cast<Create>(GetProcAddress(nt, "RtlCreateHeap"));
    real_destroy = reinterpret_cast<Destroy>(GetProcAddress(nt, "RtlDestroyHeap"));
    if (!real_alloc || !real_realloc || !real_free || !real_create || !real_destroy)
        return ERROR_PROC_NOT_FOUND;
    HANDLE threads[2048]{};
    size_t count = 0;
    LONG error = NO_ERROR;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return GetLastError();
    THREADENTRY32 t{};
    t.dwSize = sizeof(t);
    if (Thread32First(snapshot, &t))
        do {
            if (t.th32OwnerProcessID == GetCurrentProcessId() && t.th32ThreadID != GetCurrentThreadId()) {
                if (count == 2048) {
                    error = ERROR_TOO_MANY_TCBS;
                    break;
                }
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                                          THREAD_QUERY_INFORMATION,
                                      FALSE, t.th32ThreadID);
                if (h)
                    threads[count++] = h;
                else if (GetLastError() != ERROR_INVALID_PARAMETER) {
                    error = GetLastError();
                    break;
                }
            }
        } while (Thread32Next(snapshot, &t));
    CloseHandle(snapshot);
    if (!error)
        error = DetourTransactionBegin();
    if (!error) {
        error = DetourUpdateThread(GetCurrentThread());
        for (size_t i = 0; i < count && !error; ++i)
            error = DetourUpdateThread(threads[i]);
        for (unsigned pass = 0; !error && pass < 8; ++pass) {
            bool added = false;
            HANDLE current = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (current == INVALID_HANDLE_VALUE) {
                error = GetLastError();
                break;
            }
            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            if (Thread32First(current, &entry))
                do {
                    if (entry.th32OwnerProcessID != GetCurrentProcessId() ||
                        entry.th32ThreadID == GetCurrentThreadId())
                        continue;
                    bool known = false;
                    for (size_t i = 0; i < count; ++i)
                        if (GetThreadId(threads[i]) == entry.th32ThreadID) {
                            known = true;
                            break;
                        }
                    if (known)
                        continue;
                    if (count == 2048) {
                        error = ERROR_TOO_MANY_TCBS;
                        break;
                    }
                    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                                              THREAD_QUERY_INFORMATION,
                                          FALSE, entry.th32ThreadID);
                    if (!h) {
                        if (GetLastError() != ERROR_INVALID_PARAMETER)
                            error = GetLastError();
                        continue;
                    }
                    threads[count++] = h;
                    error = DetourUpdateThread(h);
                    added = true;
                    if (error)
                        break;
                } while (Thread32Next(current, &entry));
            CloseHandle(current);
            if (!added)
                break;
            if (pass == 7)
                error = ERROR_RETRY;
        }
        if (!error)
            error = DetourAttach(reinterpret_cast<PVOID *>(&real_alloc), hook_alloc);
        if (!error)
            error = DetourAttach(reinterpret_cast<PVOID *>(&real_realloc), hook_realloc);
        if (!error)
            error = DetourAttach(reinterpret_cast<PVOID *>(&real_free), hook_free);
#if defined(HG_VALIDATE_INSTALL_FAILURE)
        if (!error)
            error = ERROR_CANCELLED;
#endif
        if (!error)
            error = DetourAttach(reinterpret_cast<PVOID *>(&real_create), hook_create);
        if (!error)
            error = DetourAttach(reinterpret_cast<PVOID *>(&real_destroy), hook_destroy);
        if (error)
            DetourTransactionAbort();
        else
            error = DetourTransactionCommit();
    }
    for (size_t i = 0; i < count; ++i)
        CloseHandle(threads[i]);
    return error;
}
} // namespace
extern "C" __declspec(dllexport) DWORD WINAPI HeapglassProtocolVersion(LPVOID) {
    return hg::protocol_version;
}
extern "C" __declspec(dllexport) DWORD WINAPI HeapglassInitialize(LPVOID) {
    if (InterlockedCompareExchange(&initialized, 1, 0))
        return shared && hg::atomic_read(&shared->ready) == 1 ? ERROR_SUCCESS : ERROR_ALREADY_INITIALIZED;
    if (depth_index == TLS_OUT_OF_INDEXES) {
        depth_index = TlsAlloc();
        if (depth_index >= TLS_MINIMUM_AVAILABLE) {
            if (depth_index != TLS_OUT_OF_INDEXES)
                TlsFree(depth_index);
            depth_index = TLS_OUT_OF_INDEXES;
            InterlockedExchange(&initialized, 0);
            return ERROR_NO_SYSTEM_RESOURCES;
        }
    }
    set_depth(true);
    FILETIME created{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exit, &kernel, &user)) {
        set_depth(false);
        InterlockedExchange(&initialized, 0);
        return GetLastError();
    }
    wchar_t name[128]{};
    hg::mapping_name(name, 128, GetCurrentProcessId(), hg::filetime(created));
    mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
    if (!mapping) {
        set_depth(false);
        InterlockedExchange(&initialized, 0);
        return GetLastError();
    }
    shared = static_cast<hg::Shared *>(
        MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(hg::Shared)));
    if (!shared || !hg::valid(shared, GetCurrentProcessId(), hg::filetime(created))) {
        if (shared)
            UnmapViewOfFile(shared);
        shared = nullptr;
        CloseHandle(mapping);
        mapping = nullptr;
        set_depth(false);
        InterlockedExchange(&initialized, 0);
        return ERROR_INVALID_DATA;
    }
    LONG error = install();
    InterlockedExchange(&shared->error, error);
    InterlockedExchange(&shared->ready, error ? -1 : 1);
    if (!error && real_backtrace) {
        auto before = hg::atomic_read(&shared->stack_violations);
        bool good = true;
        TlsSetValue(depth_index, reinterpret_cast<void *>(2));
        for (unsigned i = 0; i < 32; ++i) {
            PVOID frames[hg::max_stack_frames]{};
            if (!real_backtrace(0, hg::max_stack_frames, frames, nullptr))
                good = false;
        }
        set_depth(true);
        InterlockedExchange(&shared->stack_ready,
                            good && before == hg::atomic_read(&shared->stack_violations) ? 1 : -1);
    }
    if (error) {
        UnmapViewOfFile(shared);
        shared = nullptr;
        CloseHandle(mapping);
        mapping = nullptr;
        InterlockedExchange(&initialized, 0);
    }
    set_depth(false);
    return error;
}
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE;
}
