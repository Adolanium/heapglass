#include "controller.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
};
void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(std::string(message) + ": " + hg::error_text(GetLastError()));
}
struct WorkloadProgress {
    Handle mapping{nullptr};
    volatile LONG64 *batches = nullptr;
    WorkloadProgress() {
        const auto name = L"Local\\HeapglassFailureProgress-" + std::to_wstring(GetCurrentProcessId());
        mapping.value = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(LONG64),
                                           name.c_str());
        check(mapping.value != nullptr, "Create workload progress mapping");
        check(GetLastError() != ERROR_ALREADY_EXISTS, "Workload progress mapping already exists");
        batches = static_cast<volatile LONG64 *>(
            MapViewOfFile(mapping.value, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(LONG64)));
        check(batches != nullptr, "Map workload progress");
    }
    ~WorkloadProgress() {
        if (batches)
            UnmapViewOfFile(const_cast<LONG64 *>(batches));
    }
};
int workload(const wchar_t *stop_name, const wchar_t *ready_name) {
    Handle stop(OpenEventW(SYNCHRONIZE, FALSE, stop_name));
    Handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, ready_name));
    check(stop.value && ready.value, "Open workload events");
    // Fixture ground truth stays separate from the agent's transport and controller model.
    WorkloadProgress progress;
    std::array<std::jthread, 4> threads;
    for (unsigned index = 0; index < threads.size(); ++index)
        threads[index] = std::jthread([index, &progress](std::stop_token token) {
            HANDLE heap = HeapCreate(0, 0, 0);
            if (!heap)
                return;
            unsigned iteration = 0;
            while (!token.stop_requested()) {
                std::array<void *, 32> allocations{};
                for (size_t n = 0; n < allocations.size(); ++n) {
                    const size_t size = 64 + ((iteration + n + index * 17) % 1024);
                    allocations[n] = HeapAlloc(heap, HEAP_ZERO_MEMORY, size);
                    if (allocations[n])
                        static_cast<volatile unsigned char *>(allocations[n])[0] =
                            static_cast<unsigned char>(iteration);
                }
                for (void *allocation : allocations)
                    if (allocation)
                        HeapFree(heap, 0, allocation);
                InterlockedIncrement64(progress.batches);
                ++iteration;
                Sleep(1);
            }
            HeapDestroy(heap);
        });
    check(SetEvent(ready.value) != FALSE, "Signal workload readiness");
    const DWORD result = WaitForSingleObject(stop.value, 18000);
    for (auto &thread : threads)
        thread.request_stop();
    check(result == WAIT_OBJECT_0 || result == WAIT_TIMEOUT, "Wait for workload stop");
    return 0;
}
int capture_host(DWORD pid, const wchar_t *ready_name, const wchar_t *stop_name) {
    Handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, ready_name));
    Handle stop(OpenEventW(SYNCHRONIZE, FALSE, stop_name));
    check(ready.value && stop.value, "Open capture host events");
    hg::Controller controller;
    controller.attach(pid);
    const auto started = GetTickCount64();
    for (;;) {
        const auto model = controller.view();
        if (model->connected && model->recording && model->observed >= 32)
            break;
        if (GetTickCount64() - started >= 10000)
            throw std::runtime_error("Capture host attach failed: " + model->status);
        Sleep(20);
    }
    check(SetEvent(ready.value) != FALSE, "Signal capture host readiness");
    const DWORD result = WaitForSingleObject(stop.value, 25000);
    check(result == WAIT_OBJECT_0 || result == WAIT_TIMEOUT, "Wait for capture host stop");
    return 0;
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    try {
        if (argc == 4 && std::wstring(argv[1]) == L"--workload")
            return workload(argv[2], argv[3]);
        if (argc == 5 && std::wstring(argv[1]) == L"--capture-host") {
            wchar_t *end = nullptr;
            const unsigned long pid = std::wcstoul(argv[2], &end, 10);
            if (!pid || !end || *end)
                throw std::runtime_error("Invalid capture target PID");
            return capture_host(static_cast<DWORD>(pid), argv[3], argv[4]);
        }
        throw std::runtime_error("Usage: HeapglassFailureHost --workload STOP_EVENT READY_EVENT, or "
                                 "--capture-host PID READY_EVENT STOP_EVENT");
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
