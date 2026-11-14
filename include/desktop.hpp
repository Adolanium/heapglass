#pragma once
#include <windows.h>
#include <string>
namespace hg {
std::wstring file_dialog(bool save, const wchar_t *filter, const wchar_t *extension, HWND owner = nullptr);
void apply_dpi(HWND window, unsigned dpi, float font_dpi, float &current_style_dpi, const RECT *suggested);
} // namespace hg
