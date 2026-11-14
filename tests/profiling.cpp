#include "profiling.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
using namespace hg;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x))                                                                                            \
            throw std::runtime_error(std::string(__FUNCTION__) + ":" + std::to_string(__LINE__) + ": " #x);  \
    } while (false)
Event alloc(uint64_t order, uint64_t address, uint64_t size, uint64_t outer = 0x2222) {
    Event e{};
    e.op = Op::allocate;
    e.begin = order;
    e.end = order + 1;
    e.ticks = order * 10;
    e.heap = 0x1000;
    e.result = address;
    e.requested = size;
    e.caller = 0x1111;
    e.stack_count = 2;
    e.stack_status = 1;
    e.stack[0] = 0x1111;
    e.stack[1] = outer;
    return e;
}
Model fixture() {
    Model m;
    m.session = 44;
    m.frequency = 1000;
    m.started = 1;
    m.connected = true;
    m.apply(alloc(1, 0x2000, 64));
    m.apply(alloc(3, 0x3000, 128));
    m.apply(alloc(5, 0x4000, 32, 0x3333));
    auto caller = alloc(7, 0x5000, 16);
    caller.stack_count = 0;
    caller.stack_status = 0;
    m.apply(caller);
    m.apply(alloc(9, 0x6000, 24));
    m.records.at(5).life = Life::uncertain;
    m.apply(alloc(11, 0x7000, 8));
    Event freed{};
    freed.op = Op::free;
    freed.begin = 13;
    freed.end = 14;
    freed.heap = 0x1000;
    freed.previous = 0x7000;
    freed.result = 1;
    freed.ticks = 140;
    m.apply(freed);
    m.records.at(2).birth_known = false;
    return m;
}
void site_totals() {
    auto m = fixture();
    auto p = summarize_allocations(m, 1000);
    CHECK(p.groups.size() == 3);
    CHECK(p.observed_at == 1000);
    CHECK(p.totals.live_count == 4);
    CHECK(p.totals.live_bytes == 240);
    CHECK(p.totals.freed_count == 1);
    CHECK(p.totals.freed_bytes == 8);
    CHECK(p.totals.uncertain_count == 1);
    CHECK(p.totals.uncertain_bytes == 24);
    CHECK(p.groups[0].totals.live_bytes == 192);
    CHECK(p.groups[0].totals.unknown_birth_count == 1);
    CHECK(p.groups[0].totals.oldest_known_birth == 10);
    CHECK(site_frames(p.groups[0].site)[1] == 0x2222);
    CHECK(site_frames(p.groups[1].site)[1] == 0x3333);
    CHECK(std::holds_alternative<CallerSite>(p.groups[2].site));
    m.offline = true;
    CHECK(summarize_allocations(m, UINT64_MAX).observed_at == 140);
    m.records.at(4).caller = 0;
    p = summarize_allocations(m, UINT64_MAX);
    CHECK(std::holds_alternative<std::monostate>(p.groups.back().site));
    CHECK(p.groups.back().totals.live_bytes == 16);
    // Imported ranges may individually fit uint64 while their sum does not.
    m.records.at(1).size = UINT64_MAX - 0x2000;
    m.records.at(2).size = 0x3000;
    p = summarize_allocations(m, 0);
    CHECK(p.groups[0].totals.live_bytes == UINT64_MAX);
    CHECK(p.groups[0].totals.bytes_saturated);
    CHECK(p.totals.bytes_saturated);
    CHECK(byte_delta(UINT64_MAX, 0) == "-18446744073709551615");
    CHECK(byte_delta(0, UINT64_MAX) == "+18446744073709551615");
}
std::vector<Snapshot> growing() {
    Model m;
    m.session = 44;
    m.apply(alloc(1, 0x2000, 64));
    std::vector<Snapshot> result;
    result.push_back(m.snapshot("first", 100, 110));
    m.apply(alloc(13, 0x3000, 128));
    result.push_back(m.snapshot("second", 200, 210));
    m.apply(alloc(23, 0x4000, 32));
    result.push_back(m.snapshot("third", 300, 310));
    return result;
}
void reallocation_sites() {
    Model m;
    m.session = 44;
    m.connected = true;
    m.apply(alloc(1, 0x2000, 64));
    std::vector<Snapshot> snapshots{m.snapshot("allocated", 100, 110)};
    auto resized = alloc(13, 0x2000, 128, 0x3333);
    resized.op = Op::reallocate;
    resized.previous = 0x2000;
    m.apply(resized);
    auto p = summarize_allocations(m, 200);
    CHECK(p.groups.size() == 1);
    CHECK(p.groups[0].totals.live_bytes == 128);
    CHECK(site_frames(p.groups[0].site)[1] == 0x3333);
    CHECK(p.groups[0].totals.oldest_known_birth == 10);
    snapshots.push_back(m.snapshot("resized", 200, 210));
    auto moved = alloc(23, 0x3000, 256, 0x4444);
    moved.op = Op::reallocate;
    moved.previous = 0x2000;
    m.apply(moved);
    p = summarize_allocations(m, 300);
    CHECK(p.groups.size() == 2);
    CHECK(p.groups[0].totals.live_bytes == 256);
    CHECK(site_frames(p.groups[0].site)[1] == 0x4444);
    CHECK(p.groups[1].totals.freed_count == 1);
    CHECK(p.groups[1].totals.freed_bytes == 128);
    snapshots.push_back(m.snapshot("moved", 300, 310));
    const auto growth = analyze_growth(snapshots);
    CHECK(growth.rows.size() == 3);
    CHECK(growth.rows[0].after.live_bytes == 256);
    CHECK(growth.rows[0].survivor_count == 0);
    CHECK(!growth.rows[0].sustained_growth);
}
void growth_semantics() {
    auto snapshots = growing();
    auto r = analyze_growth(snapshots);
    CHECK(r.coverage == GrowthCoverage::comparable);
    CHECK(r.observations == 3);
    CHECK(r.rows.size() == 1);
    CHECK(r.rows[0].before.live_count == 1);
    CHECK(r.rows[0].after.live_count == 3);
    CHECK(r.rows[0].before.live_bytes == 64);
    CHECK(r.rows[0].after.live_bytes == 224);
    CHECK(r.rows[0].survivor_count == 1);
    CHECK(r.rows[0].survivor_bytes == 64);
    CHECK(r.rows[0].increasing_intervals == 2);
    CHECK(r.rows[0].sustained_growth);
    CHECK(!analyze_growth(std::span(snapshots).first(2)).rows[0].sustained_growth);
    snapshots[1].records[0].life = Life::uncertain;
    CHECK(!analyze_growth(snapshots).rows[0].sustained_growth);
    CHECK(analyze_growth(snapshots).rows[0].uncertain);
    snapshots = growing();
    snapshots[1].incomplete = true;
    r = analyze_growth(snapshots);
    CHECK(r.coverage == GrowthCoverage::incomplete);
    CHECK(!r.rows[0].sustained_growth);
    CHECK(r.rows[0].survivor_count == 0);
    snapshots = growing();
    snapshots[1].begin = 105;
    r = analyze_growth(snapshots);
    CHECK(r.coverage == GrowthCoverage::overlapping);
    CHECK(!r.rows[0].sustained_growth);
    snapshots = growing();
    snapshots[2].session = 45;
    r = analyze_growth(snapshots);
    CHECK(r.coverage == GrowthCoverage::different_sessions);
    CHECK(!r.rows[0].sustained_growth);
    snapshots = growing();
    // A reused address with a new generation is not a survivor of the first snapshot.
    snapshots.back().records[0].id.generation = 99;
    r = analyze_growth(snapshots);
    CHECK(r.rows[0].survivor_count == 0);
    snapshots = growing();
    snapshots.back().records[0].size = 1;
    snapshots.back().records.resize(1);
    r = analyze_growth(snapshots);
    CHECK(r.rows[0].after.live_bytes == 1);
    CHECK(!r.rows[0].sustained_growth);
    CHECK(byte_delta(r.rows[0].before.live_bytes, r.rows[0].after.live_bytes) == "-63");
}
void churn_and_baselines() {
    Model m;
    m.session = 44;
    m.limits.records = 3;
    m.apply(alloc(1, 0x2000, 64));
    uint64_t order = 3;
    for (unsigned i = 0; i < 12; ++i) {
        m.apply(alloc(order, 0x3000, 32));
        Event freed{};
        freed.op = Op::free;
        freed.begin = order + 2;
        freed.end = order + 3;
        freed.heap = 0x1000;
        freed.previous = 0x3000;
        freed.result = 1;
        m.apply(freed);
        order += 4;
    }
    CHECK(m.history_truncated);
    CHECK(!m.incomplete);
    m.add_snapshot(m.snapshot("baseline", 1000, 1010, SnapshotMode::metadata_only));
    const auto baseline = m.snapshots.front().id;
    m.pin_snapshot(baseline, true);
    m.apply(alloc(order, 0x4000, 128));
    m.add_snapshot(m.snapshot("growth one", 2000, 2010, SnapshotMode::metadata_only));
    m.apply(alloc(order + 2, 0x5000, 256));
    m.add_snapshot(m.snapshot("growth two", 3000, 3010, SnapshotMode::metadata_only));
    CHECK(!m.incomplete);
    CHECK(analyze_growth(m.snapshots).rows[0].sustained_growth);
    CHECK(analyze_growth(m.snapshots).rows[0].after.live_bytes == 448);
    for (unsigned i = 0; i < 5; ++i)
        m.add_snapshot(m.snapshot("later", 4000 + i * 100, 4010 + i * 100, SnapshotMode::metadata_only));
    CHECK(m.snapshots.size() == 4);
    CHECK(m.snapshots.front().id == baseline);
    CHECK(m.snapshots.front().pinned);
    CHECK(m.snapshots[1].begin == 4200);
    m.pin_snapshot(m.snapshots[2].id, true);
    CHECK(analyze_growth(m.snapshots).observations == 2);
    m.apply(alloc(order + 4, 0x6000, 512));
    CHECK(m.incomplete);
    auto metadata = m.snapshot("metadata", 6000, 6010, SnapshotMode::metadata_only);
    CHECK(metadata.history_truncated);
    for (const auto &a : metadata.records) {
        CHECK(a.history.empty());
        CHECK(a.captures.empty());
        CHECK(a.life != Life::freed);
    }
    auto unchanged = fixture();
    const auto before = unchanged.snapshot("metadata", 1000, 1010, SnapshotMode::metadata_only);
    const auto after = unchanged.snapshot("content", 2000, 2010);
    for (const auto &d : compare(before, after))
        CHECK(d.id.generation != 6); // Generation 6 was freed before either observation.
    for (const auto &d : compare(after, before))
        CHECK(d.id.generation != 6);
    auto other_session = after;
    other_session.session = 45;
    for (auto &a : other_session.records)
        a.id.session = 45;
    for (const auto &differences : {compare(before, other_session), compare(other_session, before)}) {
        bool historical_uncertain = false;
        for (const auto &d : differences)
            historical_uncertain |= d.id.generation == 6 && d.change == Change::uncertain;
        CHECK(historical_uncertain);
    }
}
std::string read(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
void exports_and_reload() {
    const auto path = std::filesystem::temp_directory_path() /
                      (L"heapglass-profiles-" + std::to_wstring(GetCurrentProcessId()) + L".csv");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code e;
            std::filesystem::remove(path, e);
        }
    } cleanup{path};
    auto m = fixture();
    export_sites_csv(summarize_allocations(m, 1000), path.wstring(), m.frame_symbols);
    const auto sites = read(path);
    CHECK(sites.find("44,1000,0,\"bounded stack\",\"4369;8738\",2,192,1,8,1,24,1,10,0") != std::string::npos);
    auto snapshots = growing();
    snapshots.front().name = "=untrusted,\"name\"";
    export_growth_csv(analyze_growth(snapshots), path.wstring(), m.frame_symbols);
    const auto growth = read(path);
    CHECK(growth.find("\"'=untrusted,\"\"name\"\"\"") != std::string::npos);
    CHECK(growth.find(",1,3,64,224,\"'+160\",0,0,1,64,2,1,0") != std::string::npos);
    m.snapshots = growing();
    save_session(m, path.wstring(), false);
    auto loaded = load_session(path.wstring());
    CHECK(summarize_allocations(loaded, UINT64_MAX).totals.live_bytes == 240);
    CHECK(analyze_growth(loaded.snapshots).rows[0].after.live_bytes == 224);
    CHECK(analyze_growth(loaded.snapshots).rows[0].sustained_growth);
    FrameSymbol symbol;
    symbol.quality = SymbolQuality::symbol;
    symbol.module = "fixture";
    symbol.image_path = "C:/fixture/target.exe";
    symbol.name = "RetainedBuffer";
    symbol.rva = 0x111;
    symbol.displacement = 0x11;
    symbol.image_size = 0x4000;
    symbol.timestamp = 0x12345678;
    symbol.checksum = 0x4455;
    symbol.resolved_at = 600;
    symbol.pdb_age = 3;
    symbol.pdb_guid[0] = 0xab;
    m.frame_symbols.emplace(0x1111, symbol);
    auto unicode = symbol;
    unicode.name.clear();
    for (unsigned i = 0; i < 1700; ++i)
        unicode.name += "世";
    m.frame_symbols.emplace(0x2222, unicode);
    m.history_truncated = true;
    m.snapshots.clear();
    m.add_snapshot(m.snapshot("metadata baseline", 700, 710, SnapshotMode::metadata_only));
    m.pin_snapshot(m.snapshots.front().id, true);
    save_session(m, path.wstring(), false);
    loaded = load_session(path.wstring());
    CHECK(frame_label(loaded, 0x1111) == "fixture!RetainedBuffer+0x11 [cached]");
    CHECK(loaded.frame_symbols.at(0x1111).timestamp == 0x12345678);
    CHECK(loaded.frame_symbols.at(0x1111).pdb_guid[0] == 0xab);
    CHECK(loaded.frame_symbols.at(0x1111).pdb_age == 3);
    CHECK(loaded.frame_symbols.at(0x2222).name == unicode.name);
    CHECK(loaded.frame_symbols.at(0x2222).name.size() == 5100);
    CHECK(loaded.history_truncated && !loaded.incomplete);
    CHECK(loaded.snapshots.front().pinned);
    CHECK(loaded.snapshots.front().mode == SnapshotMode::metadata_only);
    auto saved = nlohmann::json::parse(read(path));
    CHECK(saved["version"] == 3);
    auto invalid = saved;
    invalid["frame_symbols"][0]["rva"] = 0x4000u;
    {
        std::ofstream out(path);
        out << invalid;
    }
    bool rejected = false;
    try {
        load_session(path.wstring());
    } catch (const std::exception &) {
        rejected = true;
    }
    CHECK(rejected);
    auto legacy = saved;
    legacy["version"] = 2;
    legacy.erase("frame_symbols");
    legacy.erase("history_truncated");
    for (auto &s : legacy["snapshots"]) {
        s.erase("id");
        s.erase("pinned");
        s.erase("mode");
        s.erase("history_truncated");
    }
    {
        std::ofstream out(path);
        out << legacy;
    }
    loaded = load_session(path.wstring());
    CHECK(frame_label(loaded, 0x1111) == "saved address; unresolved");
    CHECK(!loaded.snapshots.front().pinned);
}
int main() {
    try {
        site_totals();
        reallocation_sites();
        growth_semantics();
        churn_and_baselines();
        exports_and_reload();
        std::cout << "Allocation sites, conservative snapshot growth, age provenance, overflow and exports "
                     "passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
