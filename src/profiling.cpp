#include "profiling.hpp"
#include <algorithm>
#include <cstdio>
#include <limits>

namespace hg {
AllocationSite allocation_site(const Allocation &a) {
    if (a.stack_count)
        return StackSite{{a.stack.begin(), a.stack.begin() + a.stack_count}};
    if (a.caller)
        return CallerSite{a.caller};
    return std::monostate{};
}
std::span<const uint64_t> site_frames(const AllocationSite &site) {
    if (const auto *stack = std::get_if<StackSite>(&site))
        return stack->frames;
    if (const auto *caller = std::get_if<CallerSite>(&site))
        return {&caller->address, 1};
    return {};
}
const char *site_source(const AllocationSite &site) {
    if (std::holds_alternative<StackSite>(site))
        return "bounded stack";
    if (std::holds_alternative<CallerSite>(site))
        return "caller only";
    return "site unavailable";
}
std::string site_label(const AllocationSite &site) {
    const auto frames = site_frames(site);
    if (frames.empty())
        return "Site unavailable";
    char address[32]{};
    sprintf_s(address, "%016llX", frames.front());
    return std::string(address) + " | " + site_source(site) +
           (std::holds_alternative<StackSite>(site) ? " (" + std::to_string(frames.size()) + " frames)" : "");
}
namespace {
void add_bytes(uint64_t &total, uint64_t value, bool &saturated) {
    if (value > std::numeric_limits<uint64_t>::max() - total) {
        total = std::numeric_limits<uint64_t>::max();
        saturated = true;
    } else
        total += value;
}
void add(SiteTotals &totals, const Allocation &a, uint64_t observed) {
    if (!totals.example_generation)
        totals.example_generation = a.id.generation;
    switch (a.life) {
    case Life::live:
        ++totals.live_count;
        add_bytes(totals.live_bytes, a.size, totals.bytes_saturated);
        if (totals.live_count == 1)
            totals.example_generation = a.id.generation;
        if (a.birth_known && a.born && a.born <= observed) {
            if (!totals.oldest_known_birth || a.born < totals.oldest_known_birth)
                totals.oldest_known_birth = a.born;
        } else
            ++totals.unknown_birth_count;
        break;
    case Life::freed:
        ++totals.freed_count;
        add_bytes(totals.freed_bytes, a.size, totals.bytes_saturated);
        break;
    case Life::uncertain:
        ++totals.uncertain_count;
        add_bytes(totals.uncertain_bytes, a.size, totals.bytes_saturated);
        break;
    }
}
using Groups = std::map<AllocationSite, SiteTotals>;
Groups snapshot_groups(const Snapshot &snapshot) {
    Groups groups;
    for (const auto &a : snapshot.records)
        add(groups[allocation_site(a)], a, snapshot.end);
    return groups;
}
} // namespace
AllocationProfile summarize_allocations(const Model &model, uint64_t live_observation) {
    AllocationProfile profile;
    profile.session = model.session;
    profile.frequency = model.frequency;
    profile.incomplete = model.incomplete;
    // A saved QPC belongs to its original host. Never subtract it from this host's clock.
    profile.observed_at = model.started;
    for (const auto &e : model.events)
        profile.observed_at = std::max(profile.observed_at, e.ticks);
    for (const auto &s : model.snapshots)
        profile.observed_at = std::max(profile.observed_at, s.end);
    for (const auto &[_, a] : model.records) {
        profile.observed_at = std::max({profile.observed_at, a.born, a.died});
        for (const auto &c : a.captures)
            profile.observed_at = std::max(profile.observed_at, c.end);
        for (const auto &h : a.history)
            profile.observed_at = std::max(profile.observed_at, h.ticks);
    }
    if (model.connected && !model.offline)
        profile.observed_at = std::max(profile.observed_at, live_observation);
    Groups groups;
    for (const auto &[_, a] : model.records) {
        add(groups[allocation_site(a)], a, profile.observed_at);
        add(profile.totals, a, profile.observed_at);
    }
    for (auto &[site, totals] : groups)
        profile.groups.push_back({std::move(site), totals});
    std::stable_sort(profile.groups.begin(), profile.groups.end(),
                     [](const auto &a, const auto &b) { return a.totals.live_bytes > b.totals.live_bytes; });
    return profile;
}
const char *growth_coverage_name(GrowthCoverage coverage) {
    switch (coverage) {
    case GrowthCoverage::comparable:
        return "Comparable retained observations";
    case GrowthCoverage::need_snapshots:
        return "Capture at least two named snapshots";
    case GrowthCoverage::different_sessions:
        return "Different sessions: growth attribution unavailable";
    case GrowthCoverage::incomplete:
        return "Incomplete history: growth attribution uncertain";
    case GrowthCoverage::overlapping:
        return "Overlapping acquisition intervals: growth attribution uncertain";
    }
    return "Growth attribution unavailable";
}
GrowthReport analyze_growth(std::span<const Snapshot> snapshots) {
    GrowthReport report;
    std::vector<const Snapshot *> ordered;
    for (const auto &s : snapshots)
        ordered.push_back(&s);
    std::stable_sort(ordered.begin(), ordered.end(), [](auto a, auto b) { return a->begin < b->begin; });
    const auto baseline = std::find_if(ordered.begin(), ordered.end(), [](auto s) { return s->pinned; });
    if (baseline != ordered.end())
        ordered.erase(ordered.begin(), baseline);
    report.observations = ordered.size();
    if (ordered.size() < 2)
        return report;
    const auto &first = *ordered.front();
    const auto &last = *ordered.back();
    report.session = first.session;
    report.begin = first.begin;
    report.end = last.end;
    report.first_name = first.name;
    report.last_name = last.name;
    report.coverage = GrowthCoverage::comparable;
    std::vector<Groups> groups;
    std::set<AllocationSite> sites;
    for (size_t i = 0; i < ordered.size(); ++i) {
        const auto &s = *ordered[i];
        if (!s.session || s.session != first.session)
            report.coverage = GrowthCoverage::different_sessions;
        else if (s.incomplete && report.coverage != GrowthCoverage::different_sessions)
            report.coverage = GrowthCoverage::incomplete;
        else if ((s.end < s.begin || (i && ordered[i - 1]->end >= s.begin)) &&
                 report.coverage == GrowthCoverage::comparable)
            report.coverage = GrowthCoverage::overlapping;
        groups.push_back(snapshot_groups(s));
        for (const auto &[site, _] : groups.back())
            sites.insert(site);
    }
    std::set<AllocationId> survivors;
    for (const auto &a : first.records)
        if (a.life == Life::live)
            survivors.insert(a.id);
    for (const auto *s : ordered) {
        std::set<AllocationId> live;
        for (const auto &a : s->records)
            if (a.life == Life::live)
                live.insert(a.id);
        std::erase_if(survivors, [&](const auto &id) { return !live.contains(id); });
    }
    for (const auto &site : sites) {
        GrowthRow row;
        row.site = site;
        for (size_t i = 0; i < groups.size(); ++i) {
            const auto found = groups[i].find(site);
            const SiteTotals totals = found == groups[i].end() ? SiteTotals{} : found->second;
            row.bytes_saturated |= totals.bytes_saturated;
            row.uncertain |= totals.uncertain_count != 0;
            if (i == 0)
                row.before = totals;
            if (i + 1 == groups.size())
                row.after = totals;
            if (i) {
                const auto previous = groups[i - 1].find(site);
                const auto bytes = previous == groups[i - 1].end() ? 0 : previous->second.live_bytes;
                if (totals.live_bytes > bytes)
                    ++row.increasing_intervals;
            }
        }
        row.sustained_growth = report.coverage == GrowthCoverage::comparable && !row.bytes_saturated &&
                               ordered.size() >= 3 && row.increasing_intervals == ordered.size() - 1 &&
                               !row.uncertain;
        report.rows.push_back(std::move(row));
    }
    // Index rows once; each final allocation contributes only to its final observed site.
    std::map<AllocationSite, size_t> row_index;
    for (size_t i = 0; i < report.rows.size(); ++i)
        row_index.emplace(report.rows[i].site, i);
    if (report.coverage == GrowthCoverage::comparable)
        for (const auto &a : last.records)
            if (a.life == Life::live && survivors.contains(a.id)) {
                auto &row = report.rows[row_index.at(allocation_site(a))];
                ++row.survivor_count;
                add_bytes(row.survivor_bytes, a.size, row.bytes_saturated);
            }
    std::stable_sort(report.rows.begin(), report.rows.end(), [](const auto &a, const auto &b) {
        if (a.sustained_growth != b.sustained_growth)
            return a.sustained_growth;
        if (a.after.live_bytes != b.after.live_bytes)
            return a.after.live_bytes > b.after.live_bytes;
        return a.site < b.site;
    });
    return report;
}
std::string byte_delta(uint64_t before, uint64_t after) {
    return after >= before ? "+" + std::to_string(after - before) : "-" + std::to_string(before - after);
}
} // namespace hg
