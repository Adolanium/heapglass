#include "analysis.hpp"
#include "profiling.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace hg {
using J = nlohmann::json;
constexpr uint64_t max_file = 128ull * 1024 * 1024;
static std::string hex(const std::vector<uint8_t> &bytes) {
    static const char *h = "0123456789abcdef";
    std::string s;
    s.reserve(bytes.size() * 2);
    for (auto b : bytes) {
        s += h[b >> 4];
        s += h[b & 15];
    }
    return s;
}
static std::vector<uint8_t> unhex(const std::string &s, size_t &budget) {
    if (s.size() % 2 || s.size() / 2 > 1024 * 1024 || s.size() / 2 > budget)
        throw std::runtime_error("Invalid sample length or byte budget");
    auto digit = [](char c) -> unsigned {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        throw std::runtime_error("Invalid sample hex");
    };
    std::vector<uint8_t> b;
    b.reserve(s.size() / 2);
    for (size_t i = 0; i < s.size(); i += 2)
        b.push_back(uint8_t((digit(s[i]) << 4) | digit(s[i + 1])));
    budget -= b.size();
    return b;
}
static std::string text(const J &j, const char *key, size_t limit = 32768) {
    auto s = j.at(key).get<std::string>();
    if (s.size() > limit || s.find('\0') != std::string::npos)
        throw std::runtime_error("Invalid text length");
    return s;
}
static uint64_t number(const J &j, const char *key) {
    const auto &n = j.at(key);
    if (!n.is_number_unsigned())
        throw std::runtime_error(std::string("Invalid unsigned field: ") + key);
    return n.get<uint64_t>();
}
static J capture_json(const Capture &c, bool bytes) {
    J j = {{"begin", c.begin},
           {"end", c.end},
           {"offset", c.offset},
           {"requested", c.requested},
           {"allocation_size", c.allocation_size},
           {"hash", c.hash},
           {"entropy", c.entropy},
           {"protection", c.protection},
           {"attributed", c.attributed},
           {"truncated", c.truncated},
           {"note", c.note},
           {"bytes", bytes ? hex(c.bytes()) : ""},
           {"sample_length", c.sampled_size},
           {"strings", J::array()}};
    for (const auto &t : c.strings())
        j["strings"].push_back({{"offset", t.offset}, {"encoding", t.encoding}, {"value", t.value}});
    return j;
}
static J saved_stack(const std::array<uint64_t, max_stack_frames> &frames, uint32_t count) {
    J result = J::array();
    for (uint32_t i = 0; i < count && i < max_stack_frames; ++i)
        result.push_back(frames[i]);
    return result;
}
static void parse_stack(const J &j, std::array<uint64_t, max_stack_frames> &frames, uint32_t &count,
                        uint32_t &status) {
    if (!j.contains("stack")) {
        count = 0;
        status = 0;
        return;
    }
    const auto &values = j.at("stack");
    if (!values.is_array() || values.size() > max_stack_frames)
        throw std::runtime_error("Stack frame limit exceeded");
    auto state = number(j, "stack_status");
    if (state > 2 || (state == 0 && !values.empty()))
        throw std::runtime_error("Invalid stack status");
    status = static_cast<uint32_t>(state);
    count = static_cast<uint32_t>(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
        if (!values[i].is_number_unsigned() || values[i].get<uint64_t>() == 0)
            throw std::runtime_error("Invalid stack address");
        frames[i] = values[i].get<uint64_t>();
    }
}
static J allocation_json(const Allocation &a, bool bytes) {
    J j = {{"session", a.id.session},
           {"heap_generation", a.id.heap_generation},
           {"generation", a.id.generation},
           {"heap", a.heap},
           {"address", a.address},
           {"size", a.size},
           {"born", a.born},
           {"died", a.died},
           {"begin_order", a.begin_order},
           {"end_order", a.end_order},
           {"caller", a.caller},
           {"parent", a.parent},
           {"thread", a.thread},
           {"life", uint32_t(a.life)},
           {"birth_known", a.birth_known},
           {"history", J::array()},
           {"captures", J::array()}};
    j["stack"] = saved_stack(a.stack, a.stack_count);
    j["stack_status"] = a.stack_status;
    for (const auto &h : a.history)
        j["history"].push_back({{"ticks", h.ticks}, {"size", h.size}, {"address", h.address}});
    for (const auto &c : a.captures)
        j["captures"].push_back(capture_json(c, bytes));
    return j;
}
static Allocation parse_allocation(const J &j, size_t &budget, uint64_t session) {
    Allocation a;
    a.id = {number(j, "session"), number(j, "heap_generation"), number(j, "generation")};
    if (a.id.session != session || !a.id.generation || !a.id.heap_generation)
        throw std::runtime_error("Allocation/session identity mismatch");
    a.heap = number(j, "heap");
    a.address = number(j, "address");
    a.size = number(j, "size");
    a.born = number(j, "born");
    a.died = number(j, "died");
    a.begin_order = number(j, "begin_order");
    a.end_order = number(j, "end_order");
    a.caller = number(j, "caller");
    a.parent = number(j, "parent");
    auto thread = number(j, "thread"), life = number(j, "life");
    if (thread > MAXDWORD || life > 2 || a.size > UINT64_MAX - a.address)
        throw std::runtime_error("Invalid allocation range or state");
    a.thread = static_cast<uint32_t>(thread);
    a.life = static_cast<Life>(life);
    a.birth_known = j.at("birth_known").get<bool>();
    parse_stack(j, a.stack, a.stack_count, a.stack_status);
    if (!j.at("history").is_array() || j.at("history").size() > 64 || !j.at("captures").is_array() ||
        j.at("captures").size() > 8)
        throw std::runtime_error("History limit exceeded");
    for (const auto &h : j.at("history"))
        a.history.push_back({number(h, "ticks"), number(h, "size"), number(h, "address")});
    for (const auto &x : j.at("captures")) {
        Capture c;
        c.begin = number(x, "begin");
        c.end = number(x, "end");
        c.offset = number(x, "offset");
        c.requested = number(x, "requested");
        c.allocation_size = number(x, "allocation_size");
        c.hash = number(x, "hash");
        c.entropy = x.at("entropy").get<double>();
        auto prot = number(x, "protection");
        if (c.end < c.begin || c.offset > UINT64_MAX - a.address ||
            c.allocation_size > UINT64_MAX - a.address || c.offset > c.allocation_size ||
            c.requested > c.allocation_size - c.offset || c.requested > 1024 * 1024 ||
            !std::isfinite(c.entropy) || c.entropy < 0 || c.entropy > 8 || prot > MAXDWORD)
            throw std::runtime_error("Invalid capture range");
        c.protection = static_cast<uint32_t>(prot);
        c.attributed = x.at("attributed").get<bool>();
        c.truncated = x.at("truncated").get<bool>();
        c.note = text(x, "note");
        auto length = number(x, "sample_length");
        c.sampled_size = length;
        if (length > c.requested)
            throw std::runtime_error("Invalid captured length");
        auto content = std::make_shared<CapturedContent>();
        content->bytes = unhex(text(x, "bytes", 2 * 1024 * 1024), budget);
        c.content = content;
        if (!c.bytes().empty() && (c.bytes().size() != length || content_hash(c.bytes()) != c.hash))
            throw std::runtime_error("Sample length/hash mismatch");
        if (!x.at("strings").is_array() || x.at("strings").size() > 262144)
            throw std::runtime_error("String count limit");
        for (const auto &t : x.at("strings")) {
            auto offset = number(t, "offset");
            auto value = text(t, "value", 2 * 1024 * 1024);
            auto encoding = text(t, "encoding", 16);
            if (offset >= length || (encoding != "ASCII" && encoding != "UTF-16LE" && encoding != "UTF-8"))
                throw std::runtime_error("Invalid string location/encoding");
            uint64_t encoded_size = value.size();
            if (encoding == "ASCII") {
                for (unsigned char ch : value)
                    if (ch < 32 || ch > 126)
                        throw std::runtime_error("Invalid ASCII string");
            } else {
                int units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                                int(value.size()), nullptr, 0);
                if (!units)
                    throw std::runtime_error("Invalid Unicode string");
                if (encoding == "UTF-16LE")
                    encoded_size = uint64_t(units) * 2;
            }
            if (encoded_size > length - offset)
                throw std::runtime_error("String extends beyond sampled range");
            content->strings.push_back({offset, std::move(encoding), std::move(value)});
        }
        if (c.bytes().empty()) {
            c.note += "; sampled bytes not retained";
            c.attributed = false;
        }
        a.captures.push_back(std::move(c));
    }
    return a;
}
static J symbol_json(uint64_t address, const FrameSymbol &s) {
    std::vector<uint8_t> guid(s.pdb_guid.begin(), s.pdb_guid.end());
    return {{"address", address},
            {"quality", uint32_t(s.quality)},
            {"module", s.module},
            {"image_path", s.image_path},
            {"name", s.name},
            {"rva", s.rva},
            {"displacement", s.displacement},
            {"resolved_at", s.resolved_at},
            {"image_size", s.image_size},
            {"timestamp", s.timestamp},
            {"checksum", s.checksum},
            {"pdb_age", s.pdb_age},
            {"pdb_guid", hex(guid)},
            {"source", "DbgHelp cached module evidence"}};
}
void save_session(const Model &m, const std::wstring &path, bool bytes) {
    J j = {{"format", "heapglass-session"}, {"version", 3},          {"session", m.session},
           {"frequency", m.frequency},      {"started", m.started},  {"pid", m.pid},
           {"creation", m.creation},        {"process", m.process},  {"start_time", m.start_time},
           {"incomplete", m.incomplete},    {"dropped", m.dropped},  {"observed", m.observed},
           {"evicted", m.evicted},          {"records", J::array()}, {"snapshots", J::array()},
           {"regions", J::array()},         {"events", J::array()}};
    j["history_truncated"] = m.history_truncated;
    j["frame_symbols"] = J::array();
    for (const auto &[address, s] : m.frame_symbols)
        j["frame_symbols"].push_back(symbol_json(address, s));
    j["limits"] = {{"per_allocation", m.limits.per_allocation},
                   {"per_scan", m.limits.per_scan},
                   {"per_session", m.limits.per_session},
                   {"sample_ms", m.limits.sample_ms},
                   {"min_string", m.limits.min_string},
                   {"stack_frames", m.limits.stack_frames},
                   {"retention", uint32_t(m.limits.retention)},
                   {"high_entropy", m.limits.high_entropy},
                   {"changed_fraction", m.limits.changed_fraction},
                   {"entropy_change", m.limits.entropy_change}};
    j["evicted_captures"] = m.evicted_captures;
    j["skipped_captures"] = m.skipped_captures;
    j["sampling_budget_exhausted"] = m.sampling_budget_exhausted;
    for (const auto &[_, a] : m.records)
        j["records"].push_back(allocation_json(a, bytes));
    uint64_t next_id = 1;
    for (const auto &s : m.snapshots)
        next_id = std::max(next_id, s.id + 1);
    for (const auto &s : m.snapshots) {
        J v = {{"name", s.name},       {"begin", s.begin},           {"end", s.end},
               {"session", s.session}, {"incomplete", s.incomplete}, {"records", J::array()}};
        v["id"] = s.id ? s.id : next_id++;
        v["history_truncated"] = s.history_truncated;
        v["pinned"] = s.pinned;
        v["mode"] = uint32_t(s.mode);
        for (const auto &a : s.records)
            v["records"].push_back(allocation_json(a, bytes));
        j["snapshots"].push_back(std::move(v));
    }
    for (const auto &r : m.regions)
        j["regions"].push_back({{"base", r.base},
                                {"size", r.size},
                                {"allocation_base", r.allocation_base},
                                {"observed", r.observed},
                                {"previous_observed", r.previous_observed},
                                {"protection", r.protection},
                                {"previous_protection", r.previous_protection},
                                {"type", r.type},
                                {"image_association", r.image_association},
                                {"module", r.module},
                                {"mapped_file", r.mapped_file}});
    for (const auto &e : m.events)
        j["events"].push_back({{"begin", e.begin},
                               {"end", e.end},
                               {"ticks", e.ticks},
                               {"heap", e.heap},
                               {"address", e.address},
                               {"previous", e.previous},
                               {"requested", e.requested},
                               {"result", e.result},
                               {"caller", e.caller},
                               {"thread", e.thread},
                               {"flags", e.flags},
                               {"op", uint32_t(e.op)},
                               {"reserved", e.reserved},
                               {"stack", saved_stack(e.stack, e.stack_count)},
                               {"stack_status", e.stack_status}});
    std::string data = j.dump(2);
    if (data.size() > max_file)
        throw std::runtime_error(
            "Session exceeds 128 MiB format limit; export without bytes or reduce retention");
    std::filesystem::path destination(path),
        temporary(path + L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(ticks()));
    HANDLE file =
        CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Could not create unique session temporary file");
    DWORD written = 0;
    bool ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
              written == data.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(temporary.c_str());
        throw std::runtime_error("Could not write session; original file preserved");
    }
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        throw std::runtime_error("Could not replace session file");
    }
}
Model load_session(const std::wstring &path) {
    std::ifstream f(std::filesystem::path(path), std::ios::binary | std::ios::ate);
    if (!f || f.tellg() < 0 || uint64_t(f.tellg()) > max_file)
        throw std::runtime_error("Session is missing or exceeds 128 MiB");
    size_t size = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::string data(size, 0);
    if (!f.read(data.data(), size))
        throw std::runtime_error("Could not read session");
    unsigned depth = 0;
    J j = J::parse(data, [&](int d, J::parse_event_t, const J &) {
        if (d > 24)
            throw std::runtime_error("JSON nesting limit exceeded");
        depth = std::max(depth, unsigned(d));
        return true;
    });
    if (text(j, "format", 64) != "heapglass-session" ||
        (number(j, "version") < 1 || number(j, "version") > 3))
        throw std::runtime_error("Unsupported session format/version");
    Model m;
    m.offline = true;
    m.session = number(j, "session");
    m.frequency = number(j, "frequency");
    m.started = number(j, "started");
    auto pid = number(j, "pid");
    if (pid > MAXDWORD || !m.session || !m.frequency)
        throw std::runtime_error("Invalid session identity");
    m.pid = static_cast<DWORD>(pid);
    m.creation = number(j, "creation");
    m.process = text(j, "process");
    m.start_time = text(j, "start_time", 64);
    m.incomplete = j.at("incomplete").get<bool>();
    m.dropped = number(j, "dropped");
    m.observed = number(j, "observed");
    m.evicted = number(j, "evicted");
    m.history_truncated =
        j.contains("history_truncated") ? j.at("history_truncated").get<bool>() : m.evicted != 0;
    if (j.contains("frame_symbols")) {
        const auto &symbols = j.at("frame_symbols");
        if (!symbols.is_array() || symbols.size() > 10000)
            throw std::runtime_error("Saved symbol limit exceeded");
        for (const auto &v : symbols) {
            const auto address = number(v, "address"), quality = number(v, "quality");
            FrameSymbol s;
            if (!address || quality > 2)
                throw std::runtime_error("Invalid saved frame identity");
            s.quality = static_cast<SymbolQuality>(quality);
            s.module = text(v, "module", 256);
            s.image_path = text(v, "image_path");
            s.name = text(v, "name", 8192);
            s.rva = number(v, "rva");
            s.displacement = number(v, "displacement");
            s.resolved_at = number(v, "resolved_at");
            auto image_size = number(v, "image_size"), timestamp = number(v, "timestamp"),
                 checksum = number(v, "checksum"), age = number(v, "pdb_age");
            if (image_size > MAXDWORD || timestamp > MAXDWORD || checksum > MAXDWORD || age > MAXDWORD ||
                s.rva > address || (quality && (!image_size || s.rva >= image_size || s.module.empty())) ||
                (quality == 2 && (s.name.empty() || s.displacement > s.rva)) ||
                (!quality && (!s.module.empty() || !s.name.empty() || s.rva || s.displacement || image_size)))
                throw std::runtime_error("Invalid saved module or symbol range");
            s.image_size = static_cast<uint32_t>(image_size);
            s.timestamp = static_cast<uint32_t>(timestamp);
            s.checksum = static_cast<uint32_t>(checksum);
            s.pdb_age = static_cast<uint32_t>(age);
            size_t guid_budget = 16;
            auto guid = unhex(text(v, "pdb_guid", 32), guid_budget);
            if (guid.size() != 16 || text(v, "source", 128) != "DbgHelp cached module evidence")
                throw std::runtime_error("Invalid saved symbol provenance");
            std::copy(guid.begin(), guid.end(), s.pdb_guid.begin());
            if (!m.frame_symbols.emplace(address, std::move(s)).second)
                throw std::runtime_error("Duplicate saved frame identity");
        }
    }
    if (j.contains("evicted_captures")) {
        m.evicted_captures = number(j, "evicted_captures");
        m.skipped_captures = number(j, "skipped_captures");
        m.sampling_budget_exhausted = j.at("sampling_budget_exhausted").get<bool>();
    }
    if (!j.at("records").is_array() || j.at("records").size() > 50000 || !j.at("snapshots").is_array() ||
        j.at("snapshots").size() > 4 || !j.at("regions").is_array() || j.at("regions").size() > 100000 ||
        !j.at("events").is_array() || j.at("events").size() > 10000)
        throw std::runtime_error("Session record limits exceeded");
    const auto &l = j.at("limits");
    m.limits.per_allocation = number(l, "per_allocation");
    m.limits.per_scan = number(l, "per_scan");
    m.limits.per_session = number(l, "per_session");
    auto interval = number(l, "sample_ms");
    if (interval > 60000)
        throw std::runtime_error("Invalid sampling interval");
    m.limits.sample_ms = static_cast<unsigned>(interval);
    m.limits.min_string = number(l, "min_string");
    if (l.contains("stack_frames")) {
        auto frames = number(l, "stack_frames"), retention = number(l, "retention");
        if (frames > max_stack_frames || retention > 1)
            throw std::runtime_error("Invalid saved capture settings");
        m.limits.stack_frames = static_cast<unsigned>(frames);
        m.limits.retention = static_cast<Retention>(retention);
    }
    m.limits.high_entropy = l.at("high_entropy").get<double>();
    m.limits.changed_fraction = l.at("changed_fraction").get<double>();
    m.limits.entropy_change = l.at("entropy_change").get<double>();
    m.limits = validated_limits(m.limits);
    size_t budget = 64 * 1024 * 1024;
    for (const auto &v : j.at("records")) {
        auto a = parse_allocation(v, budget, m.session);
        for (const auto &c : a.captures)
            m.retained_bytes += c.bytes().size();
        auto id = a.id.generation;
        if (!m.records.emplace(id, std::move(a)).second)
            throw std::runtime_error("Duplicate allocation generation");
        m.next_generation = std::max(m.next_generation, id + 1);
    }
    size_t total_records = m.records.size();
    std::set<uint64_t> snapshot_ids;
    size_t pinned_count = 0;
    for (const auto &v : j.at("snapshots")) {
        Snapshot s{text(v, "name", 256),
                   number(v, "begin"),
                   number(v, "end"),
                   number(v, "session"),
                   v.at("incomplete").get<bool>(),
                   {}};
        if (s.end < s.begin || s.session != m.session || !v.at("records").is_array() ||
            (total_records += v.at("records").size()) > 250000)
            throw std::runtime_error("Invalid snapshot interval or count");
        s.id = v.contains("id") ? number(v, "id") : m.next_snapshot;
        if (!s.id || s.id == UINT64_MAX || !snapshot_ids.insert(s.id).second)
            throw std::runtime_error("Invalid snapshot identity");
        m.next_snapshot = std::max(m.next_snapshot, s.id + 1);
        s.history_truncated =
            v.contains("history_truncated") ? v.at("history_truncated").get<bool>() : m.history_truncated;
        s.pinned = v.value("pinned", false);
        if (s.pinned && ++pinned_count > 1)
            throw std::runtime_error("Only one snapshot baseline can be pinned");
        auto mode = v.contains("mode") ? number(v, "mode") : 0;
        if (mode > 1)
            throw std::runtime_error("Invalid snapshot mode");
        s.mode = static_cast<SnapshotMode>(mode);
        std::set<uint64_t> ids;
        for (const auto &a : v.at("records")) {
            auto parsed = parse_allocation(a, budget, m.session);
            if (s.mode == SnapshotMode::metadata_only &&
                (!parsed.captures.empty() || !parsed.history.empty() || parsed.life == Life::freed))
                throw std::runtime_error("Content in metadata-only snapshot");
            if (!ids.insert(parsed.id.generation).second)
                throw std::runtime_error("Duplicate snapshot identity");
            s.records.push_back(std::move(parsed));
        }
        m.snapshots.push_back(std::move(s));
    }
    for (const auto &v : j.at("regions")) {
        Region r;
        r.base = number(v, "base");
        r.size = number(v, "size");
        r.allocation_base = number(v, "allocation_base");
        r.observed = number(v, "observed");
        r.previous_observed = number(v, "previous_observed");
        if (r.previous_observed > r.observed)
            throw std::runtime_error("Invalid protection observation interval");
        auto p = number(v, "protection"), prev = number(v, "previous_protection"), type = number(v, "type");
        if (!r.size || r.size > UINT64_MAX - r.base || p > MAXDWORD || prev > MAXDWORD ||
            (type != MEM_PRIVATE && type != MEM_MAPPED && type != MEM_IMAGE))
            throw std::runtime_error("Invalid region");
        r.protection = static_cast<uint32_t>(p);
        r.previous_protection = static_cast<uint32_t>(prev);
        r.type = static_cast<uint32_t>(type);
        r.image_association = v.at("image_association").get<bool>();
        r.module = text(v, "module");
        r.mapped_file = text(v, "mapped_file");
        m.regions.push_back(std::move(r));
    }
    for (const auto &v : j.at("events")) {
        Event e;
        e.begin = number(v, "begin");
        e.end = number(v, "end");
        e.ticks = number(v, "ticks");
        e.heap = number(v, "heap");
        e.address = number(v, "address");
        e.previous = number(v, "previous");
        e.requested = number(v, "requested");
        e.result = number(v, "result");
        e.caller = number(v, "caller");
        auto t = number(v, "thread"), fl = number(v, "flags"), op = number(v, "op"),
             re = number(v, "reserved");
        if (t > MAXDWORD || fl > MAXDWORD || op > 4 || re > 3 || !e.begin || e.end <= e.begin)
            throw std::runtime_error("Invalid event");
        e.thread = static_cast<uint32_t>(t);
        e.flags = static_cast<uint32_t>(fl);
        e.op = static_cast<Op>(op);
        e.reserved = static_cast<uint32_t>(re);
        parse_stack(v, e.stack, e.stack_count, e.stack_status);
        m.events.push_back(e);
    }
    m.rebuild_retention();
    m.analyze();
    return m;
}
static std::string csv_text(std::string s) {
    if (!s.empty() &&
        (s[0] == '=' || s[0] == '+' || s[0] == '-' || s[0] == '@' || s[0] == '\t' || s[0] == '\r'))
        s = "'" + s;
    std::string escaped = "\"";
    for (char c : s) {
        if (c == '\"')
            escaped += '\"';
        escaped += c;
    }
    return escaped + '\"';
}
void export_csv(const Model &m, const std::wstring &path) {
    std::ofstream f{std::filesystem::path(path)};
    if (!f)
        throw std::runtime_error("Could not open CSV");
    f << "session,generation,heap_generation,address,requested_size,heap,state,birth_known,born_qpc,died_qpc,"
         "caller,capture_begin_qpc,capture_end_qpc,sample_length,attributed,entropy,note\n";
    for (const auto &[id, a] : m.records) {
        f << m.session << ',' << id << ',' << a.id.heap_generation << ',' << a.address << ',' << a.size << ','
          << a.heap << ',' << life_name(a.life) << ',' << a.birth_known << ',' << a.born << ',' << a.died
          << ',' << a.caller;
        if (a.captures.empty())
            f << ",,,,,,,\n";
        else {
            const auto &c = a.captures.back();
            f << ',' << c.begin << ',' << c.end << ',' << c.bytes().size() << ',' << c.attributed << ','
              << c.entropy << ',' << csv_text(c.note) << '\n';
        }
    }
    if (!f.flush())
        throw std::runtime_error("CSV write failed");
}
static std::string frame_evidence(const AllocationSite &site,
                                  const std::map<uint64_t, FrameSymbol> &symbols) {
    auto evidence = J::array();
    for (auto address : site_frames(site)) {
        const auto found = symbols.find(address);
        evidence.push_back(found == symbols.end() ? J{{"address", address}, {"source", "not resolved"}}
                                                  : symbol_json(address, found->second));
    }
    return evidence.dump();
}
static std::string stack_text(const AllocationSite &site) {
    std::string result;
    for (auto frame : site_frames(site)) {
        if (!result.empty())
            result += ';';
        result += std::to_string(frame);
    }
    return result;
}
void export_sites_csv(const AllocationProfile &profile, const std::wstring &path,
                      const std::map<uint64_t, FrameSymbol> &symbols) {
    std::ofstream file{std::filesystem::path(path)};
    if (!file)
        throw std::runtime_error("Could not open allocation sites CSV");
    file << "session,observed_qpc,incomplete,site_source,frames,live_count,live_requested_bytes,"
            "retained_freed_count,retained_freed_bytes,uncertain_count,uncertain_bytes,unknown_birth_count,"
            "oldest_known_birth_qpc,bytes_saturated,frame_evidence_json\n";
    for (const auto &row : profile.groups) {
        const auto &t = row.totals;
        file << profile.session << ',' << profile.observed_at << ',' << profile.incomplete << ','
             << csv_text(site_source(row.site)) << ',' << csv_text(stack_text(row.site)) << ','
             << t.live_count << ',' << t.live_bytes << ',' << t.freed_count << ',' << t.freed_bytes << ','
             << t.uncertain_count << ',' << t.uncertain_bytes << ',' << t.unknown_birth_count << ','
             << t.oldest_known_birth << ',' << t.bytes_saturated << ','
             << csv_text(frame_evidence(row.site, symbols)) << '\n';
    }
    if (!file.flush())
        throw std::runtime_error("Allocation sites CSV write failed");
}
void export_growth_csv(const GrowthReport &report, const std::wstring &path,
                       const std::map<uint64_t, FrameSymbol> &symbols) {
    std::ofstream file{std::filesystem::path(path)};
    if (!file)
        throw std::runtime_error("Could not open snapshot growth CSV");
    file << "session,coverage,first_snapshot,last_snapshot,begin_qpc,end_qpc,observations,site_source,frames,"
            "before_live_count,after_live_count,before_live_bytes,after_live_bytes,byte_delta,"
            "before_uncertain_count,after_uncertain_count,survivor_count,survivor_bytes,"
            "increasing_intervals,sustained_growth,bytes_saturated,uncertain_observations,frame_evidence_"
            "json\n";
    for (const auto &row : report.rows)
        file << report.session << ',' << csv_text(growth_coverage_name(report.coverage)) << ','
             << csv_text(report.first_name) << ',' << csv_text(report.last_name) << ',' << report.begin << ','
             << report.end << ',' << report.observations << ',' << csv_text(site_source(row.site)) << ','
             << csv_text(stack_text(row.site)) << ',' << row.before.live_count << ',' << row.after.live_count
             << ',' << row.before.live_bytes << ',' << row.after.live_bytes << ','
             << csv_text(byte_delta(row.before.live_bytes, row.after.live_bytes)) << ','
             << row.before.uncertain_count << ',' << row.after.uncertain_count << ',' << row.survivor_count
             << ',' << row.survivor_bytes << ',' << row.increasing_intervals << ',' << row.sustained_growth
             << ',' << row.bytes_saturated << ',' << row.uncertain << ','
             << csv_text(frame_evidence(row.site, symbols)) << '\n';
    if (!file.flush())
        throw std::runtime_error("Snapshot growth CSV write failed");
}
} // namespace hg
