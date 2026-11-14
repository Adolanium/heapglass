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
        l.sample_ms < 50 || l.sample_ms > 60000 || l.stack_frames > max_stack_frames ||
        uint32_t(l.retention) > 1 || l.min_string < 2 || l.min_string > 256 ||
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
const std::vector<uint8_t> &Capture::bytes() const {
    static const std::vector<uint8_t> empty;
    return content ? content->bytes : empty;
}
const std::vector<Text> &Capture::strings() const {
    static const std::vector<Text> empty;
    return content ? content->strings : empty;
}
void Capture::set_bytes(std::vector<uint8_t> bytes) {
    auto value = std::make_shared<CapturedContent>();
    value->bytes = std::move(bytes);
    content = std::move(value);
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
    if (!e.begin || e.end <= e.begin || uint32_t(e.op) > uint32_t(Op::destroy_heap) ||
        e.stack_count > max_stack_frames || e.stack_status > 2 || e.reserved > 3) {
        loss(dropped + 1);
        return;
    }
    if (e.reserved & 2) {
        unsafe_through = std::max(unsafe_through, e.end);
        boundary();
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
        for (auto it = live.lower_bound({heap_handle, 0});
             it != live.end() && it->first.first == heap_handle;)
            it = live.erase(it);
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
        if (a.life == Life::uncertain) {
            a.died = 0;
            a.end_order = e.end;
            return true;
        }
        a.life = Life::freed;
        a.died = e.ticks;
        a.end_order = e.end;
        retired_records.insert(a.id.generation);
        return true;
    };
    if (e.op == Op::destroy_heap) {
        if (e.result != 0)
            return; // RtlDestroyHeap returns NULL on success.
        for (auto &[_, a] : records)
            if (a.heap == e.heap && a.life != Life::freed) {
                if (retire(a)) {
                    auto active_key = std::make_pair(a.heap, a.address);
                    auto active = live.find(active_key);
                    if (active != live.end() && active->second == a.id.generation)
                        live.erase(active);
                }
            }
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
    std::vector<SizePoint> history;
    if (e.op == Op::reallocate && prior) {
        if (e.begin <= prior->end_order) {
            prior->life = Life::uncertain;
            ambiguous = true;
        } else if (e.result == e.previous) {
            prior->size = e.requested;
            prior->caller = e.caller;
            prior->stack = e.stack;
            prior->stack_count = e.stack_count;
            prior->stack_status = e.stack_status;
            prior->end_order = e.end;
            prior->history.push_back({e.ticks, e.requested, e.result});
            while (prior->history.size() > 64)
                prior->history.erase(prior->history.begin());
            return;
        } else {
            if (prior->life == Life::live) {
                parent = prior->id.generation;
                history = prior->history;
            }
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
    a.stack = e.stack;
    a.stack_count = e.stack_count;
    a.stack_status = e.stack_status;
    a.thread = e.thread;
    a.parent = parent;
    a.birth_known = (e.op == Op::allocate || prior != nullptr) && e.begin > unsafe_through;
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
        const bool retired = victim->second.dead;
        heaps.erase(victim);
        history_truncated = true;
        if (!retired)
            boundary();
        ++evicted;
    }
    while (records.size() > limits.records) {
        auto victim = retired_records.empty() ? records.begin() : records.find(*retired_records.begin());
        auto id = victim->first;
        const bool retired = victim->second.life == Life::freed;
        retired_records.erase(id);
        auto key = std::make_pair(victim->second.heap, victim->second.address);
        auto l = live.find(key);
        if (l != live.end() && l->second == id)
            live.erase(l);
        for (const auto &c : victim->second.captures) {
            retained_bytes -= c.bytes().size();
            capture_owners.erase(c.sequence);
        }
        watched.erase(id);
        records.erase(victim);
        ++evicted;
        history_truncated = true;
        if (!retired)
            incomplete = true;
    }
}
void Model::rebuild_retention() {
    capture_owners.clear();
    retired_records.clear();
    retained_bytes = 0;
    next_capture = 1;
    std::vector<std::pair<uint64_t, Capture *>> ordered;
    for (auto &[id, a] : records) {
        if (a.life == Life::freed)
            retired_records.insert(id);
        for (auto &c : a.captures)
            ordered.push_back({id, &c});
    }
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](auto a, auto b) { return a.second->begin < b.second->begin; });
    for (auto [id, c] : ordered) {
        c->sequence = next_capture++;
        capture_owners.emplace(c->sequence, id);
        retained_bytes += c->bytes().size();
    }
}
void Model::trim_captures() {
    auto evict = [&](Allocation &a) {
        auto &c = a.captures.front();
        retained_bytes -= c.bytes().size();
        capture_owners.erase(c.sequence);
        a.captures.erase(a.captures.begin());
        ++evicted_captures;
    };
    for (auto &[_, a] : records)
        while (a.captures.size() > limits.captures)
            evict(a);
    while (retained_bytes > limits.per_session && !capture_owners.empty()) {
        auto id = capture_owners.begin()->second;
        evict(records.at(id));
    }
    sampling_budget_exhausted = false;
}
void Model::capture(uint64_t gen, Capture c) {
    auto it = records.find(gen);
    if (it == records.end())
        return;
    auto &a = it->second;
    if (a.life != Life::live) {
        c.attributed = false;
        c.note = "Lifetime changed around acquisition; attribution invalidated";
    }
    if (!c.allocation_size)
        c.allocation_size = a.size;
    auto evict = [&](Allocation &victim) {
        auto &old = victim.captures.front();
        retained_bytes -= old.bytes().size();
        capture_owners.erase(old.sequence);
        victim.captures.erase(victim.captures.begin());
        ++evicted_captures;
    };
    while (a.captures.size() >= limits.captures && !a.captures.empty())
        evict(a);
    if (c.bytes().size() > limits.per_session) {
        sampling_budget_exhausted = true;
        ++skipped_captures;
        return;
    }
    if (limits.retention == Retention::rolling)
        while (retained_bytes + c.bytes().size() > limits.per_session && !capture_owners.empty())
            evict(records.at(capture_owners.begin()->second));
    if (retained_bytes + c.bytes().size() > limits.per_session) {
        sampling_budget_exhausted = true;
        ++skipped_captures;
        return;
    }
    sampling_budget_exhausted = false;
    auto content = std::make_shared<CapturedContent>();
    content->bytes = c.bytes();
    content->strings = extract_strings(content->bytes, limits.min_string);
    c.content = std::move(content);
    c.sampled_size = c.bytes().size();
    c.entropy = entropy(c.bytes());
    c.hash = content_hash(c.bytes());
    c.sequence = next_capture++;
    capture_owners.emplace(c.sequence, gen);
    retained_bytes += c.bytes().size();
    a.captures.push_back(std::move(c));
}
Model Model::presentation() const {
    Model m;
    m.session = session;
    m.frequency = frequency;
    m.started = started;
    m.dropped = dropped;
    m.observed = observed;
    m.evicted = evicted;
    m.retained_bytes = retained_bytes;
    m.pid = pid;
    m.creation = creation;
    m.process = process;
    m.status = status;
    m.start_time = start_time;
    m.incomplete = incomplete;
    m.history_truncated = history_truncated;
    m.recording = recording;
    m.connected = connected;
    m.offline = offline;
    m.sampling_budget_exhausted = sampling_budget_exhausted;
    m.evicted_captures = evicted_captures;
    m.skipped_captures = skipped_captures;
    m.controller_dropped = controller_dropped;
    m.transport_pending = transport_pending;
    m.analysis_pending = analysis_pending;
    m.active_stack_frames = active_stack_frames;
    m.stack_state = stack_state;
    m.stack_violations = stack_violations;
    m.limits = limits;
    m.records = records;
    m.events = events;
    m.regions = regions;
    m.findings = findings;
    m.snapshots = snapshots;
    m.frame_symbols = frame_symbols;
    m.snapshot_interval_ms = snapshot_interval_ms;
    m.watched = watched;
    return m;
}
static bool compatible(const Capture &a, const Capture &b) {
    return a.attributed && b.attributed && a.offset == b.offset && !a.bytes().empty() &&
           a.bytes().size() == b.bytes().size();
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
        if (c.bytes().size() >= 256 && c.entropy >= limits.high_entropy)
            add("content.entropy/v1", "High-entropy sampled content",
                std::to_string(c.bytes().size()) + " sampled bytes; entropy " + std::to_string(c.entropy) +
                    " bits/byte. Encryption, compression and random data can all produce this property.");
        if (a.captures.size() > 1) {
            const auto &p = a.captures[a.captures.size() - 2];
            if (compatible(p, c)) {
                size_t changed = 0;
                for (size_t i = 0; i < c.bytes().size(); ++i)
                    changed += c.bytes()[i] != p.bytes()[i];
                const double fraction = double(changed) / double(c.bytes().size());
                if (fraction >= limits.changed_fraction ||
                    std::abs(c.entropy - p.entropy) >= limits.entropy_change) {
                    add("content.change/v1", "Large sampled content change",
                        std::to_string(changed) + " of " + std::to_string(c.bytes().size()) +
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
            if (c.bytes().size() >= 256 && c.entropy >= limits.high_entropy)
                add("content.crypto-hypothesis/v1", "Possible crypto-related change pattern",
                    "Repeated transformations temporally coincide with high-entropy samples of this buffer. "
                    "Compression or random replacement are alternatives; no cryptographic operation or key "
                    "rotation is established.",
                    "low");
            if (c.bytes().size() >= 256 && c.entropy >= limits.high_entropy)
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
Snapshot Model::snapshot(std::string name, uint64_t begin, uint64_t end, SnapshotMode mode) const {
    Snapshot s{std::move(name), begin, end, session, incomplete, {}};
    s.history_truncated = history_truncated;
    s.mode = mode;
    s.records.reserve(records.size());
    for (const auto &[_, a] : records) {
        if (mode == SnapshotMode::metadata_only && a.life == Life::freed)
            continue;
        auto copy = a;
        if (mode == SnapshotMode::metadata_only) {
            copy.captures.clear();
            copy.history.clear();
        }
        s.records.push_back(std::move(copy));
    }
    return s;
}
void Model::add_snapshot(Snapshot snapshot) {
    if (next_snapshot == UINT64_MAX)
        throw std::runtime_error("Snapshot identity exhausted");
    if (snapshot.pinned)
        for (auto &s : snapshots)
            s.pinned = false;
    snapshot.id = next_snapshot++;
    snapshots.push_back(std::move(snapshot));
    if (snapshots.size() > 4) {
        auto victim =
            std::find_if(snapshots.begin(), snapshots.end(), [](const auto &s) { return !s.pinned; });
        snapshots.erase(victim);
    }
}
void Model::pin_snapshot(uint64_t id, bool pinned) {
    auto found = std::find_if(snapshots.begin(), snapshots.end(), [=](const auto &s) { return s.id == id; });
    if (found == snapshots.end())
        throw std::invalid_argument("Snapshot is no longer retained");
    if (pinned)
        for (auto &s : snapshots)
            s.pinned = false;
    found->pinned = pinned;
}
std::string frame_label(const Model &model, uint64_t address) {
    auto found = model.frame_symbols.find(address);
    if (found == model.frame_symbols.end())
        return model.offline || !model.connected ? "saved address; unresolved" : "resolving";
    const auto &symbol = found->second;
    if (symbol.quality == SymbolQuality::unresolved)
        return "unresolved at lookup";
    char offset[32]{};
    sprintf_s(offset, "+0x%llX", symbol.quality == SymbolQuality::symbol ? symbol.displacement : symbol.rva);
    return symbol.module + (symbol.quality == SymbolQuality::symbol ? "!" + symbol.name : "") + offset +
           " [cached]";
}
std::vector<Difference> compare(const Snapshot &a, const Snapshot &b) {
    std::map<AllocationId, const Allocation *> left, right;
    for (auto &x : a.records)
        left[x.id] = &x;
    for (auto &x : b.records)
        right[x.id] = &x;
    std::vector<Difference> out;
    for (const auto &[id, r] : right) {
        const auto it = left.find(id);
        if (a.session == b.session && a.mode == SnapshotMode::metadata_only && it == left.end() &&
            r->life == Life::freed && r->died && r->died < a.begin)
            continue;
        Difference d{id, r->address, Change::added, {}};
        if (a.session != b.session || a.incomplete || b.incomplete || r->life == Life::uncertain)
            d.change = Change::uncertain;
        else if (a.mode == SnapshotMode::metadata_only && it == left.end() && r->life == Life::freed)
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
                    for (size_t i = 0; i < x.bytes().size(); ++i)
                        if (x.bytes()[i] != y.bytes()[i])
                            d.byte_offsets.push_back(x.offset + i);
                }
            } else if (l.captures.empty() || r->captures.empty() ||
                       !compatible(l.captures.back(), r->captures.back()))
                d.change = Change::uncertain;
            else {
                const auto &x = l.captures.back();
                const auto &y = r->captures.back();
                d.change = x.bytes() == y.bytes() ? Change::unchanged : Change::modified;
                for (size_t i = 0; i < x.bytes().size(); ++i)
                    if (x.bytes()[i] != y.bytes()[i])
                        d.byte_offsets.push_back(x.offset + i);
            }
        }
        out.push_back(std::move(d));
    }
    for (const auto &[id, l] : left)
        if (!right.contains(id) &&
            !(a.session == b.session && b.mode == SnapshotMode::metadata_only && l->life == Life::freed))
            out.push_back(
                {id, l->address, Change::uncertain, {}}); // Absence may be retention eviction, not a free.
    return out;
}
} // namespace hg
