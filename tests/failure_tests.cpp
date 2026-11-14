#include "controller.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <sddl.h>
#include <set>
#include <stdexcept>

#define REQUIRE(x)                                                                                           \
    do {                                                                                                     \
        if (!(x))                                                                                            \
            throw std::runtime_error(std::string(__FUNCTION__) + ":" + std::to_string(__LINE__) + ": " #x);  \
    } while (false)

namespace {
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE h = nullptr) : value(h) {}
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
};
struct LocalMemory {
    void *value = nullptr;
    ~LocalMemory() {
        if (value)
            LocalFree(value);
    }
};
struct Mapping {
    Handle handle;
    hg::Shared *shared = nullptr;
    Mapping(DWORD pid, uint64_t created, bool create) {
        wchar_t name[128]{};
        hg::mapping_name(name, 128, pid, created);
        handle.value = create ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                                   sizeof(hg::Shared), name)
                              : OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
        REQUIRE(handle.value);
        if (create)
            REQUIRE(GetLastError() != ERROR_ALREADY_EXISTS);
        shared = static_cast<hg::Shared *>(
            MapViewOfFile(handle.value, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(hg::Shared)));
        REQUIRE(shared);
    }
    ~Mapping() {
        if (shared)
            UnmapViewOfFile(shared);
    }
};
template <class F> void wait_until(F condition, unsigned timeout, const std::string &message) {
    const auto start = GetTickCount64();
    while (!condition()) {
        if (GetTickCount64() - start >= timeout)
            throw std::runtime_error(message);
        Sleep(20);
    }
}
template <class F>
std::shared_ptr<const hg::Model> await(hg::Controller &controller, F condition, unsigned timeout = 10000) {
    std::shared_ptr<const hg::Model> model;
    wait_until(
        [&] {
            model = controller.view();
            return condition(*model);
        },
        timeout, "Controller timeout");
    return model;
}
struct Child {
    Handle process, stop, ready;
    DWORD pid = 0;
    Child(DWORD capture_pid = 0, PSECURITY_DESCRIPTOR descriptor = nullptr) {
        static unsigned sequence = 0;
        const auto suffix = std::to_wstring(GetCurrentProcessId()) + L"-" +
                            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++sequence);
        const std::wstring stop_name = L"Local\\HeapglassFailureStop-" + suffix;
        const std::wstring ready_name = L"Local\\HeapglassFailureReady-" + suffix;
        stop.value = CreateEventW(nullptr, TRUE, FALSE, stop_name.c_str());
        ready.value = CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str());
        REQUIRE(stop.value && ready.value);
        const auto exe = hg::executable_directory() + L"\\HeapglassFailureHost.exe";
        auto command = L"\"" + exe +
                       (capture_pid ? L"\" --capture-host " + std::to_wstring(capture_pid) + L" \"" +
                                          ready_name + L"\" \"" + stop_name + L"\""
                                    : L"\" --workload \"" + stop_name + L"\" \"" + ready_name + L"\"");
        SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        REQUIRE(CreateProcessW(exe.c_str(), command.data(), descriptor ? &security : nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info));
        process.value = info.hProcess;
        pid = info.dwProcessId;
        CloseHandle(info.hThread);
        // A throwing constructor still must stop the child it just created.
        HANDLE objects[]{ready.value, process.value};
        if (WaitForMultipleObjects(2, objects, FALSE, 12000) != WAIT_OBJECT_0) {
            SetEvent(stop.value);
            if (WaitForSingleObject(process.value, 5000) != WAIT_OBJECT_0) {
                TerminateProcess(process.value, 90);
                WaitForSingleObject(process.value, 5000);
            }
            throw std::runtime_error("Failure helper exited or did not become ready");
        }
    }
    ~Child() {
        SetEvent(stop.value);
        if (WaitForSingleObject(process.value, 5000) != WAIT_OBJECT_0) {
            // Only the exact child handle returned by our CreateProcessW is terminated.
            TerminateProcess(process.value, 91);
            WaitForSingleObject(process.value, 5000);
        }
    }
    void alive() const {
        REQUIRE(WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT);
    }
    void finish() {
        REQUIRE(SetEvent(stop.value));
        REQUIRE(WaitForSingleObject(process.value, 5000) == WAIT_OBJECT_0);
        DWORD result = 0;
        REQUIRE(GetExitCodeProcess(process.value, &result));
        REQUIRE(result == 0);
    }
    uint64_t creation() const {
        FILETIME created{}, exited{}, kernel{}, user{};
        REQUIRE(GetProcessTimes(process.value, &created, &exited, &kernel, &user));
        return hg::filetime(created);
    }
};
struct NoDebugPrivilege {
    Handle token;
    TOKEN_PRIVILEGES previous{};
    NoDebugPrivilege() {
        REQUIRE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &token.value));
        TOKEN_PRIVILEGES disabled{};
        disabled.PrivilegeCount = 1;
        REQUIRE(LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &disabled.Privileges[0].Luid));
        DWORD bytes = sizeof(previous);
        REQUIRE(AdjustTokenPrivileges(token.value, FALSE, &disabled, sizeof(previous), &previous, &bytes));
    }
    ~NoDebugPrivilege() {
        AdjustTokenPrivileges(token.value, FALSE, &previous, 0, nullptr, nullptr);
    }
};
void access_denied() {
    NoDebugPrivilege privilege;
    Handle token;
    REQUIRE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value));
    DWORD bytes = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &bytes);
    std::vector<uint8_t> user(bytes);
    REQUIRE(GetTokenInformation(token.value, TokenUser, user.data(), bytes, &bytes));
    LocalMemory sid;
    REQUIRE(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid,
                                   reinterpret_cast<LPWSTR *>(&sid.value)));
    // SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, with no VM or injection rights.
    const auto sddl = L"D:P(A;;0x00101000;;;" + std::wstring(static_cast<wchar_t *>(sid.value)) + L")";
    LocalMemory descriptor;
    REQUIRE(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                                 &descriptor.value, nullptr));
    Child target(0, descriptor.value);
    Handle limited(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, target.pid));
    REQUIRE(limited.value);
    Handle denied(OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_CREATE_THREAD, FALSE, target.pid));
    REQUIRE(!denied.value);
    REQUIRE(GetLastError() == ERROR_ACCESS_DENIED);
    hg::Controller controller;
    controller.attach(target.pid);
    const auto model = await(
        controller, [](const hg::Model &m) { return m.status.find("Open process:") != std::string::npos; });
    REQUIRE(!model->connected);
    REQUIRE(model->status.find("(5)") != std::string::npos);
    target.alive();
    target.finish();
}
void identity_mismatch() {
    Child target;
    Mapping mapping(target.pid, target.creation(), true);
    hg::initialize(mapping.shared, target.pid, target.creation() + 1, 123);
    hg::Controller controller;
    controller.attach(target.pid);
    const auto model = await(
        controller, [](const hg::Model &m) { return m.status == "IPC protocol/process identity mismatch"; });
    REQUIRE(!model->connected);
    REQUIRE(hg::atomic_read(&mapping.shared->ready) == 0);
    target.alive();
    // This exercises a stale creation identity explicitly; it does not claim actual PID reuse.
    target.finish();
}
void install_rollback() {
    Child target;
    Mapping mapping(target.pid, target.creation(), true);
    hg::initialize(mapping.shared, target.pid, target.creation(), 456);
    {
        hg::Controller failed(L"HeapglassAgentFault.dll");
        failed.attach(target.pid);
        const auto model = await(failed, [](const hg::Model &m) {
            return m.status.find("Hook transaction failed:") != std::string::npos;
        });
        REQUIRE(!model->connected);
        REQUIRE(model->status.find("(1223)") != std::string::npos);
        REQUIRE(hg::atomic_read(&mapping.shared->error) == ERROR_CANCELLED);
        REQUIRE(hg::atomic_read(&mapping.shared->ready) == -1);
        REQUIRE(hg::atomic_read(&mapping.shared->recording) == 0);
        target.alive();
    }
    hg::Controller controller;
    controller.attach(target.pid);
    const auto model = await(controller, [](const hg::Model &m) { return m.connected && m.observed >= 100; });
    REQUIRE(model->recording);
    REQUIRE(hg::atomic_read(&mapping.shared->ready) == 1);
    REQUIRE(hg::atomic_read(&mapping.shared->error) == ERROR_SUCCESS);
    target.alive();
    controller.disconnect();
    await(controller, [](const hg::Model &m) { return !m.connected; });
    target.finish();
}
void bounded_read() {
    Child target;
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const size_t page = system.dwPageSize;
    auto *base = static_cast<uint8_t *>(
        VirtualAllocEx(target.process.value, nullptr, 4 * page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    REQUIRE(base);
    struct RemoteMemory {
        HANDLE process;
        void *address;
        ~RemoteMemory() {
            VirtualFreeEx(process, address, 0, MEM_RELEASE);
        }
    } memory{target.process.value, base};
    std::vector<uint8_t> bytes(4 * page, 'A');
    std::fill(bytes.begin() + page, bytes.begin() + 2 * page, uint8_t('B'));
    SIZE_T written = 0;
    REQUIRE(WriteProcessMemory(target.process.value, base, bytes.data(), bytes.size(), &written));
    REQUIRE(written == bytes.size());
    DWORD previous = 0;
    REQUIRE(VirtualProtectEx(target.process.value, base + page, page, PAGE_READONLY, &previous));
    REQUIRE(VirtualProtectEx(target.process.value, base + 2 * page, page, PAGE_NOACCESS, &previous));
    REQUIRE(VirtualProtectEx(target.process.value, base + 3 * page, page, PAGE_READWRITE | PAGE_GUARD,
                             &previous));
    const auto address = reinterpret_cast<uint64_t>(base);
    auto complete = hg::read_memory(target.process.value, address + page - 32, 64);
    auto expected = std::vector<uint8_t>(32, 'A');
    expected.insert(expected.end(), 32, 'B');
    REQUIRE(complete.bytes() == expected);
    REQUIRE(!complete.truncated);
    auto partial = hg::read_memory(target.process.value, address + 2 * page - 32, 64);
    REQUIRE(partial.bytes() == std::vector<uint8_t>(32, 'B'));
    REQUIRE(partial.truncated);
    REQUIRE(!partial.note.empty());
    auto inaccessible = hg::read_memory(target.process.value, address + 2 * page, 32);
    REQUIRE(inaccessible.bytes().empty());
    REQUIRE(inaccessible.truncated);
    auto guarded = hg::read_memory(target.process.value, address + 3 * page, 32);
    REQUIRE(guarded.bytes().empty());
    REQUIRE(guarded.truncated);
    MEMORY_BASIC_INFORMATION region{};
    REQUIRE(VirtualQueryEx(target.process.value, base + 3 * page, &region, sizeof(region)) == sizeof(region));
    REQUIRE((region.Protect & PAGE_GUARD) != 0);
    for (const auto &[start, count] : std::array<std::pair<uint64_t, size_t>, 2>{
             {{address, 1024 * 1024 + 1}, {std::numeric_limits<uint64_t>::max() - 15, 32}}}) {
        bool rejected = false;
        try {
            hg::read_memory(target.process.value, start, count);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        REQUIRE(rejected);
    }
    target.finish();
    auto exited = hg::read_memory(target.process.value, address, 32);
    REQUIRE(exited.bytes().empty());
    REQUIRE(exited.truncated);
    REQUIRE(!exited.note.empty());
}
void reconnect() {
    Child target;
    hg::Controller controller;
    std::set<uint64_t> sessions;
    for (unsigned iteration = 0; iteration < 6; ++iteration) {
        controller.attach(target.pid);
        const auto model = await(
            controller, [](const hg::Model &m) { return m.connected && m.recording && m.observed >= 100; });
        REQUIRE(model->pid == target.pid);
        REQUIRE(model->session != 0);
        REQUIRE(sessions.insert(model->session).second);
        controller.disconnect();
        await(controller, [](const hg::Model &m) { return !m.connected; });
        target.alive();
    }
    REQUIRE(sessions.size() == 6);
    target.finish();
}
struct WorkloadProgress {
    Handle mapping;
    volatile LONG64 *batches = nullptr;
    explicit WorkloadProgress(DWORD pid) {
        const auto name = L"Local\\HeapglassFailureProgress-" + std::to_wstring(pid);
        mapping.value = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name.c_str());
        REQUIRE(mapping.value);
        batches = static_cast<volatile LONG64 *>(
            MapViewOfFile(mapping.value, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(LONG64)));
        REQUIRE(batches);
    }
    ~WorkloadProgress() {
        if (batches)
            UnmapViewOfFile(const_cast<LONG64 *>(batches));
    }
    LONG64 completed() const {
        return InterlockedCompareExchange64(batches, 0, 0);
    }
};
void legacy_agent() {
    Child target;
    WorkloadProgress progress(target.pid);
    {
        hg::Controller legacy(L"HeapglassAgentLegacy.dll");
        legacy.attach(target.pid);
        const auto model = await(legacy, [](const hg::Model &m) {
            return m.status == "Resident agent version is incompatible; restart the target before "
                               "upgrading instrumentation";
        });
        REQUIRE(!model->connected);
        REQUIRE(!model->recording);
        target.alive();
        const auto batches_before = progress.completed();
        wait_until([&] { return progress.completed() > batches_before; }, 2000,
                   "Workload stopped making progress after legacy agent rejection");
    }
    hg::Controller controller;
    controller.attach(target.pid);
    const auto model =
        await(controller, [](const hg::Model &m) { return m.connected && m.recording && m.observed >= 100; });
    REQUIRE(model->pid == target.pid);
    target.alive();
    controller.disconnect();
    await(controller, [](const hg::Model &m) { return !m.connected; });
    target.finish();
}
void controller_crash() {
    Child target;
    WorkloadProgress progress(target.pid);
    const auto initial_batches = progress.completed();
    wait_until([&] { return progress.completed() > initial_batches; }, 2000,
               "Workload did not complete allocation batches before attach");
    Child host(target.pid);
    Mapping mapping(target.pid, target.creation(), false);
    REQUIRE(hg::atomic_read(&mapping.shared->recording) == 1);
    const auto old_session = mapping.shared->session;
    const auto old_epoch = hg::atomic_read(&mapping.shared->epoch);
    // Deliberately crash only our capture helper, never the instrumented workload or another PID.
    REQUIRE(TerminateProcess(host.process.value, 77));
    REQUIRE(WaitForSingleObject(host.process.value, 5000) == WAIT_OBJECT_0);
    wait_until([&] { return hg::atomic_read(&mapping.shared->recording) == 0; }, 7000,
               "Agent did not stop recording after controller heartbeat loss");
    REQUIRE(hg::atomic_read(&mapping.shared->epoch) > old_epoch);
    target.alive();
    const auto batches_before = progress.completed();
    wait_until([&] { return progress.completed() > batches_before; }, 2000,
               "Workload stopped making progress after controller crash");
    std::cout << "allocation batches after heartbeat expiry: " << batches_before << " -> "
              << progress.completed() << '\n';
    hg::Controller controller;
    controller.attach(target.pid);
    const auto model =
        await(controller, [](const hg::Model &m) { return m.connected && m.recording && m.observed >= 100; });
    REQUIRE(model->session != old_session);
    REQUIRE(model->incomplete);
    target.alive();
    controller.disconnect();
    await(controller, [](const hg::Model &m) { return !m.connected; });
    target.finish();
}
} // namespace
int main(int argc, char **argv) {
    const std::array<std::pair<const char *, void (*)()>, 7> cases{{{"access-denied", access_denied},
                                                                    {"identity-mismatch", identity_mismatch},
                                                                    {"install-rollback", install_rollback},
                                                                    {"bounded-read", bounded_read},
                                                                    {"reconnect", reconnect},
                                                                    {"controller-crash", controller_crash},
                                                                    {"legacy-agent", legacy_agent}}};
    try {
        std::string selected = "all";
        if (argc == 3 && std::string(argv[1]) == "--case")
            selected = argv[2];
        else if (argc != 1)
            throw std::runtime_error("Usage: HeapglassFailureTests [--case NAME|all]");
        bool found = false;
        for (const auto &[name, run] : cases)
            if (selected == "all" || selected == name) {
                found = true;
                std::cout << "RUN " << name << std::endl;
                run();
                std::cout << "PASS " << name << std::endl;
            }
        if (!found)
            throw std::runtime_error("Unknown failure case: " + selected);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
