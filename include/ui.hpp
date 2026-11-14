#pragma once
#include "controller.hpp"
#include <imgui.h>
namespace hg {
class UI {
  public:
    Controller controller;
    void draw();
    void auto_target();
    void show_memory_map();

  private:
    std::shared_ptr<const Model> shown;
    bool display_paused = false, include_bytes = false, show_attach = false, show_launch = false,
         show_limits = false, regions_view = false;
    uint64_t selected = 0, heap_filter = 0, region_selected = 0, min_address = 0, max_address = UINT64_MAX,
             min_size = 0, max_size = UINT64_MAX;
    int state_filter = 0, thread_filter = 0, min_lifetime = 0, capture_index = -1, snap_a = 0, snap_b = 1,
        inspecting_snapshot = -1;
    float entropy_filter = 0;
    char search[256]{}, permission_filter[32]{}, finding_filter[128]{},
        snapshot_name[128] = "Snapshot", launch_path[1024]{}, launch_args[1024]{}, launch_cwd[1024]{};
    std::string ui_error;
    void menu();
    void sidebar();
    void allocation_table();
    void details();
    void bottom();
    void dialogs();
    void select_allocation(uint64_t generation, int capture = -1);
    void navigate(uint64_t address, uint64_t heap = 0, uint64_t at = 0);
};
} // namespace hg
