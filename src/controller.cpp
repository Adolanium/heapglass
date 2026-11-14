#include "controller.hpp"
#include "profiling.hpp"
#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <dbghelp.h>
#include <filesystem>
#include <psapi.h>
#include <sddl.h>
#include <stdexcept>
#include <tlhelp32.h>

namespace hg {
std::string narrow(const std::wstring &s) {
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0, nullptr,
                                nullptr);
    if (!n)
        return "<invalid Unicode>";
    std::string r(n, 0);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), int(s.size()), r.data(), n, nullptr,
                        nullptr);
    return r;
}
std::wstring widen(const std::string &s) {
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0);
    if (!n)
        throw std::runtime_error("Invalid UTF-8 path");
    std::wstring r(n, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), r.data(), n);
    return r;
}
std::string error_text(DWORD code) {
    wchar_t *b = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<wchar_t *>(&b), 0, nullptr);
    std::string r = b ? narrow(b) : "Windows error";
    if (b)
        LocalFree(b);
    return r + " (" + std::to_string(code) + ")";
}
static void check(bool ok, const char *action) {
    if (!ok) {
        DWORD code = GetLastError();
        throw std::runtime_error(std::string(action) + ": " + error_text(code));
    }
}
std::wstring executable_directory() {
    wchar_t b[32768]{};
    GetModuleFileNameW(nullptr, b, 32768);
    return std::filesystem::path(b).parent_path().wstring();
}
static std::string architecture(HANDLE h) {
    USHORT p = 0, n = 0;
    if (!IsWow64Process2(h, &p, &n))
        return "unknown";
    if (n != IMAGE_FILE_MACHINE_AMD64)
        return "unsupported host";
    return p == IMAGE_FILE_MACHINE_UNKNOWN ? "x64"
           : p == IMAGE_FILE_MACHINE_I386  ? "x86 (unsupported)"
                                           : "ARM (unsupported)";
}
std::vector<Process> processes() {
    std::vector<Process> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return out;
    PROCESSENTRY32W p{};
    p.dwSize = sizeof(p);
    if (Process32FirstW(snap, &p))
        do {
            if (!p.th32ProcessID)
                continue;
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p.th32ProcessID);
            out.push_back({p.th32ProcessID, narrow(p.szExeFile), h ? architecture(h) : "access denied"});
            if (h)
                CloseHandle(h);
        } while (Process32NextW(snap, &p));
    CloseHandle(snap);
    std::sort(out.begin(), out.end(), [](const Process &a, const Process &b) { return a.name < b.name; });
    return out;
}
std::string protection_name(uint32_t p) {
    std::string s;
    switch (p & 255) {
    case PAGE_NOACCESS:
        s = "---";
        break;
    case PAGE_READONLY:
        s = "R--";
        break;
    case PAGE_READWRITE:
        s = "RW-";
        break;
    case PAGE_WRITECOPY:
        s = "RC-";
        break;
    case PAGE_EXECUTE:
        s = "--X";
        break;
    case PAGE_EXECUTE_READ:
        s = "R-X";
        break;
    case PAGE_EXECUTE_READWRITE:
        s = "RWX";
        break;
    case PAGE_EXECUTE_WRITECOPY:
        s = "RCX";
        break;
    default:
        s = "?";
    }
    if (p & PAGE_GUARD)
        s += " guard";
    return s;
}
Controller::Controller(std::wstring filename) : agent_filename(std::move(filename)) {
    if (std::filesystem::path(agent_filename).filename() != agent_filename ||
        std::filesystem::path(agent_filename).extension() != L".dll")
        throw std::invalid_argument("Agent filename must be a DLL beside the executable");
    published.store(std::make_shared<const Model>(model));
    worker = std::jthread([this](std::stop_token t) { loop(t); });
}
Controller::~Controller() {
    worker.request_stop();
    if (worker.joinable())
        worker.join();
    close();
}
void Controller::enqueue(std::function<void()> f) {
    std::lock_guard l(jobs_mutex);
    if (jobs.size() >= 128)
        return;
    jobs.push_back(std::move(f));
}
void Controller::attach(DWORD pid) {
    enqueue([this, pid] { connect(pid); });
}
void Controller::launch(std::wstring path, std::wstring args, std::wstring cwd) {
    enqueue([this, path = std::move(path), args = std::move(args), cwd = std::move(cwd)] {
        close();
        std::wstring command = L"\"" + path + L"\" " + args;
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        check(CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE,
                             CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr,
                             cwd.empty() ? nullptr : cwd.c_str(), &si, &pi) != FALSE,
              "Launch");
        // Always resume a launched process, including on instrumentation failure.
        ResumeThread(pi.hThread);
        try {
            connect(pi.dwProcessId);
        } catch (...) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            throw;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    });
}
void Controller::disconnect() {
    enqueue([this] {
        close();
        model.status = "Disconnected; inert agent remains resident until target exit";
    });
}
void Controller::record(bool enabled) {
    enqueue([this, enabled] {
        if (shared) {
            InterlockedExchange(&shared->recording, 0);
            drain();
            model.boundary();
            model.unsafe_through = std::max(model.unsafe_through, uint64_t(atomic_read(&shared->clock)));
            if (enabled)
                InterlockedExchange(&shared->recording, 1);
            model.recording = enabled;
            if (!enabled)
                model.snapshot_interval_ms = 0;
            model.status = enabled ? "Recording resumed; earlier coverage gap remains"
                                   : "Recording stopped; target continues; agent resident";
        }
    });
}
void Controller::take_snapshot(std::string name, SnapshotMode mode) {
    enqueue([this, name = std::move(name), mode] {
        if (!shared || !model.recording || WaitForSingleObject(process_handle, 0) != WAIT_TIMEOUT)
            throw std::runtime_error("Snapshots require an active recording");
        capture_snapshot(name, mode);
    });
}
void Controller::capture_snapshot(std::string name, SnapshotMode mode) {
    auto b = ticks();
    if (mode == SnapshotMode::with_content)
        sample();
    bool cutoff_known = false;
    uint64_t cutoff = ticks();
    const auto deadline = GetTickCount64() + 100;
    do {
        drain();
        const auto clock = atomic_read(&shared->clock);
        const bool quiet = transport_quiet(shared, model.dropped, local_dropped.load()) &&
                           pending_read.load() == pending_write.load();
        cutoff = ticks();
        cutoff_known = model.recording && atomic_read(&shared->recording) && quiet &&
                       clock == atomic_read(&shared->clock) &&
                       transport_quiet(shared, model.dropped, local_dropped.load()) &&
                       pending_read.load() == pending_write.load();
        if (cutoff_known || GetTickCount64() >= deadline)
            break;
        Sleep(1);
    } while (true);
    auto s = model.snapshot(std::move(name), mode == SnapshotMode::metadata_only ? cutoff : b, cutoff, mode);
    s.incomplete = s.incomplete || !cutoff_known;
    size_t bytes = 0;
    for (auto &a : s.records) {
        while (a.captures.size() > 1)
            a.captures.erase(a.captures.begin());
        for (auto &c : a.captures) {
            if (bytes + c.bytes().size() > 8 * 1024 * 1024) {
                auto content = std::make_shared<CapturedContent>();
                content->strings = c.strings();
                c.content = std::move(content);
                c.attributed = false;
                c.note = "Snapshot byte retention limit";
            }
            bytes += c.bytes().size();
        }
    }
    model.add_snapshot(std::move(s));
    growth_cache.reset();
    dirty = true;
}
void Controller::pin_snapshot(uint64_t id, bool pinned) {
    enqueue([this, id, pinned] {
        model.pin_snapshot(id, pinned);
        growth_cache.reset();
    });
}
void Controller::schedule_snapshots(unsigned interval_ms) {
    enqueue([this, interval_ms] {
        if (interval_ms && (interval_ms < 1000 || interval_ms > 600000))
            throw std::invalid_argument("Timed observation interval must be 1..600 seconds");
        if (interval_ms && (!shared || !model.recording))
            throw std::runtime_error("Timed snapshots require an active recording");
        if (interval_ms && std::none_of(model.snapshots.begin(), model.snapshots.end(),
                                        [](const auto &s) { return s.pinned; })) {
            capture_snapshot("Timed baseline", SnapshotMode::metadata_only);
            model.pin_snapshot(model.snapshots.back().id, true);
        }
        if (interval_ms && (!model.recording || !atomic_read(&shared->recording)))
            throw std::runtime_error("Recording stopped while acquiring the timed baseline");
        model.snapshot_interval_ms = interval_ms;
        next_timed_snapshot = GetTickCount64() + interval_ms;
        timed_sequence = 0;
    });
}
void Controller::watch(uint64_t g, bool enabled) {
    enqueue([this, g, enabled] {
        if (enabled)
            model.watched.insert(g);
        else
            model.watched.erase(g);
        force_sample = true;
    });
}
void Controller::configure(Limits l) {
    enqueue([this, l] {
        auto validated = validated_limits(l);
        if (shared && validated.stack_frames && atomic_read(&shared->stack_ready) != 1)
            throw std::runtime_error("Bounded stack capture did not pass the agent allocation-free probe");
        desired_limits = validated;
        model.limits = validated;
        model.trim_captures();
        if (shared)
            InterlockedExchange(&shared->stack_frames, validated.stack_frames);
    });
}
void Controller::sample_now() {
    enqueue([this] { force_sample = true; });
}
void Controller::save(std::wstring path, bool bytes) {
    enqueue([this, path = std::move(path), bytes] {
        save_session(model, path, bytes);
        model.status = "Session saved";
    });
}
void Controller::load(std::wstring path) {
    enqueue([this, path = std::move(path)] {
        auto loaded = load_session(path);
        close();
        model = std::move(loaded);
        growth_cache.reset();
        model.status = "Offline session; no process reads";
    });
}
void Controller::csv(std::wstring path) {
    enqueue([this, path = std::move(path)] {
        export_csv(model, path);
        model.status = "CSV exported";
    });
}
void Controller::sites_csv(std::wstring path) {
    enqueue([this, path = std::move(path)] {
        export_sites_csv(summarize_allocations(model, ticks()), path, model.frame_symbols);
        model.status = "Allocation sites exported";
    });
}
void Controller::growth_csv(std::wstring path) {
    enqueue([this, path = std::move(path)] {
        export_growth_csv(analyze_growth(model.snapshots), path, model.frame_symbols);
        model.status = "Snapshot growth exported";
    });
}
void Controller::stall_ingestion(unsigned ms) {
    stall_until.store(GetTickCount64() + std::min(ms, 2000u));
}
PipelineMetrics Controller::metrics() const {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return {uint64_t(f.QuadPart), ingest_ticks.load(), apply_ticks.load(),   publish_ticks.load(),
            sample_ticks.load(),  symbol_ticks.load(), local_dropped.load(), max_pending.load()};
}
void Controller::collect(std::stop_token stop, HANDLE idle_timer) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    uint64_t heartbeat = 0;
    for (;;) {
        auto now = GetTickCount64();
        if (now - heartbeat >= 100) {
            WriteRelease64(&shared->heartbeat, now);
            heartbeat = now;
        }
        if (!stop.stop_requested() && now < stall_until.load()) {
            Sleep(1);
            continue;
        }
        auto begin = ticks();
        size_t count = 0;
        while (count < 65536 && transfer(
                                    shared,
                                    [&](const Event &event) {
                                        auto write = pending_write.load(std::memory_order_relaxed);
                                        auto read = pending_read.load(std::memory_order_acquire);
                                        if (write - read >= pending_capacity)
                                            ++local_dropped;
                                        else {
                                            pending_events[write % pending_capacity] = event;
                                            pending_write.store(write + 1, std::memory_order_release);
                                            auto size = write + 1 - read;
                                            if (size > max_pending.load(std::memory_order_relaxed))
                                                max_pending.store(size, std::memory_order_relaxed);
                                        }
                                    })) {
            ++count;
        }
        ingest_ticks.fetch_add(ticks() - begin, std::memory_order_relaxed);
        if (stop.stop_requested() && !count)
            break;
        if (!count)
            WaitForSingleObject(idle_timer, INFINITE);
    }
}
void Controller::stop_collector() {
    if (collector.joinable()) {
        collector.request_stop();
        collector.join();
    }
}
void Controller::publish() {
    if (shared) {
        model.active_stack_frames = atomic_read(&shared->stack_frames);
        auto state = atomic_read(&shared->stack_ready);
        model.stack_state = state == 1 ? 1 : state < 0 ? 2 : 0;
        model.stack_violations = atomic_read(&shared->stack_violations);
    }
    auto begin = ticks();
    model.controller_dropped = local_dropped.load();
    model.analysis_pending = pending_write.load() - pending_read.load();
    model.transport_pending = shared ? uint64_t(atomic_read(&shared->write) - atomic_read(&shared->read)) : 0;
    auto presentation = model.presentation();
    presentation.allocation_profile =
        std::make_shared<const AllocationProfile>(summarize_allocations(model, ticks()));
    if (!growth_cache)
        growth_cache = std::make_shared<const GrowthReport>(analyze_growth(model.snapshots));
    presentation.growth_report = growth_cache;
    published.store(std::make_shared<const Model>(std::move(presentation)));
    publish_ticks.fetch_add(ticks() - begin, std::memory_order_relaxed);
    last_publish = GetTickCount64();
    dirty = false;
}
void Controller::loop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::deque<std::function<void()>> pending;
        {
            std::lock_guard l(jobs_mutex);
            pending.swap(jobs);
        }
        for (auto &f : pending)
            try {
                f();
                publish();
            } catch (const std::exception &e) {
                model.status = e.what();
                publish();
            }
        try {
            if (shared) {
                if (WaitForSingleObject(process_handle, 0) == WAIT_OBJECT_0) {
                    stop_collector();
                    while (pending_read.load() != pending_write.load())
                        drain();
                    close();
                    model.status = "Target exited; retained observations available";
                    publish();
                } else {
                    drain();
                    auto now = GetTickCount64();
                    if (model.recording && model.snapshot_interval_ms && now >= next_timed_snapshot) {
                        capture_snapshot("Timed observation " + std::to_string(++timed_sequence),
                                         SnapshotMode::metadata_only);
                        next_timed_snapshot = GetTickCount64() + model.snapshot_interval_ms;
                    }
                    if (force_sample || now - last_sample >= model.limits.sample_ms) {
                        auto b = ticks();
                        sample();
                        sample_ticks.fetch_add(ticks() - b, std::memory_order_relaxed);
                        force_sample = false;
                        last_sample = now;
                    }
                    if (now - last_region >= 1000) {
                        memory_map();
                        last_region = now;
                        model.analyze();
                        dirty = true;
                    }
                    auto b = ticks();
                    resolve();
                    symbol_ticks.fetch_add(ticks() - b, std::memory_order_relaxed);
                }
            }
            if (dirty && GetTickCount64() - last_publish >= 250)
                publish();
        } catch (const std::exception &e) {
            model.status = e.what();
            publish();
        }
        Sleep(pending_write.load() != pending_read.load() ? 0 : 1);
    }
}
static uint64_t remote_module(DWORD pid, const std::wstring &filename) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (s == INVALID_HANDLE_VALUE)
        return 0;
    MODULEENTRY32W m{};
    m.dwSize = sizeof(m);
    uint64_t base = 0;
    if (Module32FirstW(s, &m))
        do {
            if (_wcsicmp(m.szModule, filename.c_str()) == 0) {
                base = reinterpret_cast<uint64_t>(m.modBaseAddr);
                break;
            }
        } while (Module32NextW(s, &m));
    CloseHandle(s);
    return base;
}
static uint64_t remote_export(HANDLE process, uint64_t base, const char *wanted) {
    auto read = [&](uint64_t address, void *buffer, size_t count) {
        SIZE_T got = 0;
        return address <= UINT64_MAX - count &&
               ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), buffer, count, &got) &&
               got == count;
    };
    IMAGE_DOS_HEADER dos{};
    if (!read(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 64 ||
        dos.e_lfanew > 1024 * 1024)
        return 0;
    IMAGE_NT_HEADERS64 nt{};
    if (!read(base + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.SizeOfImage > 256 * 1024 * 1024)
        return 0;
    auto size = nt.OptionalHeader.SizeOfImage;
    auto directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    auto range = [&](uint64_t rva, size_t count) {
        return rva < size && count <= size - rva && base <= UINT64_MAX - rva;
    };
    IMAGE_EXPORT_DIRECTORY exports{};
    if (!range(directory.VirtualAddress, sizeof(exports)) ||
        !range(directory.VirtualAddress, directory.Size) ||
        !read(base + directory.VirtualAddress, &exports, sizeof(exports)) || exports.NumberOfNames > 4096 ||
        exports.NumberOfFunctions > 4096)
        return 0;
    std::vector<DWORD> names(exports.NumberOfNames), functions(exports.NumberOfFunctions);
    std::vector<WORD> ordinals(exports.NumberOfNames);
    if (!range(exports.AddressOfNames, names.size() * 4) ||
        !range(exports.AddressOfFunctions, functions.size() * 4) ||
        !range(exports.AddressOfNameOrdinals, ordinals.size() * 2) ||
        !read(base + exports.AddressOfNames, names.data(), names.size() * 4) ||
        !read(base + exports.AddressOfFunctions, functions.data(), functions.size() * 4) ||
        !read(base + exports.AddressOfNameOrdinals, ordinals.data(), ordinals.size() * 2))
        return 0;
    for (size_t i = 0; i < names.size(); ++i) {
        char name[128]{};
        if (!range(names[i], 1))
            return 0;
        auto n = std::min<size_t>(sizeof(name) - 1, size - names[i]);
        if (!read(base + names[i], name, n) || !memchr(name, 0, n))
            continue;
        if (strcmp(name, wanted) == 0) {
            auto ordinal = ordinals[i];
            if (ordinal >= functions.size() || !range(functions[ordinal], 1))
                return 0;
            auto rva = functions[ordinal];
            if (rva >= directory.VirtualAddress && rva - directory.VirtualAddress < directory.Size)
                return 0;
            return base + rva;
        }
    }
    return 0;
}
static DWORD remote_call(HANDLE process, uint64_t function, void *parameter) {
    HANDLE t = CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(function),
                                  parameter, 0, nullptr);
    check(t != nullptr, "CreateRemoteThread (protected processes cannot be instrumented)");
    DWORD wait = WaitForSingleObject(t, 15000);
    DWORD result = 0;
    if (wait != WAIT_OBJECT_0) {
        CloseHandle(t);
        throw std::runtime_error("Agent initialization timed out. Remote thread was left running; no unsafe "
                                 "termination attempted.");
    }
    check(GetExitCodeThread(t, &result) != FALSE, "Read agent result");
    CloseHandle(t);
    return result;
}
void Controller::connect(DWORD pid) {
    if (pid == GetCurrentProcessId())
        throw std::runtime_error("Self instrumentation is unsupported");
    close();
    model = Model{};
    growth_cache.reset();
    model.limits = desired_limits;
    pending_read = 0;
    pending_write = 0;
    local_dropped = 0;
    max_pending = 0;
    ingest_ticks = 0;
    apply_ticks = 0;
    publish_ticks = 0;
    sample_ticks = 0;
    symbol_ticks = 0;
    if (pending_events.empty())
        pending_events.resize(pending_capacity);
    symbol_jobs.clear();
    queued_symbols.clear();
    process_handle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE |
                                     PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD | SYNCHRONIZE,
                                 FALSE, pid);
    try {
        check(process_handle != nullptr, "Open process: access denied or protected process");
        auto arch = architecture(process_handle);
        if (arch != "x64")
            throw std::runtime_error("Unsupported architecture: " + arch +
                                     ". Native x64 on x64 Windows is required.");
        FILETIME c{}, x{}, k{}, u{};
        check(GetProcessTimes(process_handle, &c, &x, &k, &u) != FALSE, "Process creation identity");
        uint64_t created = filetime(c);
        wchar_t path[32768]{};
        DWORD length = 32768;
        check(QueryFullProcessImageNameW(process_handle, 0, path, &length) != FALSE, "Process name");
        wchar_t name[128]{};
        mapping_name(name, 128, pid, created);
        HANDLE token = nullptr;
        check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE, "Token");
        DWORD needed = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
        std::vector<uint8_t> info(needed);
        BOOL got = GetTokenInformation(token, TokenUser, info.data(), needed, &needed);
        CloseHandle(token);
        check(got != FALSE, "Token user");
        LPWSTR sid = nullptr;
        check(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(info.data())->User.Sid, &sid) != FALSE,
              "SID");
        std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + std::wstring(sid) + L")";
        LocalFree(sid);
        PSECURITY_DESCRIPTOR sd = nullptr;
        check(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd,
                                                                   nullptr) != FALSE,
              "IPC ACL");
        SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
        std::wstring lease_name = std::wstring(name) + L"-owner";
        lease = CreateMutexW(&sa, FALSE, lease_name.c_str());
        if (!lease) {
            LocalFree(sd);
            check(false, "IPC ownership mutex");
        }
        const auto wait = WaitForSingleObject(lease, 0);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
            LocalFree(sd);
            throw std::runtime_error("Another controller owns this process session");
        }
        map_handle = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(Shared), name);
        DWORD existed = GetLastError();
        LocalFree(sd);
        check(map_handle != nullptr, "Shared mapping");
        shared = static_cast<Shared *>(
            MapViewOfFile(map_handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Shared)));
        check(shared != nullptr, "Map shared transport");
        if (existed != ERROR_ALREADY_EXISTS) {
            uint64_t nonce = 0;
            check(BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&nonce), sizeof(nonce),
                                  BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0,
                  "Session nonce");
            initialize(shared, pid, created, nonce);
        }
        if (!valid(shared, pid, created))
            throw std::runtime_error("IPC protocol/process identity mismatch");
        InterlockedExchange64(&shared->heartbeat, GetTickCount64());
        InterlockedExchange(&shared->disconnected, 0);
        InterlockedExchange(&shared->recording, 0);
        std::wstring dll = executable_directory() + L"\\" + agent_filename;
        if (!std::filesystem::is_regular_file(dll))
            throw std::runtime_error("HeapglassAgent.dll is missing beside the executable");
        uint64_t agent = remote_module(pid, agent_filename);
        if (!agent) {
            void *remote = VirtualAllocEx(process_handle, nullptr, (dll.size() + 1) * sizeof(wchar_t),
                                          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            check(remote != nullptr, "Allocate DLL path");
            SIZE_T written = 0;
            if (!WriteProcessMemory(process_handle, remote, dll.c_str(), (dll.size() + 1) * sizeof(wchar_t),
                                    &written)) {
                DWORD code = GetLastError();
                VirtualFreeEx(process_handle, remote, 0, MEM_RELEASE);
                SetLastError(code);
                check(false, "Write DLL path");
            }
            HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
            auto loader = GetProcAddress(kernel, "LoadLibraryW");
            HMODULE owner = nullptr;
            check(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     reinterpret_cast<LPCWSTR>(loader), &owner) != FALSE,
                  "Loader module");
            wchar_t owner_path[32768]{};
            GetModuleFileNameW(owner, owner_path, 32768);
            auto base = remote_module(pid, std::filesystem::path(owner_path).filename().wstring());
            for (unsigned retry = 0; !base && retry < 200; ++retry) {
                Sleep(10);
                base = remote_module(pid, std::filesystem::path(owner_path).filename().wstring());
            }
            if (!base)
                throw std::runtime_error("Could not locate target loader module");
            // On timeout retain the path; a live remote thread may still read it.
            remote_call(process_handle,
                        base + (reinterpret_cast<uint64_t>(loader) - reinterpret_cast<uint64_t>(owner)),
                        remote);
            VirtualFreeEx(process_handle, remote, 0, MEM_RELEASE);
            agent = remote_module(pid, agent_filename);
            if (!agent)
                throw std::runtime_error("Agent DLL could not load. Check signature policy, architecture and "
                                         "process restrictions.");
        }
        auto version_entry = remote_export(process_handle, agent, "HeapglassProtocolVersion");
        if (!version_entry || remote_call(process_handle, version_entry, nullptr) != protocol_version)
            throw std::runtime_error("Resident agent version is incompatible; restart the target before "
                                     "upgrading instrumentation");
        auto entry = remote_export(process_handle, agent, "HeapglassInitialize");
        if (!entry)
            throw std::runtime_error("Agent initialization export missing");
        DWORD result = remote_call(process_handle, entry, nullptr);
        if (result || atomic_read(&shared->ready) != 1)
            throw std::runtime_error("Hook transaction failed: " +
                                     error_text(result ? result : atomic_read(&shared->error)));
        if (existed == ERROR_ALREADY_EXISTS) {
            uint64_t nonce = 0;
            check(BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&nonce), sizeof(nonce),
                                  BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0,
                  "New observation identity");
            shared->session = nonce;
        }
        model.pid = pid;
        model.creation = created;
        model.session = shared->session;
        model.frequency = shared->frequency;
        model.started = ticks();
        model.process = narrow(path);
        SYSTEMTIME now{};
        GetLocalTime(&now);
        char date[64]{};
        sprintf_s(date, "%04u-%02u-%02u %02u:%02u:%02u", now.wYear, now.wMonth, now.wDay, now.wHour,
                  now.wMinute, now.wSecond);
        model.start_time = date;
        model.connected = true;
        model.recording = true;
        model.status = "Recording allocations observed since instrumentation began";
        if (existed == ERROR_ALREADY_EXISTS) {
            model.boundary();
            Event discard{};
            while (pop(shared, discard)) {
            };
            model.dropped = atomic_read(&shared->dropped);
            model.status = "Reconnected; prior recording gap makes state incomplete";
        }
        if (model.limits.stack_frames && atomic_read(&shared->stack_ready) != 1)
            throw std::runtime_error("Bounded stack capture did not pass the agent allocation-free probe");
        model.unsafe_through = atomic_read(&shared->clock);
        InterlockedExchange(&shared->stack_frames, model.limits.stack_frames);
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                              TIMER_MODIFY_STATE | SYNCHRONIZE);
        check(timer != nullptr, "Create collector timer");
        std::shared_ptr<void> timer_owner(timer, [](void *handle) { CloseHandle(handle); });
        LARGE_INTEGER due{};
        due.QuadPart = -10000;
        check(SetWaitableTimer(timer, &due, 1, nullptr, nullptr, FALSE) != FALSE, "Set collector timer");
        collector =
            std::jthread([this, timer_owner](std::stop_token token) { collect(token, timer_owner.get()); });
        InterlockedExchange(&shared->recording, 1);
        last_sample = 0;
        last_region = 0;
        cursor = 0;
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS |
                      SYMOPT_NO_PROMPTS);
        // Empty search path avoids network symbol servers; all resolution runs on this worker.
        symbols_ready = SymInitializeW(process_handle, L"", TRUE) != FALSE;
    } catch (...) {
        close();
        throw;
    }
}
void Controller::close() {
    model.snapshot_interval_ms = 0;
    symbol_jobs.clear();
    queued_symbols.clear();
    if (shared) {
        InterlockedExchange(&shared->recording, 0);
        InterlockedExchange(&shared->disconnected, 1);
    }
    stop_collector();
    if (shared) {
        while (pending_read.load() != pending_write.load())
            drain();
    }
    if (shared) {
        InterlockedExchange(&shared->recording, 0);
        InterlockedExchange(&shared->disconnected, 1);
        UnmapViewOfFile(shared);
        shared = nullptr;
    }
    if (symbols_ready) {
        SymCleanup(process_handle);
        symbols_ready = false;
    }
    if (map_handle) {
        CloseHandle(map_handle);
        map_handle = nullptr;
    }
    if (process_handle) {
        CloseHandle(process_handle);
        process_handle = nullptr;
    }
    if (lease) {
        ReleaseMutex(lease);
        CloseHandle(lease);
        lease = nullptr;
    }
    if (model.connected)
        model.boundary();
    model.connected = false;
    model.recording = false;
}
void Controller::drain() {
    if (!shared)
        return;
    if (model.recording && !atomic_read(&shared->recording)) {
        model.boundary();
        model.recording = false;
        model.snapshot_interval_ms = 0;
        model.status = "Recording stopped after controller heartbeat expired; coverage gap";
    }
    auto begin = ticks();
    std::vector<Event> batch;
    auto read = pending_read.load(std::memory_order_relaxed),
         write = pending_write.load(std::memory_order_acquire);
    const auto count = std::min<size_t>(65536, write - read);
    batch.reserve(count);
    for (size_t i = 0; i < count; ++i)
        batch.push_back(pending_events[(read + i) % pending_capacity]);
    pending_read.store(read + count, std::memory_order_release);
    if (!batch.empty())
        dirty = true;
    std::sort(batch.begin(), batch.end(), [](const Event &a, const Event &b) { return a.end < b.end; });
    auto lost = uint64_t(atomic_read(&shared->dropped)) + local_dropped.load();
    if (lost > model.dropped)
        dirty = true;
    model.loss(lost, atomic_read(&shared->clock));
    for (const auto &event : batch) {
        model.apply(event);
        queue_symbol(event.caller);
        if (event.stack_count <= max_stack_frames)
            for (unsigned i = 0; i < event.stack_count; ++i)
                queue_symbol(event.stack[i]);
    }
    apply_ticks.fetch_add(ticks() - begin, std::memory_order_relaxed);
}
void Controller::sample() {
    if (!shared || !model.recording)
        return;
    drain();
    size_t budget = model.limits.per_scan;
    std::vector<uint64_t> ids(model.watched.begin(), model.watched.end());
    auto it = model.records.upper_bound(cursor);
    for (size_t count = 0; count < model.records.size(); ++count) {
        if (it == model.records.end())
            it = model.records.begin();
        if (it == model.records.end())
            break;
        if (!model.watched.contains(it->first))
            ids.push_back(it->first);
        ++it;
    }
    for (auto id : ids) {
        if (!budget)
            break;
        auto found = model.records.find(id);
        if (found == model.records.end())
            continue;
        auto a = found->second;
        if (a.life != Life::live || !a.size)
            continue;
        size_t n = static_cast<size_t>(std::min<uint64_t>({a.size, model.limits.per_allocation, budget}));
        if (model.limits.retention == Retention::stop_at_capacity &&
            model.retained_bytes + n > model.limits.per_session &&
            a.captures.size() < model.limits.captures) {
            model.sampling_budget_exhausted = true;
            ++model.skipped_captures;
            dirty = true;
            break;
        }
        drain();
        auto check = model.records.find(id);
        if (check == model.records.end() || check->second.life != Life::live ||
            check->second.address != a.address || check->second.end_order != a.end_order)
            continue;
        auto before = atomic_read(&shared->clock);
        bool quiet = transport_quiet(shared) && pending_read.load() == pending_write.load();
        Capture c = read_memory(process_handle, a.address, n);
        c.allocation_size = a.size;
        c.truncated = c.truncated || n < a.size;
        auto after = atomic_read(&shared->clock);
        quiet = quiet && before == after && transport_quiet(shared) &&
                pending_read.load() == pending_write.load();
        drain();
        auto current = model.records.find(id);
        c.attributed = quiet && current != model.records.end() && current->second.life == Life::live &&
                       current->second.address == a.address && current->second.end_order == a.end_order;
        if (!c.attributed)
            c.note = "Concurrent heap activity or incomplete lifetime; attribution unsafe";
        budget -= std::min(budget, n);
        model.capture(id, std::move(c));
        dirty = true;
        cursor = id;
    }
    model.analyze();
}
void Controller::memory_map() {
    std::vector<Region> next;
    std::map<uint64_t, Region> old;
    for (const auto &r : model.regions)
        old[r.base] = r;
    struct Module {
        uint64_t base, size;
        std::string name;
    };
    std::vector<Module> modules;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, model.pid);
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W m{};
        m.dwSize = sizeof(m);
        if (Module32FirstW(snap, &m))
            do {
                modules.push_back(
                    {reinterpret_cast<uint64_t>(m.modBaseAddr), m.modBaseSize, narrow(m.szModule)});
            } while (Module32NextW(snap, &m));
        CloseHandle(snap);
    }
    MEMORY_BASIC_INFORMATION b{};
    uint64_t address = 0;
    auto observed = ticks();
    while (next.size() < 100000 &&
           VirtualQueryEx(process_handle, reinterpret_cast<LPCVOID>(address), &b, sizeof(b)) == sizeof(b)) {
        auto base = reinterpret_cast<uint64_t>(b.BaseAddress);
        if (b.State == MEM_COMMIT) {
            Region r;
            r.base = base;
            r.size = b.RegionSize;
            r.allocation_base = reinterpret_cast<uint64_t>(b.AllocationBase);
            r.protection = b.Protect;
            r.type = b.Type;
            r.observed = observed;
            if (auto previous = old.find(base);
                previous != old.end() && previous->second.allocation_base == r.allocation_base &&
                previous->second.size == r.size && previous->second.type == r.type) {
                r.previous_protection = previous->second.protection;
                r.previous_observed = previous->second.observed;
            }
            for (const auto &m : modules)
                if (base >= m.base && base - m.base < m.size) {
                    r.image_association = true;
                    r.module = m.name;
                    break;
                }
            wchar_t file[32768]{};
            if (GetMappedFileNameW(process_handle, b.BaseAddress, file, 32768))
                r.mapped_file = narrow(file);
            next.push_back(std::move(r));
        }
        if (!b.RegionSize || base + b.RegionSize <= address)
            break;
        address = base + b.RegionSize;
    }
    model.regions = std::move(next);
}
void Controller::resolve() {
    if (!symbols_ready)
        return;
    unsigned budget = 8;
    while (budget && !symbol_jobs.empty()) {
        auto address = symbol_jobs.front();
        symbol_jobs.pop_front();
        queued_symbols.erase(address);
        alignas(SYMBOL_INFOW) std::array<uint8_t, sizeof(SYMBOL_INFOW) + MAX_SYM_NAME * sizeof(wchar_t)>
            buffer{};
        auto *s = reinterpret_cast<SYMBOL_INFOW *>(buffer.data());
        s->SizeOfStruct = sizeof(SYMBOL_INFOW);
        s->MaxNameLen = MAX_SYM_NAME;
        DWORD64 offset = 0;
        FrameSymbol value;
        value.resolved_at = ticks();
        const bool named = SymFromAddrW(process_handle, address, &offset, s) != FALSE && s->NameLen > 0 &&
                           s->NameLen < MAX_SYM_NAME;
        IMAGEHLP_MODULEW64 m{};
        m.SizeOfStruct = sizeof(m);
        if (SymGetModuleInfoW64(process_handle, address, &m) && m.ModuleName[0] && address >= m.BaseOfImage &&
            address - m.BaseOfImage < m.ImageSize) {
            const bool named_in_module = named && offset <= address - m.BaseOfImage;
            value.quality = named_in_module ? SymbolQuality::symbol : SymbolQuality::module;
            value.module = narrow(m.ModuleName);
            value.image_path = narrow(m.ImageName);
            value.rva = address - m.BaseOfImage;
            value.image_size = m.ImageSize;
            value.timestamp = m.TimeDateStamp;
            value.checksum = m.CheckSum;
            value.pdb_age = m.PdbAge;
            memcpy(value.pdb_guid.data(), &m.PdbSig70, value.pdb_guid.size());
            if (named_in_module) {
                value.name = narrow(std::wstring(s->Name, s->NameLen));
                value.displacement = offset;
            }
        }
        model.frame_symbols[address] = std::move(value);
        dirty = true;
        --budget;
    }
}
void Controller::queue_symbol(uint64_t address) {
    if (address && model.frame_symbols.size() + symbol_jobs.size() < 10000 &&
        !model.frame_symbols.contains(address) && queued_symbols.insert(address).second)
        symbol_jobs.push_back(address);
}
std::string Controller::symbol(uint64_t address) const {
    return frame_label(*view(), address);
}
} // namespace hg
