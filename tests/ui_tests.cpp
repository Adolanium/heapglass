#include "ui.hpp"
#include <imgui_internal.h>
#include <algorithm>
#include <cstdarg>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>

namespace {
struct Item {
    ImGuiID id{};
    ImRect rect;
    std::string label, window;
};
std::map<ImGuiID, Item> items;
void require(bool value, const std::string &message) {
    if (!value)
        throw std::runtime_error(message);
}
bool in_window(const Item &item, const char *name) {
    return item.window.starts_with(name);
}
} // namespace

// These hooks observe ImGui items. All input below stays in the offscreen context.
void ImGuiTestEngineHook_ItemAdd(ImGuiContext *ctx, ImGuiID id, const ImRect &bb, const ImGuiLastItemData *) {
    if (!id || !ctx->CurrentWindow)
        return;
    auto &item = items[id];
    item.id = id;
    item.rect = bb;
    item.rect.ClipWith(ctx->CurrentWindow->ClipRect);
    item.window = ctx->CurrentWindow->Name;
    // BeginCombo also lacks ItemInfo in this pinned version. Resolve the
    // selector in its actual submitted window scope, never with a saved ID.
    if (item.window == "Allocations & memory map" && ctx->CurrentWindow->GetID("##state") == id)
        item.label = "##state";
    // This ImGui version does not emit ItemInfo for TableHeader. Resolve its
    // submitted ID against the current table's rendered column name.
    if (ctx->CurrentTable && (ctx->CurrentTable->RowFlags & ImGuiTableRowFlags_Headers) &&
        ctx->CurrentTable->CurrentColumn >= 0) {
        const char *name = ImGui::TableGetColumnName(ctx->CurrentTable->CurrentColumn);
        if (name && ctx->CurrentWindow->GetID(name) == id)
            item.label = name;
    }
}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext *ctx, ImGuiID id, const char *label, ImGuiItemStatusFlags) {
    if (!id)
        return;
    auto &item = items[id];
    item.id = id;
    item.label = label ? label : "";
    if (ctx->CurrentWindow)
        item.window = ctx->CurrentWindow->Name;
}
void ImGuiTestEngineHook_Log(ImGuiContext *, const char *, ...) {}
const char *ImGuiTestEngine_FindItemDebugLabel(ImGuiContext *, ImGuiID id) {
    auto it = items.find(id);
    return it == items.end() ? nullptr : it->second.label.c_str();
}

namespace {
class Context {
  public:
    Context() {
        ImGui::CreateContext();
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1600, 1000);
        io.DeltaTime = 1.0f / 60.0f;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char *pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        GImGui->TestEngineHookItems = true;
    }
    ~Context() {
        ImGui::DestroyContext();
    }
};

class Harness {
  public:
    hg::UI app;
    std::string log;
    void frame() {
        items.clear();
        ImGui::NewFrame();
        ImGui::LogToBuffer();
        app.draw();
        log = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::Render();
        require(ImGui::GetDrawData()->CmdListsCount > 0, "UI produced no draw commands");
    }
    Item find(const char *window, const std::string &label) {
        const Item *first = nullptr;
        for (const auto &[_, item] : items)
            if (in_window(item, window) && item.label == label && item.rect.GetWidth() > 0 &&
                item.rect.GetHeight() > 0 && (!first || item.rect.Min.y < first->rect.Min.y))
                first = &item;
        if (first)
            return *first;
        throw std::runtime_error("Visible item missing: " + std::string(window) + "/" + label);
    }
    void click(const char *window, const std::string &label) {
        frame(); // IDs and positions can change with filtering, sorting and docking.
        const auto item = find(window, label);
        // SpanAllColumns rows cover column resize handles. Click their address
        // area, where a user selects the row, instead of the full-row midpoint.
        const ImVec2 point(item.rect.Min.x + std::min(20.0f, item.rect.GetWidth() * .5f),
                           item.rect.GetCenter().y);
        auto &io = ImGui::GetIO();
        io.AddMousePosEvent(point.x, point.y);
        frame();
        io.AddMouseButtonEvent(0, true);
        frame();
        io.AddMouseButtonEvent(0, false);
        frame();
        frame(); // Some tabs and Inspector content are submitted before the click handler.
    }
    void key(ImGuiKey key, bool control = false) {
        frame();
        auto &io = ImGui::GetIO();
        if (control)
            io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(key, true);
        frame();
        io.AddKeyEvent(key, false);
        if (control)
            io.AddKeyEvent(ImGuiMod_Ctrl, false);
        frame();
        frame();
    }
    void contains(const std::string &expected) const {
        if (log.find(expected) == std::string::npos) {
            std::cerr << "Rendered log at failure:\n" << log << '\n';
            throw std::runtime_error("Rendered text missing: " + expected);
        }
    }
    std::vector<std::string> allocation_rows() const {
        std::vector<Item> rows;
        for (const auto &[_, item] : items)
            if (in_window(item, "Allocations & memory map") && item.label.size() == 16 &&
                item.label.starts_with("000000000000") && item.rect.GetHeight() > 0)
                rows.push_back(item);
        std::sort(rows.begin(), rows.end(),
                  [](const Item &a, const Item &b) { return a.rect.Min.y < b.rect.Min.y; });
        std::vector<std::string> result;
        for (const auto &row : rows)
            result.push_back(row.label);
        return result;
    }
};

void capture(hg::Model &model, uint64_t generation, uint64_t ticks, std::string text) {
    hg::Capture sample;
    sample.begin = ticks;
    sample.end = ticks + 1;
    sample.requested = text.size() + 1;
    sample.protection = PAGE_READWRITE;
    sample.attributed = true;
    std::vector<uint8_t> bytes(text.begin(), text.end());
    bytes.push_back(0);
    sample.set_bytes(std::move(bytes));
    model.capture(generation, std::move(sample));
}
hg::Model offline_fixture() {
    hg::Model model;
    model.session = 0x1234;
    model.frequency = 1000;
    model.pid = 4242;
    model.process = "OFFLINE UI FIXTURE.exe";
    model.status = "OFFLINE UI FIXTURE: synthetic saved observations";
    model.start_time = "Offline fixture, no target process";
    model.offline = true;
    const uint64_t sizes[] = {768, 128, 384};
    const char *strings[] = {"alpha-current-marker", "beta-current-marker", "gamma-current-marker"};
    for (uint64_t i = 0; i < 3; ++i) {
        hg::Allocation a;
        a.id = {model.session, 1, i + 1};
        a.heap = 0x8000;
        a.address = 0x1000 + i * 0x1000;
        a.size = sizes[i];
        a.born = 10;
        a.thread = 7;
        a.caller = i == 1 ? 0x77770002 : 0x77770001;
        if (i != 1) {
            a.stack_count = 2;
            a.stack_status = 1;
            a.stack[0] = 0x77770001;
            a.stack[1] = 0x88880001;
        }
        a.history.push_back({10, a.size, a.address});
        model.records.emplace(i + 1, a);
        capture(model, i + 1, 100, strings[i]);
    }
    model.snapshots.push_back(model.snapshot("Offline before", 100, 110));
    capture(model, 1, 200, "alpha-updated-marker");
    model.snapshots.push_back(model.snapshot("Offline after", 200, 210));
    hg::Region region;
    region.base = 0x1000;
    region.size = 0x4000;
    region.allocation_base = 0x1000;
    region.protection = PAGE_READWRITE;
    region.type = MEM_PRIVATE;
    region.observed = 200;
    model.regions.push_back(region);
    return model;
}

struct TemporarySession {
    std::filesystem::path path;
    TemporarySession() {
        path = std::filesystem::temp_directory_path() /
               (L"Heapglass-offline-ui-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetTickCount64()) + L".json");
        hg::save_session(offline_fixture(), path.wstring(), true);
    }
    ~TemporarySession() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};

void exercise_ui() {
    Context context;
    TemporarySession fixture;
    Harness h;
    h.app.controller.load(fixture.path.wstring());
    const auto deadline = GetTickCount64() + 5000;
    while (!h.app.controller.view()->offline && GetTickCount64() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    require(h.app.controller.view()->offline, "Controller did not load the offline fixture");
    for (int i = 0; i < 5; ++i)
        h.frame();
    h.contains("OFFLINE");
    h.contains("OFFLINE UI FIXTURE.exe");
    h.contains("3 / 3 records");
    h.app.show_allocation_sites();
    for (int i = 0; i < 3; ++i)
        h.frame();
    h.contains("Known live: 3 blocks | 1280 requested bytes | uncertain: 0 blocks");
    h.app.show_snapshot_growth();
    for (int i = 0; i < 3; ++i)
        h.frame();
    h.contains("Comparable retained observations");
    require(h.allocation_rows() ==
                std::vector<std::string>{"0000000000001000", "0000000000002000", "0000000000003000"},
            "Initial visible allocation order differs");

    h.click("Allocations & memory map", "##strings");
    ImGui::GetIO().AddInputCharactersUTF8("beta-current");
    h.frame();
    h.frame();
    h.contains("1 / 3 records");
    require(h.allocation_rows() == std::vector<std::string>{"0000000000002000"},
            "Typing a captured-string filter did not hide other allocations");
    h.key(ImGuiKey_A, true);
    h.key(ImGuiKey_Backspace);
    h.contains("3 / 3 records");
    require(h.allocation_rows().size() == 3, "Clearing the filter did not restore rows");

    h.click("Allocations & memory map", "Size");
    require(h.allocation_rows() ==
                std::vector<std::string>{"0000000000002000", "0000000000003000", "0000000000001000"},
            "Ascending Size header sort did not reorder visible rows");
    h.click("Allocations & memory map", "Size");
    require(h.allocation_rows() ==
                std::vector<std::string>{"0000000000001000", "0000000000003000", "0000000000002000"},
            "Descending Size header sort did not reorder visible rows");

    h.click("Allocations & memory map", "0000000000002000");
    h.contains("0000000000002000 | 128 bytes | LIVE");
    h.contains("Heap generation 1 | allocation 2");
    h.click("Investigation", "Allocation sites");
    h.contains("Known live: 3 blocks | 1280 requested bytes | uncertain: 0 blocks");
    h.click("Investigation", "0000000077770001 | bounded stack (2 frames)");
    h.contains("0000000000001000 | 768 bytes | LIVE");
    h.contains("saved address");
    h.contains("2 / 3 records");
    h.click("Allocations & memory map", "Clear site filter");
    h.contains("3 / 3 records");
    h.click("Investigation", "Snapshot growth");
    h.contains("Offline before -> Offline after | 2 observations");
    h.contains("Comparable retained observations");
    h.click("Investigation", "0000000077770001 | bounded stack (2 frames)");
    h.contains("Snapshot: Offline after | acquired QPC 200 .. 210");
    h.click("Investigation", "All strings");
    // The first allocation's first string is historical, so this must select capture zero.
    h.click("Investigation", "0000000000001000");
    h.contains("0000000000001000 | 768 bytes | LIVE");
    h.contains("QPC 100 .. 101");
    h.click("Investigation", "Compare snapshots");
    h.frame();
    std::string difference;
    for (const auto &[_, item] : items)
        if (in_window(item, "Investigation") && item.label.starts_with("0000000000001000 | "))
            difference = item.label;
    require(!difference.empty(), "Snapshot comparison did not render the changed allocation");
    h.click("Investigation", difference);
    h.contains("Snapshot: Offline after | acquired QPC 200 .. 210");
    h.contains("QPC 200 .. 201");

    h.click("Session & navigation", "Process memory map");
    h.click("Allocations & memory map", "0000000000001000");
    h.contains("Memory region");
    h.contains("0000000000001000 | 16384 bytes");
    h.click("Session & navigation", "All allocations");
    h.click("Allocations & memory map", "##strings");
    h.frame();
    const auto filter = h.find("Allocations & memory map", "##strings").id;
    const auto state = h.find("Allocations & memory map", "##state").id;
    require(GImGui->NavId == filter, "Filter click did not establish keyboard focus");
    h.key(ImGuiKey_Tab);
    require(GImGui->NavId == state, "Tab did not move focus from the text filter to the state selector");

    h.frame();
    auto *sidebar = ImGui::FindWindowByName("Session & navigation");
    auto *node = sidebar && sidebar->DockNode ? sidebar->DockNode->ParentNode : nullptr;
    require(node && node->IsSplitNode(), "Sidebar has no dock splitter");
    // Root sidebar divider is a real ImGui docking splitter, dragged with synthetic mouse events.
    auto *first = node->ChildNodes[0];
    auto *second = node->ChildNodes[1];
    require(node->SplitAxis == ImGuiAxis_X, "Sidebar divider is not vertical");
    const float before = sidebar->Size.x;
    const ImVec2 point((first->Pos.x + first->Size.x + second->Pos.x) * .5f,
                       node->Pos.y + node->Size.y * .5f);
    auto &io = ImGui::GetIO();
    io.AddMousePosEvent(point.x, point.y);
    h.frame();
    h.frame();
    io.AddMouseButtonEvent(0, true);
    h.frame();
    io.AddMousePosEvent(point.x + 60, point.y);
    h.frame();
    h.frame();
    io.AddMouseButtonEvent(0, false);
    h.frame();
    h.frame();
    const float after = ImGui::FindWindowByName("Session & navigation")->Size.x;
    require(after > before + 40 && after < before + 80,
            "Dragging the dock splitter did not resize the visible sidebar by about 60 pixels");

    auto unsampled = offline_fixture();
    auto &allocation = unsampled.records.at(1);
    allocation.captures.clear();
    allocation.stack_count = 1;
    allocation.stack_status = 1;
    allocation.stack[0] = 0x12345678;
    unsampled.snapshots[0].records[0].size = 256;
    unsampled.snapshots[1].records[0].size = 512;
    auto third = unsampled.snapshots.back();
    third.name = "Offline final";
    third.begin = 300;
    third.end = 310;
    third.records[0].size = 768;
    unsampled.snapshots.push_back(std::move(third));
    hg::save_session(unsampled, fixture.path.wstring(), true);
    const auto previous_view = h.app.controller.view();
    h.app.controller.load(fixture.path.wstring());
    const auto load_deadline = GetTickCount64() + 5000;
    while (h.app.controller.view() == previous_view && GetTickCount64() < load_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    require(h.app.controller.view() != previous_view, "Controller did not reload the unsampled fixture");
    for (int i = 0; i < 5; ++i)
        h.frame();
    h.click("Allocations & memory map", "0000000000001000");
    h.contains("No captured bytes");
    h.click("Inspector", "Stack");
    h.contains("0000000012345678");
    h.contains("saved address");
    h.click("Inspector", "History");
    h.contains("QPC 10 |");
    h.click("Investigation", "Snapshot growth");
    h.contains("Offline before -> Offline final | 3 observations");
    h.contains("Repeated growth: 1 site");
    h.app.controller.growth_csv(fixture.path.wstring() + L".growth.csv");
    const auto export_deadline = GetTickCount64() + 5000;
    while (h.app.controller.view()->status != "Snapshot growth exported" &&
           GetTickCount64() < export_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    require(h.app.controller.view()->status == "Snapshot growth exported", "Growth CSV job did not complete");
    std::ifstream growth_file(fixture.path.wstring() + L".growth.csv");
    const std::string growth_text((std::istreambuf_iterator<char>(growth_file)), {});
    growth_file.close();
    std::filesystem::remove(fixture.path.wstring() + L".growth.csv");
    require(growth_text.find("before_live_count,after_live_count") != std::string::npos,
            "Growth CSV did not contain snapshot totals");
    h.app.controller.sites_csv(fixture.path.wstring() + L".sites.csv");
    const auto sites_deadline = GetTickCount64() + 5000;
    while (h.app.controller.view()->status != "Allocation sites exported" &&
           GetTickCount64() < sites_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    require(h.app.controller.view()->status == "Allocation sites exported", "Site CSV job did not complete");
    std::ifstream sites_file(fixture.path.wstring() + L".sites.csv");
    const std::string sites_text((std::istreambuf_iterator<char>(sites_file)), {});
    sites_file.close();
    std::filesystem::remove(fixture.path.wstring() + L".sites.csv");
    require(sites_text.find("live_requested_bytes") != std::string::npos &&
                sites_text.find("caller only") != std::string::npos,
            "Sites CSV did not preserve caller-only grouping");
}
} // namespace

int main() {
    try {
        exercise_ui();
        std::cout
            << "PASS: offline real UI input, filtering, sorting, Inspector navigation, Tab and docking\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "UI validation failed: " << error.what() << '\n';
        return 1;
    }
}
