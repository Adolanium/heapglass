#pragma once
#include "controller.hpp"
#include "profiling.hpp"
#include <imgui.h>
namespace hg {
class UI {
  public:
    Controller controller;
    void draw();
    void auto_target();
    void show_memory_map();
    void show_allocation_stack();
    void show_allocation_sites();
    void show_snapshot_growth();

  private:
    std::shared_ptr<const Model> shown;
    bool display_paused = false, include_bytes = false, show_attach = false, show_launch = false,
         show_limits = false, regions_view = false;
    uint64_t selected = 0, region_selected = 0;
    struct Filter {
        uint64_t heap = 0, min_address = 0, max_address = UINT64_MAX, min_size = 0, max_size = UINT64_MAX;
        int state = 0, thread = 0, min_lifetime = 0;
        float entropy = 0;
        std::string text, protection, finding;
        std::optional<AllocationSite> site;
        auto operator<=>(const Filter &) const = default;
    };
    uint64_t heap_filter = 0, min_address = 0, max_address = UINT64_MAX, min_size = 0, max_size = UINT64_MAX;
    int state_filter = 0, thread_filter = 0, min_lifetime = 0, capture_index = -1, snap_a = 0, snap_b = 1;
    uint64_t inspecting_snapshot_id{};
    Limits limits_draft;
    bool metadata_snapshots = false;
    int timed_seconds = 10;
    float entropy_filter = 0;
    char search[256]{}, permission_filter[32]{}, finding_filter[128]{},
        snapshot_name[128] = "Snapshot", launch_path[1024]{}, launch_args[1024]{}, launch_cwd[1024]{};
    std::shared_ptr<const Model> row_owner;
    Filter row_filter;
    std::optional<AllocationSite> site_filter;
    std::vector<const Allocation *> row_cache;
    int sorted_column = -1;
    bool sorted_ascending = true;
    bool select_stack_tab = false;
    enum class InvestigationView { unchanged, sites, growth };
    InvestigationView investigation_view = InvestigationView::unchanged;
    std::string ui_error;
    std::wstring choose_file(bool save, const wchar_t *filter, const wchar_t *extension);
    void menu();
    void sidebar();
    void allocation_table();
    void details();
    void bottom();
    void allocation_sites();
    void snapshot_growth();
    void site_tooltip(const AllocationSite &);
    void dialogs();
    void select_allocation(uint64_t generation, int capture = -1);
    void navigate(uint64_t address, uint64_t heap = 0, uint64_t at = 0);
};
} // namespace hg
