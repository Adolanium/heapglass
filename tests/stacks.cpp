#include "controller.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace hg;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x))                                                                                            \
            throw std::runtime_error(#x);                                                                    \
    } while (false)
template <class F> std::shared_ptr<const Model> await(Controller &c, F condition, unsigned timeout = 12000) {
    auto start = GetTickCount64();
    for (;;) {
        auto view = c.view();
        if (condition(*view))
            return view;
        if (GetTickCount64() - start > timeout)
            throw std::runtime_error("Stack validation timeout: " + view->status);
        Sleep(20);
    }
}
int main() {
    try {
        Controller controller;
        Limits settings;
        settings.stack_frames = 8;
        controller.configure(settings);
        await(controller, [](const Model &m) { return m.limits.stack_frames == 8; });
        controller.launch(executable_directory() + L"\\HeapglassTarget.exe", L"--duration 7");
        auto model = await(controller, [](const Model &m) {
            size_t count = 0;
            for (const auto &e : m.events)
                if (e.stack_count)
                    ++count;
            return m.connected && count >= 100;
        });
        CHECK(model->active_stack_frames == 8);
        CHECK(model->stack_state == 1);
        CHECK(model->stack_violations == 0);
        HANDLE target =
            OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, model->pid);
        CHECK(target);
        for (const auto &e : model->events) {
            CHECK(e.stack_count <= 8);
            for (unsigned i = 0; i < e.stack_count; ++i) {
                CHECK(e.stack[i] != 0);
                MEMORY_BASIC_INFORMATION region{};
                CHECK(VirtualQueryEx(target, reinterpret_cast<LPCVOID>(e.stack[i]), &region,
                                     sizeof(region)) == sizeof(region));
                CHECK(region.State == MEM_COMMIT);
            }
        }
        auto path = std::filesystem::temp_directory_path() /
                    (L"heapglass-stacks-" + std::to_wstring(GetCurrentProcessId()) + L".json");
        controller.save(path.wstring(), false);
        await(controller, [](const Model &m) { return m.status == "Session saved"; });
        auto saved = load_session(path.wstring());
        bool stack_found = false;
        for (const auto &[_, a] : saved.records)
            if (a.stack_count) {
                stack_found = true;
                CHECK(a.stack_status == 1);
            }
        CHECK(stack_found);
        std::filesystem::remove(path);
        settings.stack_frames = 0;
        controller.configure(settings);
        await(controller, [](const Model &m) {
            return m.active_stack_frames == 0 && !m.events.empty() && m.events.back().stack_count == 0;
        });
        settings.stack_frames = 8;
        controller.configure(settings);
        await(controller, [](const Model &m) {
            return m.active_stack_frames == 8 && !m.events.empty() && m.events.back().stack_count > 0;
        });
        CHECK(WaitForSingleObject(target, 12000) == WAIT_OBJECT_0);
        DWORD result = 1;
        CHECK(GetExitCodeProcess(target, &result));
        CloseHandle(target);
        CHECK(result == 0);
        model = await(controller,
                      [](const Model &m) { return !m.connected && m.status.starts_with("Target exited"); });
        CHECK(model->stack_violations == 0);
        Controller unsafe(L"HeapglassAgentStackFault.dll");
        settings.stack_frames = 8;
        unsafe.configure(settings);
        await(unsafe, [](const Model &m) { return m.limits.stack_frames == 8; });
        unsafe.launch(executable_directory() + L"\\HeapglassTarget.exe", L"--duration 3");
        auto rejected = await(unsafe, [](const Model &m) {
            return m.connected && m.stack_state == 2 && m.stack_violations > 0 && m.incomplete &&
                   m.dropped > 0;
        });
        CHECK(rejected->active_stack_frames == 0);
        CHECK(rejected->incomplete);
        CHECK(rejected->dropped > 0);
        HANDLE operational =
            OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, rejected->pid);
        CHECK(operational);
        CHECK(WaitForSingleObject(operational, 0) == WAIT_TIMEOUT);
        CHECK(WaitForSingleObject(operational, 10000) == WAIT_OBJECT_0);
        DWORD unsafe_exit = 1;
        CHECK(GetExitCodeProcess(operational, &unsafe_exit));
        CloseHandle(operational);
        CHECK(unsafe_exit == 0);
        std::cout << "Bounded stack capture, allocation-free probe, native API behavior, toggling and saved "
                     "frames passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
