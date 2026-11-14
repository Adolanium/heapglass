#include "ui.hpp"
#include "desktop.hpp"
#include <algorithm>
#include <cfloat>
#include <commdlg.h>
#include <filesystem>
#include <imgui_internal.h>
#include <iomanip>
#include <sstream>

namespace hg {
static std::string hex_address(uint64_t n) {
    char b[32]{};
    sprintf_s(b, "%016llX", n);
    return b;
}
static std::string basename(const std::string &s) {
    return narrow(std::filesystem::path(widen(s)).filename().wstring());
}
static void muted(const char *s) {
    ImGui::TextColored(ImVec4(.60f, .68f, .73f, 1), "%s", s);
}
static void heading(const char *s) {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(.44f, .84f, .80f, 1), "%s", s);
    ImGui::Separator();
}
static void empty(const char *title, const char *help) {
    ImGui::Spacing();
    ImGui::TextUnformatted(title);
    ImGui::TextWrapped("%s", help);
}
static const Capture *latest(const Allocation &a) {
    return a.captures.empty() ? nullptr : &a.captures.back();
}
static size_t acquired(const Snapshot &s) {
    size_t n = 0;
    for (const auto &a : s.records)
        if (!a.captures.empty() && a.captures.back().begin >= s.begin && a.captures.back().end <= s.end)
            ++n;
    return n;
}
static double age(const Allocation &a, const Model &m) {
    return a.birth_known && a.died && m.frequency ? double(a.died - a.born) / double(m.frequency) : -1;
}
std::wstring UI::choose_file(bool save, const wchar_t *filter, const wchar_t *extension) {
    try {
        return hg::file_dialog(save, filter, extension);
    } catch (const std::exception &e) {
        ui_error = e.what();
        return {};
    }
}
void UI::show_memory_map() {
    regions_view = true;
    selected = 0;
    for (const auto &r : shown->regions)
        if (r.type == MEM_PRIVATE && (r.protection & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) {
            region_selected = r.base;
            break;
        }
}
void UI::show_allocation_stack() {
    region_selected = 0;
    regions_view = false;
    select_stack_tab = true;
    for (const auto &[id, a] : shown->records)
        if (a.life == Life::live && a.stack_count) {
            selected = id;
            break;
        }
}
void UI::auto_target() {
    controller.launch(executable_directory() + L"\\HeapglassTarget.exe", L"--duration 180");
}
void UI::show_allocation_sites() {
    investigation_view = InvestigationView::sites;
}
void UI::show_snapshot_growth() {
    investigation_view = InvestigationView::growth;
}
void UI::draw() {
    if (!display_paused || !shown) {
        auto next = controller.view();
        if (shown && shown->session != next->session) {
            site_filter.reset();
            selected = region_selected = 0;
            inspecting_snapshot = capture_index = -1;
        }
        shown = std::move(next);
    }
    menu();
    const auto *viewport = ImGui::GetMainViewport();
    ImGuiID dock = ImGui::DockSpaceOverViewport(0, viewport, ImGuiDockNodeFlags_PassthruCentralNode);
    if (!ImGui::DockBuilderGetNode(dock) || !ImGui::DockBuilderGetNode(dock)->IsSplitNode()) {
        ImGui::DockBuilderRemoveNode(dock);
        ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dock, viewport->WorkSize);
        ImGuiID center = dock,
                left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, .16f, nullptr, &center);
        ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, .38f, nullptr, &center);
        ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, .32f, nullptr, &center);
        ImGui::DockBuilderDockWindow("Session & navigation", left);
        ImGui::DockBuilderDockWindow("Allocations & memory map", center);
        ImGui::DockBuilderDockWindow("Inspector", right);
        ImGui::DockBuilderDockWindow("Investigation", bottom);
        ImGui::DockBuilderFinish(dock);
    }
    if (!selected && !region_selected && shown->recording)
        for (const auto &[id, a] : shown->records) {
            if (auto c = latest(a);
                c && !c->strings().empty() && c->strings()[0].value.starts_with("Heapglass")) {
                selected = id;
                break;
            }
        }
    sidebar();
    allocation_table();
    details();
    bottom();
    dialogs();
}
void UI::menu() {
    if (!ImGui::BeginMainMenuBar())
        return;
    ImGui::TextColored(ImVec4(.44f, .84f, .80f, 1), "Heapglass");
    ImGui::Separator();
    if (ImGui::BeginMenu("Session")) {
        if (ImGui::MenuItem("Attach to process..."))
            show_attach = true;
        if (ImGui::MenuItem("Launch executable..."))
            show_launch = true;
        if (ImGui::MenuItem("Launch test target"))
            auto_target();
        ImGui::Separator();
        if (ImGui::MenuItem("Open saved session...")) {
            auto p = choose_file(false, L"Heapglass session\0*.json\0All files\0*.*\0", L"json");
            if (!p.empty())
                controller.load(p);
        }
        ImGui::MenuItem("Include sampled bytes in JSON", nullptr, &include_bytes);
        if (ImGui::MenuItem("Save session / export JSON...")) {
            auto p = choose_file(true, L"Heapglass session\0*.json\0", L"json");
            if (!p.empty())
                controller.save(p, include_bytes);
        }
        if (ImGui::MenuItem("Export allocation CSV...")) {
            auto p = choose_file(true, L"CSV\0*.csv\0", L"csv");
            if (!p.empty())
                controller.csv(p);
        }
        if (ImGui::MenuItem("Export allocation sites CSV...")) {
            auto p = choose_file(true, L"CSV\0*.csv\0", L"csv");
            if (!p.empty())
                controller.sites_csv(p);
        }
        if (ImGui::MenuItem("Export snapshot growth CSV...", nullptr, false, shown->snapshots.size() >= 2)) {
            auto p = choose_file(true, L"CSV\0*.csv\0", L"csv");
            if (!p.empty())
                controller.growth_csv(p);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Disconnect", nullptr, false, shown->connected))
            controller.disconnect();
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem(shown->recording ? "Stop recording" : "Start recording", nullptr, false,
                        shown->connected))
        controller.record(!shown->recording);
    if (ImGui::MenuItem(display_paused ? "Resume display" : "Pause display"))
        display_paused = !display_paused;
    if (ImGui::MenuItem("Sample now", nullptr, false, shown->recording))
        controller.sample_now();
    if (ImGui::MenuItem("Limits & rules"))
        show_limits = true;
    ImGui::Separator();
    ImGui::TextUnformatted(display_paused     ? "DISPLAY PAUSED | collection continues"
                           : shown->offline   ? "OFFLINE"
                           : shown->connected ? "LIVE x64"
                                              : "DISCONNECTED");
    ImGui::Separator();
    ImGui::Text("%.0f fps", ImGui::GetIO().Framerate);
    ImGui::EndMainMenuBar();
}
void UI::sidebar() {
    ImGui::Begin("Session & navigation");
    if (!shown->session) {
        empty("Select a process", "Attach to a native x64 process or launch an executable. The test target "
                                  "provides controlled workloads.");
        if (ImGui::Button("Attach to process", ImVec2(-1, 0)))
            show_attach = true;
        if (ImGui::Button("Launch executable", ImVec2(-1, 0)))
            show_launch = true;
        if (ImGui::Button("Launch test target", ImVec2(-1, 0)))
            auto_target();
    } else {
        ImGui::TextWrapped("%s", basename(shown->process).c_str());
        ImGui::Text("PID %lu | x64", shown->pid);
        muted(shown->start_time.c_str());
        ImGui::TextWrapped("%s", shown->recording ? "Recording"
                                 : shown->offline ? "Saved observations"
                                                  : "Stopped / disconnected");
        heading("Coverage");
        ImGui::TextWrapped("Allocations observed since instrumentation began. No pre-attach heap baseline.");
        ImGui::Text("Events: %llu", shown->observed);
        ImGui::Text("Lost: %llu", shown->dropped);
        ImGui::Text("Evicted records: %llu", shown->evicted);
        ImGui::Text("Collector losses: %llu", shown->controller_dropped);
        ImGui::Text("Queued target: %llu", shown->transport_pending);
        ImGui::Text("Queued analysis: %llu", shown->analysis_pending);
        ImGui::TextColored(shown->incomplete ? ImVec4(1, .74f, .37f, 1) : ImVec4(.44f, .84f, .8f, 1), "%s",
                           shown->incomplete ? "INCOMPLETE HISTORY" : "No reported event loss");
        ImGui::Text("Sample: %u ms / %zu bytes", shown->limits.sample_ms, shown->limits.per_allocation);
        ImGui::Text("Retained: %.1f / %.0f MiB", double(shown->retained_bytes) / 1048576.,
                    double(shown->limits.per_session) / 1048576.);
        ImGui::Text("Sample evictions: %llu", shown->evicted_captures);
        ImGui::Text("Skipped samples: %llu", shown->skipped_captures);
        muted(shown->limits.retention == Retention::rolling ? "Rolling sample retention"
                                                            : "Stop at byte capacity");
        if (shown->sampling_budget_exhausted)
            ImGui::TextWrapped(
                "SAMPLING STOPPED: byte budget exhausted. Increase the limit or choose rolling retention.");
        ImGui::Text("Stack capture: %u / %u frames", shown->active_stack_frames, max_stack_frames);
        if (shown->stack_state == 2)
            ImGui::TextWrapped("Stack capture unavailable: native unwinder validation failed.");
        heading("Snapshots");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##snapshot-name", snapshot_name, sizeof(snapshot_name));
        if (ImGui::Button("Capture named snapshot", ImVec2(-1, 0)))
            controller.take_snapshot(snapshot_name);
        for (size_t i = 0; i < shown->snapshots.size(); ++i) {
            ImGui::PushID(int(i));
            if (ImGui::Selectable(shown->snapshots[i].name.c_str(), snap_b == int(i)))
                snap_b = int(i);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%zu / %zu records sampled in acquisition interval; other captures retain "
                                  "their original times",
                                  acquired(shown->snapshots[i]), shown->snapshots[i].records.size());
            ImGui::PopID();
        }
        heading("Navigate");
        if (ImGui::Selectable("All allocations", !regions_view && !heap_filter)) {
            regions_view = false;
            heap_filter = 0;
        }
        if (ImGui::Selectable("Process memory map", regions_view))
            regions_view = true;
        std::map<uint64_t, size_t> heaps;
        for (const auto &[_, a] : shown->records)
            ++heaps[a.heap];
        for (const auto &[h, n] : heaps) {
            std::string label = hex_address(h) + " (" + std::to_string(n) + ")";
            if (ImGui::Selectable(label.c_str(), !regions_view && heap_filter == h)) {
                regions_view = false;
                heap_filter = h;
            }
        }
    }
    heading("Status");
    ImGui::TextWrapped("%s", shown->status.c_str());
    if (!ui_error.empty())
        ImGui::TextWrapped("%s", ui_error.c_str());
    ImGui::End();
}
void UI::allocation_table() {
    ImGui::Begin("Allocations & memory map");
    if (regions_view) {
        ImGui::Text("%zu committed regions | protections sampled by polling", shown->regions.size());
        if (ImGui::BeginTable("regions", 7,
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_Resizable)) {
            for (auto title :
                 {"Base", "Size", "Type", "Protection", "Allocation base", "Loaded image", "Mapped file"})
                ImGui::TableSetupColumn(title);
            ImGui::TableSetupScrollFreeze(1, 1);
            ImGui::TableHeadersRow();
            ImGuiListClipper clip;
            clip.Begin(int(shown->regions.size()));
            while (clip.Step())
                for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                    const auto &r = shown->regions[i];
                    ImGui::PushID(i);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    if (ImGui::Selectable(hex_address(r.base).c_str(), region_selected == r.base,
                                          ImGuiSelectableFlags_SpanAllColumns)) {
                        region_selected = r.base;
                        selected = 0;
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("%llu", r.size);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(r.type == MEM_PRIVATE ? "PRIVATE"
                                           : r.type == MEM_IMAGE ? "IMAGE"
                                                                 : "MAPPED");
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(protection_name(r.protection).c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(hex_address(r.allocation_base).c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(r.module.empty() ? "none known" : r.module.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(r.mapped_file.c_str());
                    ImGui::PopID();
                }
            ImGui::EndTable();
        }
        ImGui::End();
        return;
    }
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##strings", "Filter captured strings...", search, sizeof(search));
    ImGui::SameLine();
    const char *states[] = {"All states", "LIVE", "FREED", "UNCERTAIN"};
    ImGui::SetNextItemWidth(130);
    ImGui::Combo("##state", &state_filter, states, 4);
    if (site_filter) {
        ImGui::Text("Site: %s", site_label(*site_filter).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear site filter"))
            site_filter.reset();
    }
    if (ImGui::CollapsingHeader("Address, size, thread, lifetime, entropy & findings")) {
        ImGui::SetNextItemWidth(155);
        ImGui::InputScalar("Address min", ImGuiDataType_U64, &min_address, nullptr, nullptr, "%016llX",
                           ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(155);
        ImGui::InputScalar("max##address", ImGuiDataType_U64, &max_address, nullptr, nullptr, "%016llX",
                           ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SetNextItemWidth(120);
        ImGui::InputScalar("Size min", ImGuiDataType_U64, &min_size);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::InputScalar("max##size", ImGuiDataType_U64, &max_size);
        ImGui::SetNextItemWidth(100);
        ImGui::InputInt("Thread (0=all)", &thread_filter);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::InputInt("Min known lifetime ms", &min_lifetime);
        ImGui::SetNextItemWidth(100);
        ImGui::SliderFloat("Min entropy", &entropy_filter, 0, 8, "%.1f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        ImGui::InputText("Protection", permission_filter, sizeof(permission_filter));
        ImGui::SetNextItemWidth(210);
        ImGui::InputText("Finding text", finding_filter, sizeof(finding_filter));
    }
    Filter current{heap_filter,       min_address,    max_address,  min_size,       max_size,
                   state_filter,      thread_filter,  min_lifetime, entropy_filter, search,
                   permission_filter, finding_filter, site_filter};
    auto &rows = row_cache;
    if (row_owner != shown || current != row_filter) {
        row_owner = shown;
        row_filter = current;
        rows.clear();
        sorted_column = -1;
        rows.reserve(shown->records.size());
        for (const auto &[_, a] : shown->records) {
            if (a.address < min_address || a.address > max_address || a.size < min_size ||
                a.size > max_size || (heap_filter && a.heap != heap_filter) ||
                (thread_filter > 0 && a.thread != uint32_t(thread_filter)) ||
                (state_filter && int(a.life) != state_filter - 1))
                continue;
            if (site_filter && allocation_site(a) != *site_filter)
                continue;
            const auto *c = latest(a);
            if (entropy_filter > 0 && (!c || c->entropy < entropy_filter))
                continue;
            if (min_lifetime > 0 && (age(a, *shown) < double(min_lifetime) / 1000.))
                continue;
            if (*permission_filter &&
                (!c || protection_name(c->protection).find(permission_filter) == std::string::npos))
                continue;
            if (*search) {
                bool match = false;
                if (c)
                    for (const auto &s : c->strings())
                        if (s.value.find(search) != std::string::npos) {
                            match = true;
                            break;
                        }
                if (!match)
                    continue;
            }
            if (*finding_filter) {
                bool match = false;
                for (const auto &f : shown->findings)
                    if (f.generation == a.id.generation &&
                        f.title.find(finding_filter) != std::string::npos) {
                        match = true;
                        break;
                    }
                if (!match)
                    continue;
            }
            rows.push_back(&a);
        }
    }
    ImGui::Text("%zu / %zu records | event metadata + sampled content", rows.size(), shown->records.size());
    if (rows.empty())
        muted(shown->session ? "No allocations match the filters or current retention window."
                             : "Live allocation events will appear after instrumentation.");
    if (ImGui::BeginTable("allocations", 11,
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable |
                              ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable)) {
        const char *names[] = {"Address",  "Size",   "Heap",    "State",  "Entropy",   "Protection",
                               "Lifetime", "Caller", "Preview", "Thread", "Generation"};
        float widths[] = {145, 75, 145, 95, 65, 75, 85, 145, 250, 65, 85};
        for (int i = 0; i < 11; ++i)
            ImGui::TableSetupColumn(names[i], ImGuiTableColumnFlags_WidthFixed, widths[i], i);
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableHeadersRow();
        auto *spec = ImGui::TableGetSortSpecs();
        int column = spec && spec->SpecsCount ? spec->Specs[0].ColumnUserID : 0;
        bool ascending =
            !spec || !spec->SpecsCount || spec->Specs[0].SortDirection != ImGuiSortDirection_Descending;
        if (sorted_column != column || sorted_ascending != ascending) {
            std::sort(rows.begin(), rows.end(), [&](const Allocation *a, const Allocation *b) {
                auto cmp = [](const auto &x, const auto &y) { return x < y ? -1 : x > y ? 1 : 0; };
                auto ca = latest(*a), cb = latest(*b);
                int order = 0;
                switch (column) {
                case 0:
                    order = cmp(a->address, b->address);
                    break;
                case 1:
                    order = cmp(a->size, b->size);
                    break;
                case 2:
                    order = cmp(a->heap, b->heap);
                    break;
                case 3:
                    order = cmp(a->life, b->life);
                    break;
                case 4:
                    order = cmp(ca ? ca->entropy : -1., cb ? cb->entropy : -1.);
                    break;
                case 5:
                    order = cmp(ca ? protection_name(ca->protection) : "unknown",
                                cb ? protection_name(cb->protection) : "unknown");
                    break;
                case 6:
                    order = cmp(age(*a, *shown), age(*b, *shown));
                    break;
                case 7:
                    order = cmp(a->caller, b->caller);
                    break;
                case 8:
                    order = cmp(ca && !ca->strings().empty() ? ca->strings()[0].value : "",
                                cb && !cb->strings().empty() ? cb->strings()[0].value : "");
                    break;
                case 9:
                    order = cmp(a->thread, b->thread);
                    break;
                case 10:
                    order = cmp(a->id.generation, b->id.generation);
                    break;
                }
                if (!order)
                    order = cmp(a->id.generation, b->id.generation);
                return ascending ? order < 0 : order > 0;
            });
            sorted_column = column;
            sorted_ascending = ascending;
        }
        if (spec)
            spec->SpecsDirty = false;
        ImGuiListClipper clip;
        clip.Begin(int(rows.size()));
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                const auto &a = *rows[i];
                const auto *c = latest(a);
                ImGui::PushID(int(a.id.generation));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Selectable(hex_address(a.address).c_str(), selected == a.id.generation,
                                      ImGuiSelectableFlags_SpanAllColumns)) {
                    select_allocation(a.id.generation);
                    controller.watch(selected, true);
                }
                ImGui::TableNextColumn();
                ImGui::Text("%llu", a.size);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(hex_address(a.heap).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(life_name(a.life));
                ImGui::TableNextColumn();
                if (c)
                    ImGui::Text("%.2f%s", c->entropy, c->attributed ? "" : " ?");
                else
                    muted("not sampled");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(c ? protection_name(c->protection).c_str() : "unknown");
                ImGui::TableNextColumn();
                if (age(a, *shown) >= 0)
                    ImGui::Text("%.3f s", age(a, *shown));
                else
                    muted("unknown");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(hex_address(a.caller).c_str());
                ImGui::TableNextColumn();
                if (c && !c->strings().empty())
                    ImGui::TextUnformatted(c->strings()[0].value.c_str());
                else
                    muted(c ? "binary / no strings" : "not sampled");
                ImGui::TableNextColumn();
                ImGui::Text("%u", a.thread);
                ImGui::TableNextColumn();
                ImGui::Text("%llu", a.id.generation);
                ImGui::PopID();
            }
        ImGui::EndTable();
    }
    ImGui::End();
}
void UI::select_allocation(uint64_t generation, int capture) {
    selected = generation;
    capture_index = capture;
    region_selected = 0;
    regions_view = false;
    inspecting_snapshot = -1;
}
void UI::navigate(uint64_t address, uint64_t heap, uint64_t at) {
    uint64_t best = 0;
    for (const auto &[id, a] : shown->records)
        if ((!heap || a.heap == heap) &&
            (a.address == address || std::any_of(a.history.begin(), a.history.end(),
                                                 [&](const SizePoint &h) { return h.address == address; })) &&
            (!at || a.born <= at) && (!best || id > best))
            best = id;
    if (best) {
        select_allocation(best);
    } else {
        region_selected = address;
        regions_view = true;
        selected = 0;
    }
}
void UI::details() {
    ImGui::Begin("Inspector");
    if (region_selected)
        for (const auto &r : shown->regions)
            if (r.base == region_selected) {
                heading("Memory region");
                ImGui::Text("%s | %llu bytes", hex_address(r.base).c_str(), r.size);
                ImGui::Text("Protection: %s", protection_name(r.protection).c_str());
                ImGui::Text("Allocation base: %s", hex_address(r.allocation_base).c_str());
                ImGui::Text("Memory type: %s", r.type == MEM_PRIVATE ? "PRIVATE"
                                               : r.type == MEM_IMAGE ? "IMAGE"
                                                                     : "MAPPED");
                ImGui::TextWrapped("Loaded image: %s", r.module.empty() ? "none known" : r.module.c_str());
                ImGui::TextWrapped("Mapped file: %s",
                                   r.mapped_file.empty() ? "none available" : r.mapped_file.c_str());
                ImGui::Text("Observed QPC %llu", r.observed);
                muted("Protections are polled. No virtual-memory API hooks.");
                for (const auto &f : shown->findings)
                    if (f.address == r.base) {
                        heading(f.title.c_str());
                        ImGui::TextWrapped("%s", f.evidence.c_str());
                        ImGui::TextWrapped("%s | %s", f.rule.c_str(), f.confidence.c_str());
                        ImGui::Text("QPC %llu .. %llu", f.begin, f.end);
                    }
                ImGui::End();
                return;
            }
    const Allocation *inspected = nullptr;
    if (inspecting_snapshot >= 0 && inspecting_snapshot < int(shown->snapshots.size())) {
        const auto &snapshot = shown->snapshots[inspecting_snapshot];
        for (const auto &a : snapshot.records)
            if (a.id.generation == selected) {
                inspected = &a;
                break;
            }
        ImGui::TextWrapped("Snapshot: %s | acquired QPC %llu .. %llu", snapshot.name.c_str(), snapshot.begin,
                           snapshot.end);
    } else if (auto it = shown->records.find(selected); it != shown->records.end())
        inspected = &it->second;
    if (!inspected) {
        empty("Inspect an allocation", "Select a row, event, string or finding. Allocation metadata and "
                                       "sampled bytes have separate timestamps.");
        ImGui::End();
        return;
    }
    const auto &a = *inspected;
    heading("Allocation identity");
    ImGui::Text("%s | %llu bytes | %s", hex_address(a.address).c_str(), a.size, life_name(a.life));
    ImGui::Text("Session %016llX", a.id.session);
    ImGui::Text("Heap generation %llu | allocation %llu", a.id.heap_generation, a.id.generation);
    if (a.parent)
        ImGui::Text("Reallocated from generation %llu", a.parent);
    ImGui::Text("Thread %u | heap %s", a.thread, hex_address(a.heap).c_str());
    ImGui::TextWrapped("Caller %s | %s", hex_address(a.caller).c_str(), controller.symbol(a.caller).c_str());
    if (age(a, *shown) >= 0)
        ImGui::Text("Lifetime %.6f s", age(a, *shown));
    else
        muted(a.birth_known ? "Lifetime unknown; observed start boundary only"
                            : "Lifetime unknown; allocation predates observation");
    bool watched = shown->watched.contains(selected);
    if (ImGui::Checkbox("Prioritize sampling", &watched))
        controller.watch(selected, watched);
    const Capture *selected_capture = nullptr;
    if (a.captures.empty()) {
        empty("No captured bytes", "Sampling is bounded. Freed or uncertain records are never reread. Choose "
                                   "Sample now while recording to request another scan.");
    } else {
        int index = capture_index < 0 ? int(a.captures.size()) - 1
                                      : std::min(capture_index, int(a.captures.size()) - 1);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderInt("##capture", &index, 0, int(a.captures.size()) - 1, "Capture %d"))
            capture_index = index;
        const auto &c = a.captures[index];
        selected_capture = &c;
        heading("Captured content");
        ImGui::Text("%zu bytes | entropy %.3f bits/byte", c.bytes().size(), c.entropy);
        ImGui::Text("QPC %llu .. %llu", c.begin, c.end);
        ImGui::Text("FNV-1a64 %016llX", c.hash);
        ImGui::TextWrapped("%s%s%s", c.attributed ? "Attributed observed range" : "UNSAFE attribution",
                           c.truncated ? " | truncated" : "",
                           a.life == Life::freed ? " | historical bytes, not reread" : "");
        if (!c.note.empty())
            ImGui::TextWrapped("%s", c.note.c_str());
        muted("Acquisition is non-atomic. Brief writes can be missed.");
        if (c.bytes().size() < 256)
            muted("Small sample: insufficient entropy evidence for findings.");
    }
    if (ImGui::BeginTabBar("inspect-tabs")) {
        if (selected_capture && ImGui::BeginTabItem("Hex + ASCII")) {
            const auto &c = *selected_capture;
            if (ImGui::Button("Copy captured hex")) {
                std::string s;
                for (auto b : c.bytes()) {
                    char x[4]{};
                    sprintf_s(x, "%02X ", b);
                    s += x;
                }
                ImGui::SetClipboardText(s.c_str());
            }
            ImGui::BeginChild("hex", ImVec2(0, 250), ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_HorizontalScrollbar);
            if (ImGui::GetIO().Fonts->Fonts.Size > 1)
                ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[1]);
            std::string dump;
            for (size_t offset = 0; offset < c.bytes().size(); offset += 16) {
                char row[160]{};
                sprintf_s(row, "%08llX  ", c.offset + offset);
                dump += row;
                std::string ascii;
                for (size_t j = 0; j < 16; ++j) {
                    if (offset + j < c.bytes().size()) {
                        uint8_t b = c.bytes()[offset + j];
                        sprintf_s(row, "%02X ", b);
                        dump += row;
                        ascii += (b >= 32 && b < 127) ? char(b) : '.';
                    } else
                        dump += "   ";
                }
                dump += " | " + ascii + "\n";
            }
            // Read-only input permits text selection and native Ctrl+C over offsets, hex and ASCII.
            std::vector<char> buffer(dump.begin(), dump.end());
            buffer.push_back(0);
            ImGui::InputTextMultiline("##hex-text", buffer.data(), buffer.size(),
                                      ImVec2(700, ImGui::GetContentRegionAvail().y),
                                      ImGuiInputTextFlags_ReadOnly);
            if (ImGui::GetIO().Fonts->Fonts.Size > 1)
                ImGui::PopFont();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (selected_capture && ImGui::BeginTabItem("Strings")) {
            const auto &c = *selected_capture;
            for (size_t i = 0; i < c.strings().size(); ++i) {
                const auto &s = c.strings()[i];
                ImGui::PushID(int(i));
                ImGui::Text("+%08llX  %s", s.offset, s.encoding.c_str());
                if (ImGui::Selectable(s.value.c_str()))
                    ImGui::SetClipboardText(s.value.c_str());
                ImGui::PopID();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Stack", nullptr, select_stack_tab ? ImGuiTabItemFlags_SetSelected : 0)) {
            select_stack_tab = false;
            if (!a.stack_count)
                ImGui::TextWrapped(
                    "%s", a.stack_status == 2
                              ? "Stack unavailable; native capture was rejected or failed"
                              : "No stack was captured. Enable up to eight frames under Limits & rules.");
            for (uint32_t n = 0; n < a.stack_count; ++n)
                ImGui::TextWrapped("%u  %s  %s", n, hex_address(a.stack[n]).c_str(),
                                   shown->offline ? "saved address" : controller.symbol(a.stack[n]).c_str());
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("History")) {
            for (const auto &h : a.history)
                ImGui::Text("QPC %llu | %s | %llu bytes", h.ticks, hex_address(h.address).c_str(), h.size);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Findings")) {
            for (const auto &f : shown->findings)
                if (f.generation == selected) {
                    heading(f.title.c_str());
                    ImGui::TextWrapped("%s", f.evidence.c_str());
                    ImGui::TextWrapped("%s | confidence %s", f.rule.c_str(), f.confidence.c_str());
                    ImGui::Text("QPC %llu .. %llu", f.begin, f.end);
                }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    heading("Observed protections");
    for (const auto &r : shown->regions)
        if (a.address >= r.base && a.address - r.base < r.size) {
            ImGui::Text("%s | %s", protection_name(r.protection).c_str(),
                        r.type == MEM_PRIVATE ? "PRIVATE"
                        : r.type == MEM_IMAGE ? "IMAGE"
                                              : "MAPPED");
            break;
        }
    ImGui::End();
}
void UI::bottom() {
    ImGui::Begin("Investigation");
    if (ImGui::BeginTabBar("investigation-tabs")) {
        if (ImGui::BeginTabItem("Events")) {
            ImGui::Text("%zu retained events | QPC frequency %llu", shown->events.size(), shown->frequency);
            if (ImGui::BeginTable("events", 5,
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_Resizable)) {
                for (auto s : {"Operation", "Address / prior", "Requested", "Thread", "QPC / order"})
                    ImGui::TableSetupColumn(s);
                ImGui::TableHeadersRow();
                ImGuiListClipper clip;
                clip.Begin(int(shown->events.size()));
                while (clip.Step())
                    for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
                        const auto &e = shown->events[shown->events.size() - 1 - row];
                        ImGui::PushID(row);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (ImGui::Selectable(op_name(e.op), false, ImGuiSelectableFlags_SpanAllColumns))
                            navigate(e.op == Op::free ? e.previous : e.result, e.heap, e.ticks);
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(hex_address(e.op == Op::free ? e.previous : e.result).c_str());
                        ImGui::TableNextColumn();
                        ImGui::Text("%llu%s", e.requested,
                                    (e.reserved & 1)   ? " exception"
                                    : (e.reserved & 2) ? " stack gap"
                                                       : "");
                        ImGui::TableNextColumn();
                        ImGui::Text("%u", e.thread);
                        ImGui::TableNextColumn();
                        ImGui::Text("%llu / %llu..%llu", e.ticks, e.begin, e.end);
                        ImGui::PopID();
                    }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("All strings")) {
            struct Row {
                const Allocation *a;
                const Capture *c;
                const Text *s;
            };
            std::vector<Row> rows;
            for (const auto &[_, a] : shown->records)
                for (const auto &c : a.captures)
                    for (const auto &s : c.strings())
                        if (!*search || s.value.find(search) != std::string::npos)
                            rows.push_back({&a, &c, &s});
            if (ImGui::BeginTable("allstrings", 4,
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_Resizable)) {
                for (auto s : {"Location", "Encoding", "Captured QPC", "Text"})
                    ImGui::TableSetupColumn(s);
                ImGui::TableHeadersRow();
                ImGuiListClipper clip;
                clip.Begin(int(rows.size()));
                while (clip.Step())
                    for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                        const auto &r = rows[i];
                        ImGui::PushID(i);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (ImGui::Selectable(hex_address(r.a->address + r.c->offset + r.s->offset).c_str(),
                                              false, ImGuiSelectableFlags_SpanAllColumns)) {
                            select_allocation(r.a->id.generation);
                            int n = 0;
                            for (const auto &c : r.a->captures) {
                                if (&c == r.c) {
                                    capture_index = n;
                                    break;
                                }
                                ++n;
                            }
                        }
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(r.s->encoding.c_str());
                        ImGui::TableNextColumn();
                        ImGui::Text("%llu%s", r.c->begin, r.c->attributed ? "" : " unsafe");
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(r.s->value.c_str());
                        ImGui::PopID();
                    }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Findings")) {
            ImGui::TextWrapped(
                "Observations and hypotheses only. Confidence is qualitative, not a probability.");
            for (size_t i = 0; i < shown->findings.size(); ++i) {
                const auto &f = shown->findings[i];
                ImGui::PushID(int(i));
                if (ImGui::Selectable((f.title + " | " + hex_address(f.address)).c_str())) {
                    if (f.generation) {
                        select_allocation(f.generation);
                    } else {
                        region_selected = f.address;
                        selected = 0;
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(500);
                    ImGui::TextUnformatted(f.evidence.c_str());
                    ImGui::Text("%s | %s", f.rule.c_str(), f.confidence.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }
                ImGui::PopID();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Compare snapshots")) {
            if (shown->snapshots.size() < 2)
                empty("Capture two named snapshots",
                      "Comparisons use allocation generations and like-for-like retained byte ranges. "
                      "Missing data is uncertain.");
            else {
                snap_a = std::clamp(snap_a, 0, int(shown->snapshots.size()) - 1);
                snap_b = std::clamp(snap_b, 0, int(shown->snapshots.size()) - 1);
                ImGui::SetNextItemWidth(120);
                ImGui::SliderInt("A", &snap_a, 0, int(shown->snapshots.size()) - 1);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(120);
                ImGui::SliderInt("B", &snap_b, 0, int(shown->snapshots.size()) - 1);
                const auto &a = shown->snapshots[snap_a];
                const auto &b = shown->snapshots[snap_b];
                ImGui::Text("%s [%llu..%llu] -> %s [%llu..%llu]", a.name.c_str(), a.begin, a.end,
                            b.name.c_str(), b.begin, b.end);
                ImGui::Text("Acquired samples: A %zu/%zu | B %zu/%zu", acquired(a), a.records.size(),
                            acquired(b), b.records.size());
                auto diff = compare(a, b);
                ImGuiListClipper clip;
                clip.Begin(int(diff.size()));
                while (clip.Step())
                    for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                        const auto &d = diff[i];
                        std::string label = hex_address(d.address) + " | " + change_name(d.change) + " | " +
                                            std::to_string(d.byte_offsets.size()) +
                                            " differing captured bytes";
                        if (ImGui::Selectable(label.c_str())) {
                            select_allocation(d.id.generation);
                            inspecting_snapshot =
                                std::any_of(b.records.begin(), b.records.end(),
                                            [&](const Allocation &r) { return r.id == d.id; })
                                    ? snap_b
                                    : snap_a;
                        }
                        if (ImGui::IsItemHovered() && !d.byte_offsets.empty()) {
                            ImGui::BeginTooltip();
                            ImGui::TextUnformatted("Changed offsets (up to 64):");
                            for (size_t n = 0; n < std::min<size_t>(64, d.byte_offsets.size()); ++n) {
                                ImGui::Text("+%llX", d.byte_offsets[n]);
                                if (n % 8 != 7)
                                    ImGui::SameLine();
                            }
                            ImGui::EndTooltip();
                        }
                    }
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Allocation sites", nullptr,
                                investigation_view == InvestigationView::sites ? ImGuiTabItemFlags_SetSelected
                                                                               : 0)) {
            if (investigation_view == InvestigationView::sites)
                investigation_view = InvestigationView::unchanged;
            allocation_sites();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(
                "Snapshot growth", nullptr,
                investigation_view == InvestigationView::growth ? ImGuiTabItemFlags_SetSelected : 0)) {
            if (investigation_view == InvestigationView::growth)
                investigation_view = InvestigationView::unchanged;
            snapshot_growth();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}
void UI::site_tooltip(const AllocationSite &site) {
    if (!ImGui::IsItemHovered())
        return;
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(site_source(site));
    for (auto address : site_frames(site))
        ImGui::Text("%s  %s", hex_address(address).c_str(),
                    shown->offline ? "saved address" : controller.symbol(address).c_str());
    ImGui::EndTooltip();
}
void UI::allocation_sites() {
    if (!shown->allocation_profile || shown->allocation_profile->groups.empty()) {
        empty("No allocation sites yet", "Launch or attach to a process, or open a saved session. "
                                         "Caller-only grouping works with stacks disabled.");
        return;
    }
    const auto &profile = *shown->allocation_profile;
    const auto &totals = profile.totals;
    ImGui::Text("Known live: %llu blocks | %s%llu requested bytes | uncertain: %llu blocks",
                totals.live_count, totals.bytes_saturated ? ">=" : "", totals.live_bytes,
                totals.uncertain_count);
    muted(
        "Retained generations, largest live bytes first. Site = last observed allocation/reallocation call.");
    if (profile.incomplete)
        muted("Incomplete history: totals cover retained observations only.");
    if (ImGui::BeginTable("allocation-sites", 7,
                          ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Site", ImGuiTableColumnFlags_WidthFixed, 320);
        for (const auto *name : {"Live blocks", "Live bytes", "Retained freed", "Uncertain blocks",
                                 "Uncertain bytes", "Oldest observed age"})
            ImGui::TableSetupColumn(name, ImGuiTableColumnFlags_WidthFixed, 125);
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin(int(profile.groups.size()));
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                const auto &row = profile.groups[i];
                const auto &t = row.totals;
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Selectable(site_label(row.site).c_str(), false,
                                      ImGuiSelectableFlags_SpanAllColumns)) {
                    site_filter = row.site;
                    regions_view = false;
                    select_allocation(t.example_generation);
                    if (std::holds_alternative<StackSite>(row.site))
                        select_stack_tab = true;
                }
                site_tooltip(row.site);
                ImGui::TableNextColumn();
                ImGui::Text("%llu", t.live_count);
                ImGui::TableNextColumn();
                ImGui::Text("%s%llu", t.bytes_saturated ? ">=" : "", t.live_bytes);
                ImGui::TableNextColumn();
                ImGui::Text("%llu", t.freed_count);
                ImGui::TableNextColumn();
                ImGui::Text("%llu", t.uncertain_count);
                ImGui::TableNextColumn();
                ImGui::Text("%s%llu", t.bytes_saturated ? ">=" : "", t.uncertain_bytes);
                ImGui::TableNextColumn();
                if (t.oldest_known_birth && profile.frequency)
                    ImGui::Text("%.2f s", double(profile.observed_at - t.oldest_known_birth) /
                                              double(profile.frequency));
                else
                    ImGui::TextUnformatted("unknown");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%llu live records have unknown birth. Offline age ends at the latest "
                                      "retained observation.",
                                      t.unknown_birth_count);
                ImGui::PopID();
            }
        ImGui::EndTable();
    }
}
void UI::snapshot_growth() {
    if (!shown->growth_report || shown->growth_report->observations < 2) {
        empty("Capture at least two named snapshots",
              "Compare retained live requested bytes by site. Three clean observations can establish "
              "repeated growth; growth alone does not prove a leak.");
        return;
    }
    const auto &report = *shown->growth_report;
    const auto repeated = std::count_if(report.rows.begin(), report.rows.end(),
                                        [](const auto &row) { return row.sustained_growth; });
    ImGui::Text("%s -> %s | %zu observations | QPC %llu .. %llu", report.first_name.c_str(),
                report.last_name.c_str(), report.observations, report.begin, report.end);
    ImGui::TextUnformatted(growth_coverage_name(report.coverage));
    ImGui::Text("Repeated growth: %zu site%s", size_t(repeated), repeated == 1 ? "" : "s");
    muted("Growth is an investigation lead, not a confirmed leak. Reallocation can move a record between "
          "sites.");
    if (ImGui::BeginTable("snapshot-growth", 10,
                          ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Site", ImGuiTableColumnFlags_WidthFixed, 320);
        ImGui::TableSetupColumn("Byte delta", ImGuiTableColumnFlags_WidthFixed, 95);
        ImGui::TableSetupColumn("B bytes", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("B blocks", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Evidence", ImGuiTableColumnFlags_WidthFixed, 150);
        for (const auto *name :
             {"A bytes", "A blocks", "Present in all", "Survivor bytes", "Increasing intervals"})
            ImGui::TableSetupColumn(name, ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin(int(report.rows.size()));
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                const auto &row = report.rows[i];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Selectable(site_label(row.site).c_str(), false,
                                      ImGuiSelectableFlags_SpanAllColumns)) {
                    const bool final = row.after.example_generation != 0;
                    select_allocation(final ? row.after.example_generation : row.before.example_generation);
                    for (size_t s = 0; s < shown->snapshots.size(); ++s)
                        if ((final && shown->snapshots[s].end == report.end) ||
                            (!final && shown->snapshots[s].begin == report.begin)) {
                            inspecting_snapshot = int(s);
                            break;
                        }
                }
                site_tooltip(row.site);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.bytes_saturated
                                           ? "overflow: unknown"
                                           : byte_delta(row.before.live_bytes, row.after.live_bytes).c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%s%llu", row.after.bytes_saturated ? ">=" : "", row.after.live_bytes);
                ImGui::TableNextColumn();
                ImGui::Text("%llu", row.after.live_count);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(report.coverage != GrowthCoverage::comparable || row.uncertain
                                           ? "Uncertain"
                                       : row.bytes_saturated  ? "Overflow"
                                       : row.sustained_growth ? "Repeated growth"
                                                              : "No repeated growth");
                ImGui::TableNextColumn();
                ImGui::Text("%s%llu", row.before.bytes_saturated ? ">=" : "", row.before.live_bytes);
                ImGui::TableNextColumn();
                ImGui::Text("%llu", row.before.live_count);
                ImGui::TableNextColumn();
                if (report.coverage == GrowthCoverage::comparable)
                    ImGui::Text("%llu", row.survivor_count);
                else
                    ImGui::TextUnformatted("unknown");
                ImGui::TableNextColumn();
                if (report.coverage == GrowthCoverage::comparable)
                    ImGui::Text("%s%llu", row.bytes_saturated ? ">=" : "", row.survivor_bytes);
                else
                    ImGui::TextUnformatted("unknown");
                ImGui::TableNextColumn();
                ImGui::Text("%u / %zu", row.increasing_intervals, report.observations - 1);
                ImGui::PopID();
            }
        ImGui::EndTable();
    }
}
void UI::dialogs() {
    if (show_attach) {
        ImGui::OpenPopup("Attach to process");
        show_attach = false;
    }
    if (ImGui::BeginPopupModal("Attach to process", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static std::vector<Process> list;
        static char filter[128]{};
        if (list.empty())
            list = processes();
        if (ImGui::Button("Refresh processes"))
            list = processes();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(250);
        ImGui::InputTextWithHint("##process-search", "Process name or PID", filter, sizeof(filter));
        ImGui::TextWrapped(
            "Requires same-user access. Elevated and protected processes may deny instrumentation.");
        ImGui::BeginChild("process-list", ImVec2(650, 380), ImGuiChildFlags_Borders);
        for (const auto &p : list) {
            std::string label = p.name + " | PID " + std::to_string(p.pid) + " | " + p.architecture;
            if (*filter && label.find(filter) == std::string::npos)
                continue;
            ImGui::BeginDisabled(p.architecture != "x64");
            if (ImGui::Selectable(label.c_str())) {
                controller.attach(p.pid);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
        }
        ImGui::EndChild();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (show_launch) {
        ImGui::OpenPopup("Launch executable");
        show_launch = false;
    }
    if (ImGui::BeginPopupModal("Launch executable", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetNextItemWidth(550);
        ImGui::InputText("Executable", launch_path, sizeof(launch_path));
        if (ImGui::Button("Browse executable...")) {
            auto p = choose_file(false, L"Executables\0*.exe\0", L"exe");
            if (!p.empty())
                strncpy_s(launch_path, narrow(p).c_str(), _TRUNCATE);
        }
        ImGui::SetNextItemWidth(550);
        ImGui::InputText("Arguments", launch_args, sizeof(launch_args));
        ImGui::SetNextItemWidth(550);
        ImGui::InputText("Working directory", launch_cwd, sizeof(launch_cwd));
        if (ImGui::Button("Launch & instrument")) {
            try {
                controller.launch(widen(launch_path), widen(launch_args), widen(launch_cwd));
                ImGui::CloseCurrentPopup();
            } catch (const std::exception &e) {
                ui_error = e.what();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (show_limits) {
        ImGui::OpenPopup("Sampling limits & rules");
        show_limits = false;
    }
    if (ImGui::BeginPopupModal("Sampling limits & rules", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static Limits l;
        static int bytes = 4096, scan = 1024, session = 32, interval = 1000, minimum = 4;
        static float threshold = 7.2f, fraction = .35f, delta = 1.5f;
        static int retention = 0, stack_frames = 0;
        ImGui::InputInt("Bytes per allocation", &bytes);
        ImGui::InputInt("KiB per scan", &scan);
        ImGui::InputInt("MiB retained live bytes", &session);
        ImGui::InputInt("Sample interval ms", &interval);
        ImGui::InputInt("Minimum string characters", &minimum);
        const char *retention_modes[] = {"Rolling oldest samples", "Stop at capacity"};
        ImGui::Combo("Retention policy", &retention, retention_modes, 2);
        ImGui::SliderInt("Optional stack frames", &stack_frames, 0, max_stack_frames);
        ImGui::TextWrapped("Stack capture is disabled by default. Enabling it requires the target agent's "
                           "allocation-free native unwinder probe to pass and adds per-call work.");
        ImGui::SliderFloat("High entropy bits/byte", &threshold, 0, 8);
        ImGui::SliderFloat("Changed fraction", &fraction, 0, 1);
        ImGui::SliderFloat("Entropy delta", &delta, 0, 8);
        muted("At most 50,000 allocations, 10,000 events, 8 captures per record.");
        muted("At most 4 snapshots, each retains up to 8 MiB of bytes.");
        if (ImGui::Button("Apply")) {
            l.per_allocation = size_t(std::max(0, bytes));
            l.per_scan = size_t(std::max(0, scan)) * 1024;
            l.per_session = size_t(std::max(0, session)) * 1048576;
            l.sample_ms = unsigned(std::max(0, interval));
            l.min_string = size_t(std::max(0, minimum));
            l.retention = static_cast<Retention>(retention);
            l.stack_frames = static_cast<unsigned>(stack_frames);
            l.high_entropy = threshold;
            l.changed_fraction = fraction;
            l.entropy_change = delta;
            controller.configure(l);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
} // namespace hg
