#include "controller.hpp"
#include "profiling.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <psapi.h>
#include <set>
#include <stdexcept>
using namespace hg;
using J = nlohmann::json;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x))                                                                                            \
            throw std::runtime_error(#x);                                                                    \
    } while (false)
template <class F> std::shared_ptr<const Model> await(Controller &c, F condition, unsigned timeout = 15000) {
    auto start = GetTickCount64();
    for (;;) {
        auto m = c.view();
        if (condition(*m))
            return m;
        if (GetTickCount64() - start > timeout)
            throw std::runtime_error("Workflow timeout: " + m->status);
        Sleep(20);
    }
}
double cpu(HANDLE process) {
    FILETIME created{}, ended{}, kernel{}, user{};
    CHECK(GetProcessTimes(process, &created, &ended, &kernel, &user));
    return double(filetime(kernel) + filetime(user)) / 1e7;
}
struct Files {
    std::filesystem::path directory, metrics, ground, session;
    Files() {
        directory = std::filesystem::temp_directory_path() /
                    (L"heapglass-workflow-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                     std::to_wstring(GetTickCount64()));
        CHECK(std::filesystem::create_directory(directory));
        metrics = directory / L"metrics.json";
        ground = directory / L"truth.jsonl";
        session = directory / L"session.json";
    }
    ~Files() {
        std::error_code error;
        for (const auto &p : {metrics, ground, session})
            std::filesystem::remove(p, error);
        std::filesystem::remove(directory, error);
    }
};
struct Target {
    HANDLE process = nullptr;
    DWORD pid{};
    ~Target() {
        if (process) {
            if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
                TerminateProcess(process, 99); // Only the fixture PID created by this test.
                WaitForSingleObject(process, 5000);
            }
            CloseHandle(process);
        }
    }
};
J run(bool instrumented, unsigned seconds) {
    Files files;
    Controller controller;
    Target target;
    const auto arguments = L"--sustained --duration " + std::to_wstring(seconds) + L" --growth-ms " +
                           std::to_wstring(seconds >= 60 ? 5000 : 500) + L" --metrics \"" +
                           files.metrics.wstring() + L"\" --ground \"" + files.ground.wstring() + L"\"";
    const auto exe = executable_directory() + L"\\HeapglassTarget.exe";
    const double own_before = cpu(GetCurrentProcess());
    const auto started = GetTickCount64();
    uint64_t baseline_id = 0;
    if (instrumented) {
        Limits limits;
        limits.stack_frames = 8;
        controller.configure(limits);
        controller.launch(exe, arguments);
        auto m = await(controller, [](const Model &v) { return v.connected; });
        target.pid = m->pid;
        target.process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE,
                                     FALSE, target.pid);
        CHECK(target.process);
        controller.schedule_snapshots(seconds >= 60 ? 10000 : 1300);
        m = await(controller,
                  [](const Model &v) { return v.snapshot_interval_ms != 0 && !v.snapshots.empty(); });
        baseline_id = m->snapshots.front().id;
        CHECK(m->snapshots.front().pinned);
    } else {
        std::wstring command = L"\"" + exe + L"\" " + arguments;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        CHECK(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                             nullptr, &startup, &process));
        target.process = process.hProcess;
        target.pid = process.dwProcessId;
        CloseHandle(process.hThread);
    }
    uint64_t safe = 0, unsafe = 0, peak_pending = 0, peak_working = 0;
    double max_event_age = 0;
    bool requested_save = false, saved = false, saw_growth = false, matched_truth = false;
    bool probe_requested = false, probe_verified = false;
    std::set<std::pair<uint64_t, uint64_t>> captures;
    J memory = J::array();
    uint64_t last_memory = 0, last_progress = 0;
    while (WaitForSingleObject(target.process, 50) == WAIT_TIMEOUT) {
        const auto elapsed = GetTickCount64() - started;
        CHECK(elapsed < uint64_t(seconds + 30) * 1000);
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        CHECK(GetProcessMemoryInfo(GetCurrentProcess(),
                                   reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters), sizeof(counters)));
        peak_working = std::max(peak_working, uint64_t(counters.WorkingSetSize));
        if (elapsed - last_memory >= 1000) {
            memory.push_back({{"seconds", double(elapsed) / 1000.},
                              {"working_bytes", counters.WorkingSetSize},
                              {"private_bytes", counters.PrivateUsage}});
            last_memory = elapsed;
        }
        if (!instrumented)
            continue;
        const auto m = controller.view();
        if (!probe_requested && elapsed > 2500) {
            controller.stall_ingestion(300);
            Sleep(80); // Let this fixture produce a nonempty source queue while its collector is stalled.
            controller.take_snapshot("Backlog probe", SnapshotMode::metadata_only);
            probe_requested = true;
        }
        for (const auto &s : m->snapshots)
            probe_verified |= s.name == "Backlog probe" && s.incomplete && !m->incomplete && m->dropped == 0;
        CHECK(m->records.size() <= 50000 && m->frame_symbols.size() <= 10000);
        peak_pending = std::max(peak_pending, m->analysis_pending + m->transport_pending);
        if (!m->events.empty() && m->frequency) {
            const auto now = ticks(), event = m->events.back().ticks;
            if (now >= event)
                max_event_age = std::max(max_event_age, double(now - event) * 1000. / double(m->frequency));
        }
        for (const auto &[id, a] : m->records)
            for (const auto &c : a.captures)
                if (captures.emplace(id, c.sequence).second)
                    c.attributed ? ++safe : ++unsafe;
        if (m->growth_report)
            for (const auto &r : m->growth_report->rows)
                saw_growth |= r.sustained_growth;
        if (!requested_save && elapsed > uint64_t(seconds) * 500) {
            controller.save(files.session.wstring(), false);
            requested_save = true;
        }
        saved |= m->status == "Session saved";
        if (!matched_truth && elapsed > 2000) {
            std::ifstream truth(files.ground);
            std::string line;
            if (std::getline(truth, line)) {
                auto value = J::parse(line);
                const auto address = value.at("address").get<uint64_t>();
                for (const auto &[_, a] : m->records)
                    if (a.address == address && a.size == 4096 && a.life == Life::live)
                        matched_truth = true;
            }
        }
        if (elapsed - last_progress >= 15000) {
            std::cerr << "sustained " << elapsed / 1000 << " s: events=" << m->observed
                      << " lost=" << m->dropped << " queued=" << m->analysis_pending
                      << " retained=" << m->records.size() << '\n';
            last_progress = elapsed;
        }
    }
    DWORD exit_code = 1;
    CHECK(GetExitCodeProcess(target.process, &exit_code) && exit_code == 0);
    auto m =
        instrumented ? await(controller, [](const Model &v) { return !v.connected; }) : controller.view();
    std::ifstream metrics(files.metrics);
    J result;
    metrics >> result;
    result["instrumented"] = instrumented;
    result["target_cpu_seconds"] = cpu(target.process);
    result["controller_cpu_seconds"] = cpu(GetCurrentProcess()) - own_before;
    result["events_observed"] = m->observed;
    result["events_dropped"] = m->dropped;
    result["controller_peak_sampled_working_bytes"] = peak_working;
    result["max_published_pending_events"] = peak_pending;
    result["max_published_event_age_ms"] = max_event_age;
    result["observed_attributed_captures"] = safe;
    result["observed_unsafe_captures"] = unsafe;
    result["attributed_fraction_of_observed_captures"] =
        safe + unsafe ? double(safe) / double(safe + unsafe) : 0.;
    result["repeated_growth_seen"] = saw_growth;
    result["memory_samples"] = std::move(memory);
    if (instrumented) {
        CHECK(saved && matched_truth && safe > 0 && probe_verified);
        CHECK(m->dropped == 0);
        CHECK(m->snapshot_interval_ms == 0);
        CHECK(m->snapshots.size() == 4 && m->snapshots.front().id == baseline_id &&
              m->snapshots.front().pinned);
        for (const auto &s : m->snapshots) {
            CHECK(s.mode == SnapshotMode::metadata_only);
            CHECK(!s.incomplete);
        }
        auto loaded = load_session(files.session.wstring());
        CHECK(loaded.offline && loaded.snapshot_interval_ms == 0 && !loaded.frame_symbols.empty());
        CHECK(loaded.snapshots.front().pinned);
        CHECK(loaded.snapshots.front().id == baseline_id);
        std::vector<J> truth;
        std::ifstream ground(files.ground);
        std::string line;
        while (std::getline(ground, line))
            truth.push_back(J::parse(line));
        CHECK(!truth.empty());
        AllocationSite retained_site;
        bool site_known = false;
        size_t checked = 0;
        std::map<uint64_t, uint64_t> expected_counts;
        std::vector<Snapshot> verified_snapshots;
        for (const auto &s : m->snapshots) {
            const uint64_t margin = m->frequency / 20; // Exclude logs within 50 ms of the observation cutoff.
            bool ambiguous = false;
            for (const auto &entry : truth) {
                const auto at = entry.at("qpc").get<uint64_t>();
                ambiguous |= at + margin >= s.begin && at <= s.end + margin;
            }
            if (ambiguous)
                continue;
            uint64_t expected = 0, actual = 0;
            for (const auto &entry : truth) {
                const auto address = entry.at("address").get<uint64_t>();
                const auto heap = entry.at("heap").get<uint64_t>();
                const auto at = entry.at("qpc").get<uint64_t>();
                const bool present = at < s.begin;
                expected += present;
                const auto found = std::find_if(s.records.begin(), s.records.end(), [&](const auto &a) {
                    return a.address == address && a.heap == heap;
                });
                CHECK((found != s.records.end()) == present);
                if (present) {
                    CHECK(found->life == Life::live && found->size == 4096);
                    if (!site_known) {
                        retained_site = allocation_site(*found);
                        site_known = true;
                    }
                    CHECK(allocation_site(*found) == retained_site);
                    ++actual;
                }
            }
            CHECK(actual == expected);
            expected_counts.emplace(s.id, expected);
            verified_snapshots.push_back(s);
            ++checked;
        }
        CHECK(checked >= 2 && site_known);
        std::ifstream image(std::filesystem::path(exe), std::ios::binary);
        IMAGE_DOS_HEADER dos{};
        image.read(reinterpret_cast<char *>(&dos), sizeof(dos));
        CHECK(image && dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0);
        image.seekg(dos.e_lfanew);
        IMAGE_NT_HEADERS64 pe{};
        image.read(reinterpret_cast<char *>(&pe), sizeof(pe));
        CHECK(image && pe.Signature == IMAGE_NT_SIGNATURE);
        bool verified_module = false;
        for (auto address : site_frames(retained_site)) {
            const auto symbol = loaded.frame_symbols.find(address);
            if (symbol != loaded.frame_symbols.end() &&
                lstrcmpiA(symbol->second.module.c_str(), "HeapglassTarget") == 0) {
                CHECK(symbol->second.quality != SymbolQuality::unresolved);
                CHECK(symbol->second.image_size == pe.OptionalHeader.SizeOfImage);
                CHECK(symbol->second.timestamp == pe.FileHeader.TimeDateStamp);
                verified_module = true;
            }
        }
        CHECK(verified_module);
        const auto report = analyze_growth(verified_snapshots);
        const auto row = std::find_if(report.rows.begin(), report.rows.end(),
                                      [&](const auto &r) { return r.site == retained_site; });
        CHECK(row != report.rows.end());
        const auto first_count = expected_counts.at(verified_snapshots.front().id);
        const auto last_count = expected_counts.at(verified_snapshots.back().id);
        CHECK(row->before.live_count == first_count && row->before.live_bytes == first_count * 4096);
        CHECK(row->after.live_count == last_count && row->after.live_bytes == last_count * 4096);
        CHECK(row->survivor_count == first_count && row->survivor_bytes == first_count * 4096);
        result["independently_verified_snapshots"] = checked;
        result["target_growth_verified"] = row->sustained_growth;
        result["controlled_backlog_probe_verified"] = probe_verified;
        if (seconds >= 60) {
            CHECK(m->history_truncated);
            CHECK(row->sustained_growth);
            CHECK(loaded.history_truncated && !loaded.incomplete);
        }
    }
    return result;
}
void stopped_baseline() {
    Files files;
    Controller controller;
    Target target;
    controller.launch(executable_directory() + L"\\HeapglassTarget.exe",
                      L"--sustained --duration 3 --growth-ms 500");
    auto m = await(controller, [](const Model &v) { return v.connected && v.observed > 100; });
    target.pid = m->pid;
    target.process =
        OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, target.pid);
    CHECK(target.process);
    struct Mapping {
        HANDLE handle = nullptr;
        Shared *shared = nullptr;
        ~Mapping() {
            if (shared)
                UnmapViewOfFile(shared);
            if (handle)
                CloseHandle(handle);
        }
    } mapping;
    wchar_t name[128]{};
    mapping_name(name, 128, m->pid, m->creation);
    mapping.handle = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
    CHECK(mapping.handle);
    mapping.shared = static_cast<Shared *>(
        MapViewOfFile(mapping.handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Shared)));
    CHECK(mapping.shared && valid(mapping.shared, m->pid, m->creation));
    controller.stall_ingestion(300);
    Sleep(80);
    controller.schedule_snapshots(1000);
    Sleep(20);
    InterlockedExchange(&mapping.shared->recording, 0); // Simulate an agent stop during baseline acquisition.
    m = await(controller, [](const Model &v) {
        return !v.recording && !v.snapshots.empty() &&
               v.status.find("Recording stopped while acquiring") != std::string::npos;
    });
    CHECK(m->snapshot_interval_ms == 0 && m->snapshots.front().incomplete);
    CHECK(WaitForSingleObject(target.process, 0) == WAIT_TIMEOUT);
    CHECK(WaitForSingleObject(target.process, 10000) == WAIT_OBJECT_0);
}
int wmain(int argc, wchar_t **argv) {
    try {
        const bool sustained = argc == 3 && std::wstring(argv[1]) == L"--sustained";
        CHECK(argc == 1 || sustained);
        const unsigned seconds = sustained ? std::stoul(argv[2]) : 8;
        CHECK(!sustained || (seconds >= 60 && seconds <= 600));
        J runs = J::array();
        if (sustained)
            runs.push_back(run(false, seconds));
        runs.push_back(run(true, seconds));
        if (!sustained)
            stopped_baseline();
        std::cout << J{{"workload", "four private-heap worker threads, 16 operations per batch, 25 ms pause; "
                                    "retained 4096-byte buffers"},
                       {"runs", std::move(runs)}}
                         .dump(2)
                  << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Workflow validation failed: " << error.what() << '\n';
        return 1;
    }
}
