#include "controller.hpp"
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <psapi.h>
#include <set>
#include <stdexcept>
#include <thread>

#define REQUIRE(x)                                                                                           \
    do {                                                                                                     \
        if (!(x))                                                                                            \
            throw std::runtime_error(std::string(__FUNCTION__) + ":" + std::to_string(__LINE__) + ": " #x);  \
    } while (false)
using namespace hg;
Event event(Op op, uint64_t begin, uint64_t end, uint64_t result, uint64_t previous = 0, uint64_t size = 64,
            uint64_t heap = 0x1000) {
    Event e{};
    e.op = op;
    e.begin = begin;
    e.end = end;
    e.ticks = end * 10;
    e.heap = heap;
    e.result = result;
    e.address = result;
    e.previous = previous;
    e.requested = size;
    e.thread = 42;
    return e;
}
Model model() {
    Model m;
    m.session = 123;
    m.frequency = 1000;
    m.started = 1;
    m.pid = 42;
    m.creation = 123456;
    m.process = "fixture.exe";
    m.start_time = "fixture";
    return m;
}
void allocation_semantics() {
    auto m = model();
    m.apply(event(Op::allocate, 1, 2, 0x2000));
    REQUIRE(m.records.at(1).size == 64);
    REQUIRE(m.records.at(1).life == Life::live);
    m.apply(event(Op::reallocate, 3, 4, 0, 0x2000, 100));
    REQUIRE(m.records.at(1).size == 64);
    REQUIRE(m.live.at({0x1000, 0x2000}) == 1);
    m.apply(event(Op::reallocate, 5, 6, 0x2000, 0x2000, 0));
    REQUIRE(m.records.at(1).size == 0);
    REQUIRE(m.records.at(1).life == Life::live);
    m.apply(event(Op::reallocate, 7, 8, 0x3000, 0x2000, 128));
    REQUIRE(m.records.at(1).life == Life::freed);
    REQUIRE(m.records.at(2).parent == 1);
    REQUIRE(m.records.at(2).history.size() == 3);
    m.apply(event(Op::free, 9, 10, 0, 0x3000));
    REQUIRE(m.records.at(2).life == Life::live);
    m.apply(event(Op::free, 11, 12, 1, 0x3000));
    REQUIRE(m.records.at(2).life == Life::freed);
    REQUIRE(m.records.at(2).died == 120);
    m.apply(event(Op::allocate, 13, 14, 0x3000));
    REQUIRE(m.records.at(3).id.generation == 3);
    m.apply(event(Op::destroy_heap, 15, 16, 0));
    REQUIRE(m.records.at(3).life == Life::freed);
    auto old = m.records.at(3).id.heap_generation;
    m.apply(event(Op::create_heap, 17, 18, 0x1000));
    m.apply(event(Op::allocate, 19, 20, 0x3000));
    REQUIRE(m.records.at(4).id.heap_generation != old);
    m.apply(event(Op::reallocate, 21, 22, 0x9000, 0x8000));
    REQUIRE(!m.records.at(5).birth_known);
    m.loss(4);
    REQUIRE(m.records.at(5).life == Life::uncertain);
    REQUIRE(m.dropped == 4);
    m.apply(event(Op::allocate, 23, 24, 0xa000));
    REQUIRE(m.records.at(6).life == Life::live);
    m.loss(5, 30);
    m.apply(event(Op::allocate, 25, 26, 0xb000));
    REQUIRE(m.records.at(7).life == Life::uncertain);
    m.apply(event(Op::allocate, 31, 32, 0xc000));
    REQUIRE(m.records.at(8).life == Life::live);
}
void uncertain_boundaries() {
    auto m = model();
    m.apply(event(Op::allocate, 1, 2, 0x2000));
    m.loss(1, 4);
    m.apply(event(Op::free, 5, 6, 1, 0x2000));
    REQUIRE(m.records.at(1).life == Life::uncertain);
    REQUIRE(m.records.at(1).died == 0);
    REQUIRE(m.live.empty());
    auto n = model();
    n.apply(event(Op::allocate, 1, 2, 0x2000));
    n.apply(event(Op::destroy_heap, 3, 4, 0));
    REQUIRE(n.live.empty());
    n.apply(event(Op::create_heap, 5, 6, 0x1000));
    n.apply(event(Op::allocate, 7, 8, 0x2000));
    REQUIRE(n.records.at(1).life == Life::freed);
    REQUIRE(n.records.at(1).died == 40);
    REQUIRE(n.records.at(2).id.heap_generation != n.records.at(1).id.heap_generation);
    auto boundary = model();
    boundary.unsafe_through = 10;
    boundary.apply(event(Op::allocate, 5, 11, 0x2000));
    REQUIRE(boundary.records.at(1).life == Life::uncertain);
    REQUIRE(!boundary.records.at(1).birth_known);
    boundary.apply(event(Op::allocate, 12, 13, 0x3000));
    REQUIRE(boundary.records.at(2).life == Life::live);
    REQUIRE(boundary.records.at(2).birth_known);
}
void delayed_free_race() {
    auto m = model();
    m.apply(event(Op::allocate, 1, 2, 0x2000));
    // Old free starts at 3, releases memory, then is descheduled before completion publication.
    // Another thread obtains the same address and publishes its allocation at 5.
    m.apply(event(Op::allocate, 4, 5, 0x2000));
    m.apply(event(Op::free, 3, 6, 1, 0x2000));
    REQUIRE(m.records.at(2).life == Life::uncertain);
    REQUIRE(m.records.at(2).died == 0);
    REQUIRE(m.live.at({0x1000, 0x2000}) == 2);
    // Transport arrival can reverse even non-overlapping completion order.
    auto n = model();
    n.apply(event(Op::allocate, 1, 2, 0x2000));
    n.apply(event(Op::allocate, 7, 8, 0x2000));
    n.apply(event(Op::free, 3, 4, 1, 0x2000));
    REQUIRE(n.records.at(2).died == 0);
    REQUIRE(n.live.at({0x1000, 0x2000}) == 2);
}
Capture cap(std::vector<uint8_t> bytes, uint64_t begin = 10) {
    Capture c;
    c.set_bytes(std::move(bytes));
    c.begin = begin;
    c.end = begin + 1;
    c.requested = c.bytes().size();
    c.attributed = true;
    return c;
}
void content_snapshots() {
    std::array<uint8_t, 18> data{'x', 0, 'H', 'e', 'l', 'l', 'o', 0, 0, 0, 'W', 0, 'i', 0, 'd', 0, 0, 0};
    auto strings = extract_strings(data, 3);
    bool ascii = false, wide = false;
    for (const auto &s : strings) {
        if (s.encoding == "ASCII" && s.offset == 2 && s.value == "Hello")
            ascii = true;
        if (s.encoding == "UTF-16LE" && s.offset == 10 && s.value == "Wid")
            wide = true;
    }
    REQUIRE(ascii && wide);
    std::vector<uint8_t> all;
    for (int i = 0; i < 256; ++i)
        all.push_back(uint8_t(i));
    REQUIRE(entropy(all) == 8.0);
    REQUIRE(entropy(std::vector<uint8_t>(256, 0)) == 0);
    auto m = model();
    m.apply(event(Op::allocate, 1, 2, 0x2000, 0, 256));
    m.capture(1, cap(all));
    auto a = m.snapshot("before", 1, 10);
    all[7] = 99;
    m.capture(1, cap(all, 20));
    auto b = m.snapshot("after", 20, 30);
    auto d = compare(a, b);
    REQUIRE(d.size() == 1);
    REQUIRE(d[0].change == Change::modified);
    REQUIRE(d[0].byte_offsets == std::vector<uint64_t>{7});
    m.apply(event(Op::free, 3, 4, 1, 0x2000));
    auto c = m.snapshot("freed", 30, 40);
    REQUIRE(compare(b, c)[0].change == Change::freed);
    REQUIRE(m.records.at(1).captures.back().begin == 20);
    m.capture(1, cap({1, 2, 3}));
    REQUIRE(!m.records.at(1).captures.back().attributed);
    auto small = model();
    small.apply(event(Op::allocate, 1, 2, 0x2000));
    small.capture(1, cap({1, 2, 3, 4, 5, 6, 7, 8}));
    small.analyze();
    REQUIRE(small.findings.empty());
    auto entropy_model = model();
    entropy_model.apply(event(Op::allocate, 1, 2, 0x2000, 0, 256));
    entropy_model.capture(1, cap(all));
    entropy_model.analyze();
    REQUIRE(entropy_model.findings.size() == 1);
    REQUIRE(entropy_model.findings[0].title == "High-entropy sampled content");
    Region r;
    r.base = 0x100000;
    r.size = 4096;
    r.type = MEM_PRIVATE;
    r.protection = PAGE_EXECUTE_READ;
    r.previous_protection = PAGE_READWRITE;
    r.observed = 42;
    entropy_model.regions.push_back(r);
    entropy_model.analyze();
    REQUIRE(entropy_model.findings.size() == 4);
    for (auto &f : entropy_model.findings) {
        REQUIRE(f.title.find("malware") == std::string::npos);
        REQUIRE(f.title.find("shellcode") == std::string::npos);
    }
}
void transport() {
    auto *s = static_cast<Shared *>(
        VirtualAlloc(nullptr, sizeof(Shared), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    REQUIRE(s);
    initialize(s, 1, 2, 3);
    constexpr unsigned producers = 4, per = 3000;
    std::atomic<unsigned> done = 0;
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < producers; ++t)
        threads.emplace_back([&, t] {
            for (unsigned n = 0; n < per; ++n) {
                auto e = event(Op::allocate, 1 + t * per + n, 2 + t * per + n, 0x2000);
                e.requested = uint64_t(t) * per + n;
                push(s, e);
            }
            ++done;
        });
    std::set<uint64_t> received;
    Event e{};
    while (done < producers || atomic_read(&s->read) != atomic_read(&s->write)) {
        if (pop(s, e)) {
            REQUIRE(e.result == 0x2000);
            REQUIRE(received.insert(e.requested).second);
        } else
            SwitchToThread();
    }
    for (auto &t : threads)
        t.join();
    REQUIRE(received.size() + size_t(atomic_read(&s->dropped)) == 12000);
    auto reservation = reserve(s);
    REQUIRE(reservation.slot);
    push(s, event(Op::free, 3, 4, 1, 0x2000));
    REQUIRE(!pop(s, e));
    publish(reservation, event(Op::allocate, 1, 2, 0x2000));
    REQUIRE(pop(s, e));
    REQUIRE(e.op == Op::allocate);
    REQUIRE(pop(s, e));
    REQUIRE(e.op == Op::free);
    auto before = atomic_read(&s->dropped);
    for (size_t i = 0; i < ring_capacity + 17; ++i)
        push(s, event(Op::allocate, 1, 2, 0x2000));
    REQUIRE(atomic_read(&s->dropped) - before == 17);
    size_t count = 0;
    while (pop(s, e))
        ++count;
    REQUIRE(count == 32768);
    VirtualFree(s, 0, MEM_RELEASE);
}
void storage() {
    auto p = std::filesystem::temp_directory_path() / L"heapglass-unit-session.json";
    auto m = model();
    m.apply(event(Op::allocate, 1, 2, 0x2000, 0, 8));
    m.capture(1, cap({'H', 'e', 'l', 'l', 'o', 0, 0, 0}));
    m.snapshots.push_back(m.snapshot("one", 10, 11));
    m.limits.high_entropy = 7.6;
    m.records.at(1).stack = {0x10000, 0x20000};
    m.records.at(1).stack_count = 2;
    m.records.at(1).stack_status = 1;
    save_session(m, p.wstring(), true);
    auto loaded = load_session(p.wstring());
    REQUIRE(loaded.offline);
    REQUIRE(loaded.records.at(1).stack_count == 2);
    REQUIRE(loaded.records.at(1).stack[1] == 0x20000);
    REQUIRE(loaded.limits.high_entropy == 7.6);
    REQUIRE(loaded.records.at(1).captures.back().bytes() ==
            std::vector<uint8_t>({'H', 'e', 'l', 'l', 'o', 0, 0, 0}));
    REQUIRE(loaded.snapshots[0].name == "one");
    save_session(m, p.wstring(), false);
    loaded = load_session(p.wstring());
    REQUIRE(loaded.records.at(1).captures.back().bytes().empty());
    REQUIRE(loaded.records.at(1).captures.back().strings()[0].value == "Hello");
    save_session(loaded, p.wstring(), false);
    REQUIRE(load_session(p.wstring()).records.size() == 1);
    nlohmann::json j;
    {
        std::ifstream f(p);
        f >> j;
    }
    auto legacy = j;
    legacy["version"] = 1;
    legacy["limits"].erase("stack_frames");
    legacy["limits"].erase("retention");
    legacy["records"][0].erase("stack");
    legacy["records"][0].erase("stack_status");
    {
        std::ofstream f(p);
        f << legacy;
    }
    auto old = load_session(p.wstring());
    REQUIRE(old.records.at(1).stack_count == 0);
    REQUIRE(old.records.at(1).captures.back().strings()[0].value == "Hello");
    auto oversized = j;
    oversized["records"][0]["stack"] = nlohmann::json::array({1, 2, 3, 4, 5, 6, 7, 8, 9});
    {
        std::ofstream f(p);
        f << oversized;
    }
    bool stack_rejected = false;
    try {
        load_session(p.wstring());
    } catch (...) {
        stack_rejected = true;
    }
    REQUIRE(stack_rejected);
    j["records"][0]["captures"][0]["offset"] = UINT64_MAX;
    {
        std::ofstream f(p);
        f << j;
    }
    bool rejected = false;
    try {
        load_session(p.wstring());
    } catch (...) {
        rejected = true;
    }
    REQUIRE(rejected);
    auto csv = p;
    csv += L".csv";
    m.records.at(1).captures.back().note = "=1+1";
    export_csv(m, csv.wstring());
    std::ifstream csv_file(csv);
    std::string csv_contents((std::istreambuf_iterator<char>(csv_file)), {});
    REQUIRE(csv_contents.find("\"'=1+1\"") != std::string::npos);
    csv_file.close();
    std::filesystem::remove(csv);
    std::filesystem::remove(p);
}
template <class F> std::shared_ptr<const Model> await(Controller &c, F condition, unsigned ms = 15000) {
    auto start = GetTickCount64();
    for (;;) {
        auto m = c.view();
        if (condition(*m))
            return m;
        if (GetTickCount64() - start > ms)
            throw std::runtime_error("Live wait failed: " + m->status);
        Sleep(25);
    }
}
void live() {
    auto ground = std::filesystem::temp_directory_path() / L"heapglass-live.groundtruth.jsonl";
    Controller c;
    c.launch(executable_directory() + L"\\HeapglassTarget.exe",
             L"--duration 20 --ground \"" + ground.wstring() + L"\"");
    auto m = await(c, [](const Model &x) { return x.observed > 300 && x.records.size() > 10; });
    auto pid = m->pid;
    for (const auto &e : m->events)
        if (e.op == Op::free)
            REQUIRE(e.address == e.previous);
    REQUIRE(m->connected);
    REQUIRE(m->dropped == 0);
    std::cout << "live pid=" << pid << " observed=" << m->observed << "\n";
    m = await(c, [](const Model &x) {
        for (auto &[_, a] : x.records)
            for (auto &p : a.captures)
                for (auto &t : p.strings())
                    if (t.encoding == "ASCII" && t.value.find("Heapglass") != std::string::npos &&
                        p.attributed)
                        return true;
        return false;
    });
    m = await(
        c,
        [](const Model &x) {
            for (auto &f : x.findings)
                if (f.rule == "vm.private-executable/v1")
                    return true;
            return false;
        },
        10000);
    bool ground_address = false, semantics = false, compressed = false;
    {
        std::ifstream f(ground);
        std::string line;
        while (std::getline(f, line)) {
            try {
                auto truth = nlohmann::json::parse(line);
                auto op = truth.at("operation").get<std::string>();
                if (op == "api-semantics")
                    semantics = truth.at("success").get<bool>();
                if (op == "compressed")
                    compressed = truth.at("size").get<uint64_t>() > 0;
                if (op == "ascii") {
                    auto address = truth.at("address").get<uint64_t>();
                    for (const auto &[_, a] : m->records)
                        if (a.address == address && a.size == 4096)
                            ground_address = true;
                }
            } catch (const nlohmann::json::exception &) {
            }
        }
    }
    REQUIRE(ground_address);
    REQUIRE(semantics);
    REQUIRE(compressed);
    m = await(c, [](const Model &x) {
        bool utf8 = false, wide = false;
        for (const auto &[_, a] : x.records)
            for (const auto &sample : a.captures)
                if (sample.attributed)
                    for (const auto &text : sample.strings()) {
                        if (text.encoding == "UTF-8" && text.offset == 0 &&
                            text.value == "Heapglass UTF-8 fixture: שלום 世界 😀")
                            utf8 = true;
                        if (text.encoding == "UTF-16LE" && text.offset == 0 &&
                            text.value == "שלום 世界 Ελληνικά Привет 😀")
                            wide = true;
                    }
        return utf8 && wide;
    });
    c.take_snapshot("first");
    await(c, [](const Model &x) { return x.snapshots.size() == 1; });
    Sleep(600);
    c.take_snapshot("second");
    m = await(c, [](const Model &x) { return x.snapshots.size() == 2; });
    REQUIRE(!compare(m->snapshots[0], m->snapshots[1]).empty());
    auto session = std::filesystem::temp_directory_path() / L"heapglass-live.json";
    c.save(session.wstring(), true);
    await(c, [](const Model &x) { return x.status == "Session saved"; });
    auto saved = load_session(session.wstring());
    REQUIRE(saved.snapshots.size() == 2);
    std::filesystem::remove(session);
    c.record(false);
    m = await(c, [](const Model &x) { return !x.recording; });
    auto stopped = m->observed;
    Sleep(300);
    REQUIRE(c.view()->observed >= stopped);
    c.record(true);
    await(c, [](const Model &x) { return x.recording; });
    c.disconnect();
    await(c, [](const Model &x) { return !x.connected; });
    HANDLE target = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    REQUIRE(target);
    REQUIRE(WaitForSingleObject(target, 0) == WAIT_TIMEOUT);
    c.attach(pid);
    m = await(c, [](const Model &x) { return x.connected && x.observed > 50; });
    REQUIRE(m->incomplete);
    c.disconnect();
    await(c, [](const Model &x) { return !x.connected; });
    REQUIRE(WaitForSingleObject(target, 25000) == WAIT_OBJECT_0);
    CloseHandle(target);
    std::filesystem::remove(ground);
    Controller burst;
    burst.launch(executable_directory() + L"\\HeapglassTarget.exe", L"--duration 7 --burst");
    await(burst, [](const Model &x) { return x.connected; });
    burst.stall_ingestion(1500);
    m = await(burst, [](const Model &x) { return x.dropped > 0; }, 10000);
    REQUIRE(m->incomplete);
    std::cout << "overflow dropped=" << m->dropped << "\n";
    await(
        burst,
        [](const Model &x) { return !x.connected && x.status.find("Target exited") != std::string::npos; },
        20000);
}
double cpu_seconds(HANDLE h) {
    FILETIME c{}, x{}, k{}, u{};
    GetProcessTimes(h, &c, &x, &k, &u);
    return double(filetime(k) + filetime(u)) / 1e7;
}
nlohmann::json benchmark_run(bool instrumented, unsigned repeat) {
    Controller controller;
    auto metric = std::filesystem::temp_directory_path() /
                  (L"heapglass-benchmark-" + std::to_wstring(GetCurrentProcessId()) + L".json");
    std::filesystem::remove(metric);
    auto exe = executable_directory() + L"\\HeapglassTarget.exe";
    std::wstring command = L"\"" + exe + L"\" --benchmark 200000 --metrics \"" + metric.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    double own_before = cpu_seconds(GetCurrentProcess());
    auto start = GetTickCount64();
    REQUIRE(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                           nullptr, &si, &pi));
    CloseHandle(pi.hThread);
    if (instrumented) {
        controller.attach(pi.dwProcessId);
        await(controller, [](const Model &m) { return m.connected; });
    }
    size_t target_working = 0, target_private = 0, controller_working = 0;
    while (WaitForSingleObject(pi.hProcess, 10) == WAIT_TIMEOUT) {
        PROCESS_MEMORY_COUNTERS_EX pm{};
        pm.cb = sizeof(pm);
        if (GetProcessMemoryInfo(pi.hProcess, reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pm), sizeof(pm))) {
            target_working = std::max(target_working, size_t(pm.PeakWorkingSetSize));
            target_private = std::max(target_private, size_t(pm.PrivateUsage));
        }
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pm),
                                 sizeof(pm)))
            controller_working = std::max(controller_working, size_t(pm.WorkingSetSize));
        if (GetTickCount64() - start > 30000)
            throw std::runtime_error("Benchmark timeout");
    }
    double target_cpu = cpu_seconds(pi.hProcess);
    CloseHandle(pi.hProcess);
    if (instrumented)
        await(controller, [](const Model &m) { return !m.connected; });
    auto model = controller.view();
    double wall = double(GetTickCount64() - start) / 1000.;
    double own_cpu = cpu_seconds(GetCurrentProcess()) - own_before;
    nlohmann::json result;
    {
        std::ifstream input(metric);
        input >> result;
    }
    std::filesystem::remove(metric);
    result["instrumented"] = instrumented;
    result["repeat"] = repeat;
    result["target_cpu_seconds"] = target_cpu;
    result["controller_cpu_seconds"] = own_cpu;
    result["wall_seconds_including_startup_delay"] = wall;
    result["target_cpu_percent_one_core_over_wall"] = target_cpu / wall * 100.;
    result["controller_cpu_percent_one_core_over_wall"] = own_cpu / wall * 100.;
    result["target_peak_working_bytes"] = target_working;
    result["target_peak_private_bytes"] = target_private;
    result["controller_peak_sampled_working_bytes"] = controller_working;
    result["events_observed"] = model->observed;
    result["events_dropped"] = model->dropped;
    auto pipeline = controller.metrics();
    result["collector_ms"] = double(pipeline.ingest_ticks) * 1000. / double(pipeline.frequency);
    result["apply_ms"] = double(pipeline.apply_ticks) * 1000. / double(pipeline.frequency);
    result["publication_ms"] = double(pipeline.publish_ticks) * 1000. / double(pipeline.frequency);
    result["sampling_ms"] = double(pipeline.sample_ticks) * 1000. / double(pipeline.frequency);
    result["symbols_ms"] = double(pipeline.symbol_ticks) * 1000. / double(pipeline.frequency);
    result["controller_event_losses"] = pipeline.controller_dropped;
    result["max_pending_events"] = pipeline.max_pending;
    PROCESS_MEMORY_COUNTERS_EX final_memory{};
    final_memory.cb = sizeof(final_memory);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&final_memory),
                             sizeof(final_memory)))
        result["controller_peak_sampled_working_bytes"] =
            std::max(controller_working, size_t(final_memory.WorkingSetSize));
    return result;
}
int main(int argc, char **argv) {
    try {
        if (argc > 1 && std::string(argv[1]) == "--live") {
            live();
            std::cout << "live integration passed\n";
        } else if (argc > 1 && std::string(argv[1]) == "--benchmark") {
            nlohmann::json runs = nlohmann::json::array();
            for (unsigned i = 0; i < 3; ++i) {
                runs.push_back(benchmark_run(false, i));
                runs.push_back(benchmark_run(true, i));
            }
            std::cout << nlohmann::json{{"workload",
                                         "200000 HeapAlloc/HeapFree pairs, 64..1087 bytes, one private heap, "
                                         "one thread, no ground-truth logging; 1200 ms fixture startup delay "
                                         "excluded from throughput timer"},
                                        {"runs", runs}}
                             .dump(2)
                      << '\n';
        } else {
            allocation_semantics();
            uncertain_boundaries();
            delayed_free_race();
            content_snapshots();
            transport();
            storage();
            std::cout
                << "core semantics, ordering, transport, content, snapshots, findings and storage passed\n";
        }
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
