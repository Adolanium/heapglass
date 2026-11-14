#include "desktop.hpp"
#include <imgui.h>
#include <commdlg.h>
#include <dlgs.h>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void require(bool value, const std::string &message) {
    if (!value)
        throw std::runtime_error(message);
}
void win32_require(bool value, const char *operation) {
    if (!value)
        throw std::runtime_error(std::string(operation) + " failed, Win32 " + std::to_string(GetLastError()));
}

class PrivateDesktop {
    HDESK original = GetThreadDesktop(GetCurrentThreadId());
    HDESK isolated = nullptr;

  public:
    PrivateDesktop() {
        auto name = L"HeapglassValidation-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                    std::to_wstring(GetTickCount64());
        isolated = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        win32_require(isolated != nullptr, "CreateDesktopW");
        if (!SetThreadDesktop(isolated)) {
            const DWORD error = GetLastError();
            CloseDesktop(isolated);
            isolated = nullptr;
            throw std::runtime_error("SetThreadDesktop failed, Win32 " + std::to_string(error));
        }
        // Never call SwitchDesktop. Native modal windows remain on this unshown desktop.
    }
    ~PrivateDesktop() {
        if (isolated) {
            SetThreadDesktop(original);
            CloseDesktop(isolated);
        }
    }
};

struct WindowState {
    float style_dpi = 1.0f;
};
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto *creation = reinterpret_cast<CREATESTRUCTW *>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
    }
    if (message == WM_DPICHANGED) {
        auto *state = reinterpret_cast<WindowState *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (state)
            hg::apply_dpi(window, HIWORD(wparam), 1.0f, state->style_dpi,
                          reinterpret_cast<const RECT *>(lparam));
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

class Windows {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    std::wstring name = L"HeapglassIsolatedDialogValidation-" + std::to_wstring(GetCurrentProcessId());
    ATOM atom = 0;

  public:
    HWND owner = nullptr, subject = nullptr;
    WindowState state;
    Windows() {
        WNDCLASSW cls{};
        cls.hInstance = instance;
        cls.lpfnWndProc = window_proc;
        cls.lpszClassName = name.c_str();
        atom = RegisterClassW(&cls);
        win32_require(atom != 0, "RegisterClassW");
        owner = CreateWindowExW(0, name.c_str(), L"Hidden validation owner", WS_POPUP, 0, 0, 100, 100,
                                nullptr, nullptr, instance, nullptr);
        subject = CreateWindowExW(0, name.c_str(), L"Hidden owned validation window", WS_POPUP, 10, 10, 640,
                                  480, owner, nullptr, instance, &state);
        if (!owner || !subject) {
            const DWORD error = GetLastError();
            if (subject)
                DestroyWindow(subject);
            if (owner)
                DestroyWindow(owner);
            UnregisterClassW(name.c_str(), instance);
            throw std::runtime_error("CreateWindowExW failed, Win32 " + std::to_string(error));
        }
    }
    ~Windows() {
        if (subject)
            DestroyWindow(subject);
        if (owner)
            DestroyWindow(owner);
        if (atom)
            UnregisterClassW(name.c_str(), instance);
    }
};

class TemporaryFiles {
  public:
    std::filesystem::path directory, existing, saved;
    TemporaryFiles() {
        directory = std::filesystem::temp_directory_path() /
                    (L"Heapglass-dialog-\u05d1\u05d3\u05d9\u05e7\u05d4-\u03a9-" +
                     std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        require(std::filesystem::create_directory(directory),
                "Could not create unique dialog TEMP directory");
        existing = directory / L"open-\u05e7\u05d5\u05d1\u05e5-\u03a9.json";
        saved = directory / L"save-\u05e7\u05d5\u05d1\u05e5-\u03a9.json";
        std::ofstream output(existing, std::ios::binary);
        output << "owned dialog fixture\n";
        output.close();
        require(bool(output), "Could not write the owned Unicode dialog fixture");
    }
    ~TemporaryFiles() {
        std::error_code error;
        std::filesystem::remove(existing, error);
        std::filesystem::remove(saved, error);
        // Only delete the two named fixture files and their now-empty directory.
        std::filesystem::remove(directory, error);
    }
    void cleanup_and_verify() {
        std::error_code error;
        std::filesystem::remove(existing, error);
        require(!error, "Could not remove the owned Open fixture");
        std::filesystem::remove(saved, error);
        require(!error, "Could not remove the owned Save fixture");
        require(std::filesystem::remove(directory, error) && !error,
                "Owned dialog TEMP directory was not empty or could not be removed");
        require(!std::filesystem::exists(directory), "Owned dialog TEMP directory remains after cleanup");
    }
};

struct DialogAction {
    std::wstring filename;
    bool cancel = false, submitted = false, filled = false, input_failed = false, timed_out = false,
         saw_dialog = false;
    ULONGLONG deadline = 0;
    HWND dialog = nullptr;
    HWND filename_edit = nullptr;
};
thread_local DialogAction *active_action = nullptr;
BOOL CALLBACK find_filename_edit(HWND window, LPARAM parameter) {
    wchar_t cls[64]{}, parent_cls[64]{}, grandparent_cls[64]{};
    GetClassNameW(window, cls, 64);
    const int id = GetDlgCtrlID(window);
    const HWND parent = GetParent(window);
    if (parent) {
        GetClassNameW(parent, parent_cls, 64);
        const HWND grandparent = GetParent(parent);
        if (grandparent)
            GetClassNameW(grandparent, grandparent_cls, 64);
    }
    // Current Windows Save uses a shell filename ComboBox/Edit under
    // FloatNotifySink. The address bar has a different ancestor and is excluded.
    const bool shell_filename = id == 1001 && std::wstring(parent_cls) == L"ComboBox" &&
                                std::wstring(grandparent_cls) == L"FloatNotifySink";
    if (std::wstring(cls) == L"Edit" && (id == edt1 || id == cmb13 || shell_filename)) {
        reinterpret_cast<DialogAction *>(parameter)->filename_edit = window;
        return FALSE;
    }
    return TRUE;
}
BOOL CALLBACK find_dialog(HWND window, LPARAM parameter) {
    auto *action = reinterpret_cast<DialogAction *>(parameter);
    wchar_t cls[64]{};
    if (GetClassNameW(window, cls, 64) && std::wstring(cls) == L"#32770" && IsWindowVisible(window)) {
        action->dialog = window;
        return FALSE;
    }
    return TRUE;
}
void CALLBACK dialog_timer(HWND, UINT, UINT_PTR, DWORD) {
    auto *action = active_action;
    if (!action)
        return;
    action->dialog = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), find_dialog, reinterpret_cast<LPARAM>(action));
    if (!action->dialog)
        return;
    action->saw_dialog = true;
    if (GetTickCount64() >= action->deadline) {
        action->timed_out = true;
        PostMessageW(action->dialog, WM_COMMAND, IDCANCEL, 0);
    } else if (!action->submitted) {
        if (!action->cancel && !action->filled) {
            const int filename_control = GetDlgItem(action->dialog, edt1) ? edt1 : cmb13;
            SendMessageW(action->dialog, CDM_SETCONTROLTEXT, filename_control,
                         reinterpret_cast<LPARAM>(action->filename.c_str()));
            EnumChildWindows(action->dialog, find_filename_edit, reinterpret_cast<LPARAM>(action));
            wchar_t actual[32768]{};
            if (action->filename_edit) {
                GetWindowTextW(action->filename_edit, actual, 32768);
                // Shell dialog templates can ignore CDM_SETCONTROLTEXT. Target
                // only the discovered filename edit, never an address/search box.
                if (action->filename != actual)
                    SetWindowTextW(action->filename_edit, action->filename.c_str());
                GetWindowTextW(action->filename_edit, actual, 32768);
            }
            if (action->filename != actual) {
                action->input_failed = true;
                PostMessageW(action->dialog, WM_COMMAND, IDCANCEL, 0);
                return;
            }
            action->filled = true;
            return; // Let the dialog process filename change notifications before OK.
        }
        const int button = action->cancel ? IDCANCEL : IDOK;
        PostMessageW(GetDlgItem(action->dialog, button), BM_CLICK, 0, 0);
        action->submitted = true;
    }
}

std::wstring choose_file(HWND owner, bool save, bool cancel, const std::filesystem::path &path) {
    DialogAction action;
    action.filename = path.wstring();
    action.cancel = cancel;
    action.deadline = GetTickCount64() + 8000;
    active_action = &action;
    const UINT_PTR timer = SetTimer(nullptr, 0, 50, dialog_timer);
    if (!timer) {
        active_action = nullptr;
        win32_require(false, "SetTimer");
    }
    std::wstring result;
    try {
        result = hg::file_dialog(save, L"Owned JSON fixtures\0*.json\0All files\0*.*\0", L"json", owner);
    } catch (...) {
        KillTimer(nullptr, timer);
        active_action = nullptr;
        throw;
    }
    KillTimer(nullptr, timer);
    active_action = nullptr;
    require(action.saw_dialog, "No native common dialog was observed on the private worker desktop");
    require(!action.input_failed, "Could not populate the private native dialog's filename edit");
    require(!action.timed_out, "Native common dialog timed out and was cancelled on its private desktop");
    require(action.submitted, "Native common dialog did not receive its test selection");
    return result;
}

void synthetic_dpi_messages(Windows &windows) {
    ImGui::CreateContext();
    struct ContextCleanup {
        ~ContextCleanup() {
            ImGui::DestroyContext();
        }
    } cleanup;
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(1600, 1000);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char *pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    auto &style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(12, 10);
    style.ItemSpacing = ImVec2(8, 6);
    style.FramePadding = ImVec2(4, 4);
    struct Expectation {
        unsigned dpi;
        int width, height;
        float scale, padding, spacing, font_size;
    };
    const Expectation cases[] = {
        {96, 640, 480, 1.0f, 12, 8, 13},
        {144, 960, 720, 1.5f, 18, 12, 19.5f},
        {192, 1280, 960, 2.0f, 24, 16, 26},
        {96, 640, 480, 1.0f, 12, 8, 13},
    };
    for (const auto &expected : cases) {
        RECT suggested{20, 30, 20 + expected.width, 30 + expected.height};
        SendMessageW(windows.subject, WM_DPICHANGED, MAKEWPARAM(expected.dpi, expected.dpi),
                     reinterpret_cast<LPARAM>(&suggested));
        RECT client{}, outer{};
        win32_require(GetClientRect(windows.subject, &client) != FALSE, "GetClientRect");
        win32_require(GetWindowRect(windows.subject, &outer) != FALSE, "GetWindowRect");
        require(client.right == expected.width && client.bottom == expected.height,
                "Synthetic WM_DPICHANGED did not apply the suggested client size");
        require(EqualRect(&suggested, &outer) != FALSE,
                "Synthetic WM_DPICHANGED did not apply the suggested native position and size");
        require(std::abs(io.FontGlobalScale - expected.scale) < .001f &&
                    std::abs(windows.state.style_dpi - expected.scale) < .001f &&
                    std::abs(style.WindowPadding.x - expected.padding) < .001f &&
                    std::abs(style.ItemSpacing.x - expected.spacing) < .001f &&
                    std::abs(style.FramePadding.x - 4 * expected.scale) < .001f,
                "Synthetic WM_DPICHANGED did not rescale ImGui font and layout metrics");
        ImGui::NewFrame();
        ImGui::Begin("DPI validation");
        require(std::abs(ImGui::GetFontSize() - expected.font_size) < .001f,
                "Rendered font size differs after synthetic WM_DPICHANGED");
        ImGui::TextUnformatted("Synthetic DPI-message validation");
        ImGui::End();
        ImGui::Render();
    }
}

void worker_validation() {
    PrivateDesktop desktop; // Must precede every window and ImGui context on this worker.
    Windows windows;
    TemporaryFiles files;
    require(!IsWindowVisible(windows.owner) && !IsWindowVisible(windows.subject),
            "Validation owner windows must remain hidden");
    const auto opened = choose_file(windows.subject, false, false, files.existing);
    require(opened == files.existing.wstring(), "Native Open dialog returned the wrong Unicode path");
    const auto saved = choose_file(windows.subject, true, false, files.saved);
    require(saved == files.saved.wstring(), "Native Save dialog returned the wrong Unicode path");
    require(choose_file(windows.subject, false, true, files.existing).empty(),
            "Cancelling the native Open dialog returned a filename");
    require(choose_file(windows.subject, true, true, files.saved).empty(),
            "Cancelling the native Save dialog returned a filename");
    std::ifstream input(files.existing, std::ios::binary);
    std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    require(contents == "owned dialog fixture\n", "Native file dialogs modified the owned input fixture");
    synthetic_dpi_messages(windows);
    files.cleanup_and_verify();
}
} // namespace

int main() {
    std::exception_ptr failure;
    std::thread worker([&] {
        try {
            worker_validation();
        } catch (...) {
            failure = std::current_exception();
        }
    });
    worker.join();
    if (failure) {
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception &error) {
            std::cerr << "Native desktop validation failed: " << error.what() << '\n';
        }
        return 1;
    }
    std::cout << "PASS: Unicode native Open/Save and cancellation on an unshown private desktop; "
                 "synthetic DPI-message validation at 100/150/200 percent (not physical monitor proof)\n";
    return 0;
}
