#include "analysis.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace hg {
Limits validated_limits(Limits l) {
    if (l.per_allocation < 16 || l.per_allocation > 1024 * 1024 || l.per_scan < l.per_allocation ||
        l.per_scan > 16 * 1024 * 1024 || l.per_session < l.per_scan || l.per_session > 256 * 1024 * 1024 ||
        l.sample_ms < 50 || l.sample_ms > 60000 || l.min_string < 2 || l.min_string > 256 ||
        !std::isfinite(l.high_entropy) || l.high_entropy < 0 || l.high_entropy > 8 ||
        !std::isfinite(l.changed_fraction) || l.changed_fraction < 0 || l.changed_fraction > 1 ||
        !std::isfinite(l.entropy_change) || l.entropy_change < 0 || l.entropy_change > 8)
        throw std::runtime_error("Sampling limits or rule thresholds out of range");
    return l;
}
const char *life_name(Life s) {
    switch (s) {
    case Life::live:
        return "LIVE";
    case Life::freed:
        return "FREED";
    case Life::uncertain:
        return "UNCERTAIN";
    }
    return "UNCERTAIN";
}
const char *change_name(Change c) {
    switch (c) {
    case Change::added:
        return "added";
    case Change::freed:
        return "freed";
    case Change::resized:
        return "resized";
    case Change::modified:
        return "modified";
    case Change::unchanged:
        return "unchanged";
    case Change::uncertain:
        return "uncertain";
    }
    return "uncertain";
}
const char *op_name(Op o) {
    switch (o) {
    case Op::allocate:
        return "allocate";
    case Op::reallocate:
        return "reallocate";
    case Op::free:
        return "free";
    case Op::create_heap:
        return "create heap";
    case Op::destroy_heap:
        return "destroy heap";
    }
    return "invalid";
}
double entropy(std::span<const uint8_t> b) {
    if (b.empty())
        return 0;
    std::array<size_t, 256> counts{};
    for (auto x : b)
        ++counts[x];
    double h = 0;
    for (auto n : counts)
        if (n) {
            double p = double(n) / double(b.size());
            h -= p * std::log2(p);
        }
    return h;
}
uint64_t content_hash(std::span<const uint8_t> b) {
    uint64_t h = 14695981039346656037ull;
    for (auto x : b) {
        h ^= x;
        h *= 1099511628211ull;
    }
    return h;
}
static void utf8(std::string &s, uint32_t c) {
    if (c < 128)
        s += char(c);
    else if (c < 2048) {
        s += char(0xc0 | (c >> 6));
        s += char(0x80 | (c & 63));
    } else if (c < 65536) {
        s += char(0xe0 | (c >> 12));
        s += char(0x80 | ((c >> 6) & 63));
        s += char(0x80 | (c & 63));
    } else {
        s += char(0xf0 | (c >> 18));
        s += char(0x80 | ((c >> 12) & 63));
        s += char(0x80 | ((c >> 6) & 63));
        s += char(0x80 | (c & 63));
    }
}
std::vector<Text> extract_strings(std::span<const uint8_t> b, size_t minimum) {
    minimum = std::max<size_t>(2, minimum);
    std::vector<Text> out;
    for (size_t i = 0; i < b.size();) {
        auto start = i;
        while (i < b.size() && b[i] >= 32 && b[i] < 127)
            ++i;
        if (i - start >= minimum)
            out.push_back(
                {start, "ASCII", std::string(reinterpret_cast<const char *>(b.data() + start), i - start)});
        if (i == start)
            ++i;
    }
    // Scan both byte alignments. Accept printable scalar values; reject isolated surrogates.
    for (size_t alignment = 0; alignment < 2; ++alignment)
        for (size_t i = alignment; i + 1 < b.size();) {
            auto start = i;
            size_t count = 0;
            std::string s;
            while (i + 1 < b.size()) {
                uint32_t c = b[i] | (uint32_t(b[i + 1]) << 8);
                size_t step = 2;
                if (c >= 0xd800 && c <= 0xdbff && i + 3 < b.size()) {
                    uint32_t lo = b[i + 2] | (uint32_t(b[i + 3]) << 8);
                    if (lo < 0xdc00 || lo > 0xdfff)
                        break;
                    c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
                    step = 4;
                } else if (c >= 0xd800 && c <= 0xdfff)
                    break;
                if (c < 32 || c == 127 || c == 0xfffe || c == 0xffff)
                    break;
                // Arbitrary binary words are not useful UTF-16 strings. Require a Latin start.
                if (count == 0 && c > 0x24f)
                    break;
                utf8(s, c);
                i += step;
                ++count;
            }
            if (count >= minimum)
                out.push_back({start, "UTF-16LE", std::move(s)});
            if (i == start)
                i += 2;
        }
    return out;
}
void Model::loss(uint64_t n, uint64_t through) {
    if (n > dropped) {
        dropped = n;
        unsafe_through = std::max(unsafe_through, through);
        boundary();
    }
}
void Model::boundary() {
    incomplete = true;
    for (auto &[_, a] : records)
        if (a.life != Life::freed)
            a.life = Life::uncertain;
}
void Model::apply(const Event &e) {
    if (!e.begin || e.end <= e.begin || uint32_t(e.op) > uint32_t(Op::destroy_heap)) {
        loss(dropped + 1);
        return;
    }
    ++observed;
    events.push_back(e);
    if (events.size() > limits.events)
        events.pop_front();
    auto heap_handle = e.op == Op::create_heap ? e.result : e.heap;
    if (!heap_handle)
        return;
    auto &h = heaps[heap_handle];
    if (!h.generation)
        h.generation = next_heap++;
    if (e.op == Op::create_heap) {
        if (!e.result)
            return;
        for (auto &[_, a] : records)
            if (a.heap == heap_handle && a.life != Life::freed)
                a.life = Life::uncertain;
        h = {next_heap++, e.end, false};
        prune();
        return;
    }
    auto key = std::make_pair(e.heap, e.previous);
    auto it = live.find(key);
    Allocation *prior = it == live.end() ? nullptr : &records.at(it->second);
    auto retire = [&](Allocation &a) {
        if (e.begin <= a.end_order || e.begin < h.end_order) {
            a.life = Life::uncertain;
            return false;
        }
        a.life = Life::freed;
        a.died = e.ticks;
        a.end_order = e.end;
        return true;
    };
    if (e.op == Op::destroy_heap) {
        if (e.result != 0)
            return; // RtlDestroyHeap returns NULL on success.
        for (auto &[_, a] : records)
            if (a.heap == e.heap && a.life != Life::freed)
                retire(a);
        h.dead = true;
        h.end_order = std::max(h.end_order, e.end);
        prune();
        return;
    }
    if (e.op == Op::free) {
        if (e.result && prior && retire(*prior))
            live.erase(key);
        return;
    }
    if (!e.result)
        return; // Failed reallocation leaves original record intact, including zero-size failures.
    uint64_t parent = 0;
    bool ambiguous = e.begin <= unsafe_through || e.begin < h.end_order || h.dead;
    std::deque<SizePoint> history;
    if (e.op == Op::reallocate && prior) {
        if (e.begin <= prior->end_order) {
            prior->life = Life::uncertain;
            ambiguous = true;
        } else if (e.result == e.previous) {
            prior->size = e.requested;
            prior->caller = e.caller;
            prior->end_order = e.end;
            prior->history.push_back({e.ticks, e.requested, e.result});
            while (prior->history.size() > 64)
                prior->history.pop_front();
            return;
        } else {
            parent = prior->id.generation;
            history = prior->history;
            if (retire(*prior))
                live.erase(key);
        }
    }
    auto new_key = std::make_pair(e.heap, e.result);
    auto occupied = live.find(new_key);
    bool replace = true;
    if (occupied != live.end()) {
        auto &old = records.at(occupied->second);
        if (e.begin > old.end_order) {
            old.life = Life::uncertain;
        } else {
            old.life = Life::uncertain;
            ambiguous = true;
            if (old.end_order > e.end)
                replace = false;
        }
    }
    Allocation a;
    a.id = {session, h.generation, next_generation++};
    a.heap = e.heap;
    a.address = e.result;
    a.size = e.requested;
    a.born = e.ticks;
    a.begin_order = e.begin;
    a.end_order = e.end;
    a.caller = e.caller;
    a.thread = e.thread;
    a.parent = parent;
    a.birth_known = e.op == Op::allocate || prior != nullptr;
    a.life = ambiguous ? Life::uncertain : Life::live;
    a.history = std::move(history);
    a.history.push_back({e.ticks, e.requested, e.result});
    auto id = a.id.generation;
    records.emplace(id, std::move(a));
    if (replace)
        live[new_key] = id;
    prune();
}
void Model::prune() {
    while (heaps.size() > limits.records) {
        auto victim = std::find_if(heaps.begin(), heaps.end(), [](const auto &p) { return p.second.dead; });
        if (victim == heaps.end())
            victim = heaps.begin();
        heaps.erase(victim);
        boundary();
        ++evicted;
    }
    while (records.size() > limits.records) {
        auto victim = std::find_if(records.begin(), records.end(),
                                   [](const auto &p) { return p.second.life == Life::freed; });
        if (victim == records.end())
            victim = records.begin();
        auto id = victim->first;
        auto key = std::make_pair(victim->second.heap, victim->second.address);
        auto l = live.find(key);
        if (l != live.end() && l->second == id)
            live.erase(l);
        for (const auto &c : victim->second.captures)
            retained_bytes -= c.bytes.size();
        watched.erase(id);
        records.erase(victim);
        ++evicted;
        incomplete = true;
    }
}
void Model::capture(uint64_t gen, Capture c) {
    auto it = records.find(gen);
    if (it == records.end())
        return;
    auto &a = it->second;
    if (a.life != Life::live) {
        c.attributed = false;
        c.note = "Lifetime is uncertain or ended during acquisition";
    }
    if (!c.allocation_size)
        c.allocation_size = a.size;
    c.sampled_size = c.bytes.size();
    c.entropy = entropy(c.bytes);
    c.hash = content_hash(c.bytes);
    c.strings = extract_strings(c.bytes, limits.min_string);
    if (retained_bytes + c.bytes.size() > limits.per_session)
        return;
    retained_bytes += c.bytes.size();
    a.captures.push_back(std::move(c));
    while (a.captures.size() > limits.captures) {
        retained_bytes -= a.captures.front().bytes.size();
        a.captures.pop_front();
    }
}
static bool compatible(const Capture &a, const Capture &b) {
    return a.attributed && b.attributed && a.offset == b.offset && !a.bytes.empty() &&
           a.bytes.size() == b.bytes.size();
}
void Model::analyze() {
    findings.clear();
    for (const auto &[gen, a] : records) {
        if (a.captures.empty())
            continue;
        const auto &c = a.captures.back();
        if (!c.attributed)
            continue;
        auto add = [&](std::string rule, std::string title, std::string evidence,
                       std::string confidence = "medium") {
            findings.push_back({std::move(rule), std::move(title), std::move(evidence), std::move(confidence),
                                a.address + c.offset, c.begin, c.end, gen});
        };
        if (c.bytes.size() >= 256 && c.entropy >= limits.high_entropy)
            add("content.entropy/v1", "High-entropy sampled content",
                std::to_string(c.bytes.size()) + " sampled bytes; entropy " + std::to_string(c.entropy) +
                    " bits/byte. Encryption, compression and random data can all produce this property.");
        if (a.captures.size() > 1) {
            const auto &p = a.captures[a.captures.size() - 2];
            if (compatible(p, c)) {
                size_t changed = 0;
                for (size_t i = 0; i < c.bytes.size(); ++i)
                    changed += c.bytes[i] != p.bytes[i];
                const double fraction = double(changed) / double(c.bytes.size());
                if (fraction >= limits.changed_fraction ||
                    std::abs(c.entropy - p.entropy) >= limits.entropy_change) {
                    add("content.change/v1", "Large sampled content change",
                        std::to_string(changed) + " of " + std::to_string(c.bytes.size()) +
                            " like-for-like bytes changed; entropy delta " +
                            std::to_string(c.entropy - p.entropy) +
                            ". Samples do not identify the writer or count intervening writes.");
                    findings.back().begin = p.begin;
                }
            }
        }
        size_t changes = 0;
        for (size_t i = 1; i < a.captures.size(); ++i)
            if (compatible(a.captures[i - 1], a.captures[i]) && a.captures[i - 1].hash != a.captures[i].hash)
                ++changes;
        if (changes >= 3) {
            add("content.repeat/v1", "Repeated sampled transformations",
                std::to_string(changes) +
                    " changes between compatible captures. No algorithm is established.");
            findings.back().begin = a.captures.front().begin;
            if (c.bytes.size() >= 256 && c.entropy >= limits.high_entropy)
                add("content.crypto-hypothesis/v1", "Possible crypto-related change pattern",
                    "Repeated transformations temporally coincide with high-entropy samples of this buffer. "
                    "Compression or random replacement are alternatives; no cryptographic operation or key "
                    "rotation is established.",
                    "low");
            if (c.bytes.size() >= 256 && c.entropy >= limits.high_entropy)
                findings.back().begin = a.captures.front().begin;
        }
    }
    for (const auto &r : regions) {
        auto base = r.protection & 255u;
        bool exec = base == PAGE_EXECUTE || base == PAGE_EXECUTE_READ || base == PAGE_EXECUTE_READWRITE ||
                    base == PAGE_EXECUTE_WRITECOPY;
        if (!exec)
            continue;
        auto add = [&](std::string rule, std::string title, std::string why) {
            findings.push_back({std::move(rule), std::move(title), std::move(why), "high (observation)",
                                r.base, r.observed, r.observed, 0});
        };
        if (r.type == MEM_PRIVATE)
            add("vm.private-executable/v1", "Private executable region",
                "VirtualQueryEx reports committed MEM_PRIVATE with executable protection. Legitimate "
                "runtimes can generate executable code.");
        if (base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY)
            add("vm.wx/v1", "Writable/executable region",
                "Current sampled protection permits writing and execution.");
        if (!r.image_association)
            add("vm.outside-image/v1", "Executable region without known loaded image",
                "No enumerated module contains this region. Memory type and mapped-file association are "
                "separate facts.");
        const auto previous = r.previous_protection & 255u;
        if (previous == PAGE_READWRITE || previous == PAGE_WRITECOPY)
            add("vm.w-to-x/v1", "Observed writable-to-executable transition",
                "Polling observed writable protection followed by executable protection at this address. "
                "Changes and mapping reuse between polls may be missed.");
        if (previous == PAGE_READWRITE || previous == PAGE_WRITECOPY)
            findings.back().begin = r.previous_observed;
    }
}
Snapshot Model::snapshot(std::string name, uint64_t begin, uint64_t end) const {
    Snapshot s{std::move(name), begin, end, session, incomplete, {}};
    s.records.reserve(records.size());
    for (const auto &[_, a] : records)
        s.records.push_back(a);
    return s;
}
std::vector<Difference> compare(const Snapshot &a, const Snapshot &b) {
    std::map<AllocationId, const Allocation *> left, right;
    for (auto &x : a.records)
        left[x.id] = &x;
    for (auto &x : b.records)
        right[x.id] = &x;
    std::vector<Difference> out;
    for (const auto &[id, r] : right) {
        Difference d{id, r->address, Change::added, {}};
        auto it = left.find(id);
        if (a.session != b.session || a.incomplete || b.incomplete || r->life == Life::uncertain)
            d.change = Change::uncertain;
        else if (it != left.end()) {
            const auto &l = *it->second;
            if (l.life == Life::uncertain)
                d.change = Change::uncertain;
            else if (r->life == Life::freed && l.life != Life::freed)
                d.change = Change::freed;
            else if (l.size != r->size) {
                d.change = Change::resized;
                if (!l.captures.empty() && !r->captures.empty() &&
                    compatible(l.captures.back(), r->captures.back())) {
                    const auto &x = l.captures.back();
                    const auto &y = r->captures.back();
                    for (size_t i = 0; i < x.bytes.size(); ++i)
                        if (x.bytes[i] != y.bytes[i])
                            d.byte_offsets.push_back(x.offset + i);
                }
            } else if (l.captures.empty() || r->captures.empty() ||
                       !compatible(l.captures.back(), r->captures.back()))
                d.change = Change::uncertain;
            else {
                const auto &x = l.captures.back();
                const auto &y = r->captures.back();
                d.change = x.bytes == y.bytes ? Change::unchanged : Change::modified;
                for (size_t i = 0; i < x.bytes.size(); ++i)
                    if (x.bytes[i] != y.bytes[i])
                        d.byte_offsets.push_back(x.offset + i);
            }
        }
        out.push_back(std::move(d));
    }
    for (const auto &[id, l] : left)
        if (!right.contains(id))
            out.push_back(
                {id, l->address, Change::uncertain, {}}); // Absence may be retention eviction, not a free.
    return out;
}
} // namespace hg
