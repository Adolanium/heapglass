#pragma once
#include "analysis.hpp"
#include <variant>

namespace hg {
struct CallerSite {
    uint64_t address{};
    auto operator<=>(const CallerSite &) const = default;
};
struct StackSite {
    std::vector<uint64_t> frames;
    auto operator<=>(const StackSite &) const = default;
};
using AllocationSite = std::variant<std::monostate, CallerSite, StackSite>;
AllocationSite allocation_site(const Allocation &);
std::span<const uint64_t> site_frames(const AllocationSite &);
const char *site_source(const AllocationSite &);
std::string site_label(const AllocationSite &);

struct SiteTotals {
    uint64_t live_count{}, live_bytes{}, freed_count{}, freed_bytes{}, uncertain_count{}, uncertain_bytes{};
    uint64_t unknown_birth_count{}, oldest_known_birth{}, example_generation{};
    bool bytes_saturated{};
};
struct SiteGroup {
    AllocationSite site;
    SiteTotals totals;
};
struct AllocationProfile {
    uint64_t session{}, observed_at{}, frequency{};
    bool incomplete{};
    SiteTotals totals;
    std::vector<SiteGroup> groups;
};
AllocationProfile summarize_allocations(const Model &, uint64_t live_observation);

enum class GrowthCoverage { comparable, need_snapshots, different_sessions, incomplete, overlapping };
struct GrowthRow {
    AllocationSite site;
    SiteTotals before, after;
    uint64_t survivor_count{}, survivor_bytes{};
    unsigned increasing_intervals{};
    bool sustained_growth{}, bytes_saturated{}, uncertain{};
};
struct GrowthReport {
    GrowthCoverage coverage = GrowthCoverage::need_snapshots;
    uint64_t session{}, begin{}, end{};
    std::string first_name, last_name;
    size_t observations{};
    std::vector<GrowthRow> rows;
};
const char *growth_coverage_name(GrowthCoverage);
GrowthReport analyze_growth(std::span<const Snapshot>);
std::string byte_delta(uint64_t before, uint64_t after);
void export_sites_csv(const AllocationProfile &, const std::wstring &,
                      const std::map<uint64_t, FrameSymbol> &);
void export_growth_csv(const GrowthReport &, const std::wstring &, const std::map<uint64_t, FrameSymbol> &);
} // namespace hg
