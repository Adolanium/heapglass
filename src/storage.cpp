#include "analysis.hpp"
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
           {"bytes", bytes ? hex(c.bytes) : ""},
           {"sample_length", c.sampled_size},
           {"strings", J::array()}};
    for (const auto &t : c.strings)
        j["strings"].push_back({{"offset", t.offset}, {"encoding", t.encoding}, {"value", t.value}});
    return j;
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
        c.bytes = unhex(text(x, "bytes", 2 * 1024 * 1024), budget);
        if (!c.bytes.empty() && (c.bytes.size() != length || content_hash(c.bytes) != c.hash))
            throw std::runtime_error("Sample length/hash mismatch");
        if (!x.at("strings").is_array() || x.at("strings").size() > 262144)
            throw std::runtime_error("String count limit");
        for (const auto &t : x.at("strings")) {
            auto offset = number(t, "offset");
            auto value = text(t, "value", 2 * 1024 * 1024);
            auto encoding = text(t, "encoding", 16);
            if (offset >= length || (encoding != "ASCII" && encoding != "UTF-16LE"))
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
                encoded_size = uint64_t(units) * 2;
            }
            if (encoded_size > length - offset)
                throw std::runtime_error("String extends beyond sampled range");
            c.strings.push_back({offset, std::move(encoding), std::move(value)});
        }
        if (c.bytes.empty()) {
            c.note += "; sampled bytes not retained";
            c.attributed = false;
        }
        a.captures.push_back(std::move(c));
    }
    return a;
}
void save_session(const Model &m, const std::wstring &path, bool bytes) {
    J j = {{"format", "heapglass-session"}, {"version", 1},          {"session", m.session},
           {"frequency", m.frequency},      {"started", m.started},  {"pid", m.pid},
           {"creation", m.creation},        {"process", m.process},  {"start_time", m.start_time},
           {"incomplete", m.incomplete},    {"dropped", m.dropped},  {"observed", m.observed},
           {"evicted", m.evicted},          {"records", J::array()}, {"snapshots", J::array()},
           {"regions", J::array()},         {"events", J::array()}};
    j["limits"] = {{"per_allocation", m.limits.per_allocation},
                   {"per_scan", m.limits.per_scan},
                   {"per_session", m.limits.per_session},
                   {"sample_ms", m.limits.sample_ms},
                   {"min_string", m.limits.min_string},
                   {"high_entropy", m.limits.high_entropy},
                   {"changed_fraction", m.limits.changed_fraction},
                   {"entropy_change", m.limits.entropy_change}};
    for (const auto &[_, a] : m.records)
        j["records"].push_back(allocation_json(a, bytes));
    for (const auto &s : m.snapshots) {
        J v = {{"name", s.name},       {"begin", s.begin},           {"end", s.end},
               {"session", s.session}, {"incomplete", s.incomplete}, {"records", J::array()}};
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
                               {"reserved", e.reserved}});
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
    if (text(j, "format", 64) != "heapglass-session" || number(j, "version") != 1)
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
    m.limits.high_entropy = l.at("high_entropy").get<double>();
    m.limits.changed_fraction = l.at("changed_fraction").get<double>();
    m.limits.entropy_change = l.at("entropy_change").get<double>();
    m.limits = validated_limits(m.limits);
    size_t budget = 64 * 1024 * 1024;
    for (const auto &v : j.at("records")) {
        auto a = parse_allocation(v, budget, m.session);
        for (const auto &c : a.captures)
            m.retained_bytes += c.bytes.size();
        auto id = a.id.generation;
        if (!m.records.emplace(id, std::move(a)).second)
            throw std::runtime_error("Duplicate allocation generation");
        m.next_generation = std::max(m.next_generation, id + 1);
    }
    size_t total_records = m.records.size();
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
        std::set<uint64_t> ids;
        for (const auto &a : v.at("records")) {
            auto parsed = parse_allocation(a, budget, m.session);
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
        if (t > MAXDWORD || fl > MAXDWORD || op > 4 || re > 1 || !e.begin || e.end <= e.begin)
            throw std::runtime_error("Invalid event");
        e.thread = static_cast<uint32_t>(t);
        e.flags = static_cast<uint32_t>(fl);
        e.op = static_cast<Op>(op);
        e.reserved = static_cast<uint32_t>(re);
        m.events.push_back(e);
    }
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
            f << ',' << c.begin << ',' << c.end << ',' << c.bytes.size() << ',' << c.attributed << ','
              << c.entropy << ',' << csv_text(c.note) << '\n';
        }
    }
    if (!f.flush())
        throw std::runtime_error("CSV write failed");
}
} // namespace hg
