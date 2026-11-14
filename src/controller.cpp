#include "controller.hpp"
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
Controller::Controller() {
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
            InterlockedExchange(&shared->recording, enabled ? 1 : 0);
            drain();
            model.boundary();
            model.recording = enabled;
            model.status = enabled ? "Recording resumed; earlier coverage gap remains"
                                   : "Recording stopped; target continues; agent resident";
        }
    });
}
void Controller::take_snapshot(std::string name) {
    enqueue([this, name = std::move(name)] {
        auto b = ticks();
        sample();
        auto s = model.snapshot(name, b, ticks());
        size_t bytes = 0;
        for (auto &a : s.records) {
            while (a.captures.size() > 1)
                a.captures.pop_front();
            for (auto &c : a.captures) {
                if (bytes + c.bytes.size() > 8 * 1024 * 1024) {
                    c.bytes.clear();
                    c.attributed = false;
                    c.note = "Snapshot byte retention limit";
                }
                bytes += c.bytes.size();
            }
        }
        model.snapshots.push_back(std::move(s));
        if (model.snapshots.size() > 4)
            model.snapshots.erase(model.snapshots.begin());
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
    enqueue([this, l] { model.limits = validated_limits(l); });
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
        model.status = "Offline session; no process reads";
    });
}
void Controller::csv(std::wstring path) {
    enqueue([this, path = std::move(path)] {
        export_csv(model, path);
        model.status = "CSV exported";
    });
}
void Controller::stall_ingestion(unsigned ms) {
    enqueue([this, ms] {
        if (shared)
            InterlockedExchange64(&shared->heartbeat, GetTickCount64());
        Sleep(std::min(ms, 2000u));
    });
}
void Controller::publish() {
    published.store(std::make_shared<const Model>(model));
    last_publish = GetTickCount64();
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
                InterlockedExchange64(&shared->heartbeat, GetTickCount64());
                if (WaitForSingleObject(process_handle, 0) == WAIT_OBJECT_0) {
                    drain();
                    close();
                    model.status = "Target exited; retained observations available";
                    publish();
                } else {
                    drain();
                    auto now = GetTickCount64();
                    if (force_sample || now - last_sample >= model.limits.sample_ms) {
                        sample();
                        force_sample = false;
                        last_sample = now;
                    }
                    if (now - last_region >= 1000) {
                        memory_map();
                        last_region = now;
                        model.analyze();
                    }
                    resolve();
                }
            }
            if (GetTickCount64() - last_publish >= 250)
                publish();
        } catch (const std::exception &e) {
            model.status = e.what();
            publish();
        }
        Sleep(10);
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
    {
        std::lock_guard l(symbol_mutex);
        symbols.clear();
    }
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
        std::wstring dll = executable_directory() + L"\\HeapglassAgent.dll";
        if (!std::filesystem::is_regular_file(dll))
            throw std::runtime_error("HeapglassAgent.dll is missing beside the executable");
        uint64_t agent = remote_module(pid, L"HeapglassAgent.dll");
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
            agent = remote_module(pid, L"HeapglassAgent.dll");
            if (!agent)
                throw std::runtime_error("Agent DLL could not load. Check signature policy, architecture and "
                                         "process restrictions.");
        }
        HMODULE local = LoadLibraryExW(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
        check(local != nullptr, "Locate initialization export");
        auto entry = GetProcAddress(local, "HeapglassInitialize");
        auto offset = reinterpret_cast<uint64_t>(entry) - reinterpret_cast<uint64_t>(local);
        FreeLibrary(local);
        if (!entry)
            throw std::runtime_error("Agent initialization export missing");
        DWORD result = remote_call(process_handle, agent + offset, nullptr);
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
        model.status = "Recording stopped after controller heartbeat expired; coverage gap";
    }
    std::vector<Event> batch;
    batch.reserve(4096);
    Event e{};
    while (batch.size() < ring_capacity && pop(shared, e))
        batch.push_back(e);
    std::sort(batch.begin(), batch.end(), [](const Event &a, const Event &b) { return a.end < b.end; });
    model.loss(atomic_read(&shared->dropped), atomic_read(&shared->clock));
    for (const auto &x : batch)
        model.apply(x);
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
        if (model.retained_bytes + n > model.limits.per_session)
            break;
        drain();
        auto before = atomic_read(&shared->clock);
        bool quiet =
            atomic_read(&shared->active) == 0 && atomic_read(&shared->write) == atomic_read(&shared->read);
        Capture c;
        c.begin = ticks();
        c.requested = n;
        c.allocation_size = a.size;
        c.truncated = n < a.size;
        MEMORY_BASIC_INFORMATION mbi{};
        SIZE_T got = 0;
        if (VirtualQueryEx(process_handle, reinterpret_cast<LPCVOID>(a.address), &mbi, sizeof(mbi)) ==
                sizeof(mbi) &&
            mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
            auto remain = reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize - a.address;
            n = static_cast<size_t>(std::min<uint64_t>(n, remain));
            c.bytes.resize(n);
            c.protection = mbi.Protect;
            BOOL ok = ReadProcessMemory(process_handle, reinterpret_cast<LPCVOID>(a.address), c.bytes.data(),
                                        n, &got);
            c.bytes.resize(got);
            if (!ok)
                c.note = "Partial or failed ReadProcessMemory";
        } else
            c.note = "Inaccessible, guard, uncommitted or exited target";
        c.end = ticks();
        auto after = atomic_read(&shared->clock);
        quiet = quiet && before == after && atomic_read(&shared->active) == 0;
        drain();
        auto current = model.records.find(id);
        c.attributed = quiet && current != model.records.end() && current->second.life == Life::live &&
                       current->second.address == a.address && current->second.end_order == a.end_order;
        if (!c.attributed)
            c.note = "Concurrent heap activity or incomplete lifetime; attribution unsafe";
        c.truncated = c.truncated || got < c.requested;
        budget -= std::min(budget, n);
        model.capture(id, std::move(c));
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
    for (auto it = model.events.rbegin(); it != model.events.rend() && budget; ++it) {
        if (!it->caller)
            continue;
        {
            std::lock_guard l(symbol_mutex);
            if (symbols.contains(it->caller))
                continue;
        }
        alignas(SYMBOL_INFO) std::array<uint8_t, sizeof(SYMBOL_INFO) + MAX_SYM_NAME> buffer{};
        auto *s = reinterpret_cast<SYMBOL_INFO *>(buffer.data());
        s->SizeOfStruct = sizeof(SYMBOL_INFO);
        s->MaxNameLen = MAX_SYM_NAME;
        DWORD64 offset = 0;
        std::string value;
        if (SymFromAddr(process_handle, it->caller, &offset, s))
            value = std::string(s->Name, s->NameLen) + "+" + std::to_string(offset);
        else {
            IMAGEHLP_MODULE64 m{};
            m.SizeOfStruct = sizeof(m);
            if (SymGetModuleInfo64(process_handle, it->caller, &m))
                value = std::string(m.ModuleName) + "+" + std::to_string(it->caller - m.BaseOfImage);
            else
                value = "unresolved";
        }
        {
            std::lock_guard l(symbol_mutex);
            if (symbols.size() > 10000)
                symbols.clear();
            symbols[it->caller] = std::move(value);
        }
        --budget;
    }
}
std::string Controller::symbol(uint64_t address) const {
    std::lock_guard l(symbol_mutex);
    auto i = symbols.find(address);
    return i == symbols.end() ? "resolving" : i->second;
}
} // namespace hg
