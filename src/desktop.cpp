#include "desktop.hpp"
#include <commdlg.h>
#include <imgui.h>
#include <cmath>
#include <stdexcept>
namespace hg {
std::wstring file_dialog(bool save, const wchar_t *filter, const wchar_t *extension, HWND owner) {
    wchar_t path[32768]{};
    OPENFILENAMEW options{};
    options.lStructSize = sizeof(options);
    options.hwndOwner = owner ? owner : GetActiveWindow();
    options.lpstrFilter = filter;
    options.lpstrFile = path;
    options.nMaxFile = 32768;
    options.lpstrDefExt = extension;
    options.Flags = OFN_EXPLORER | OFN_DONTADDTORECENT | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST |
                    (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (save ? GetSaveFileNameW(&options) : GetOpenFileNameW(&options))
        return path;
    auto error = CommDlgExtendedError();
    if (error)
        throw std::runtime_error("File dialog failed (" + std::to_string(error) +
                                 "). Check the path and access permissions.");
    return {};
}
void apply_dpi(HWND window, unsigned dpi, float font_dpi, float &current_style_dpi, const RECT *suggested) {
    if (dpi < 48 || dpi > 768 || !std::isfinite(font_dpi) || font_dpi <= 0 ||
        !std::isfinite(current_style_dpi) || current_style_dpi <= 0)
        return;
    const float scale = float(dpi) / 96.f;
    if (ImGui::GetCurrentContext()) {
        ImGui::GetIO().FontGlobalScale = scale / font_dpi;
        ImGui::GetStyle().ScaleAllSizes(scale / current_style_dpi);
        current_style_dpi = scale;
    }
    if (window && suggested)
        SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
}
} // namespace hg
