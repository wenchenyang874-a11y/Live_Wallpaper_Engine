#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include "core/WallpaperLibrary.h"

namespace lwe::app {

class ModernMainWindow final {
public:
    enum ControlId : int {
        Search = 1100,
        Filter = 1101,
        Library = 1102,
        Import = 1103,
        Export = 1104,
        Apply = 1105,
        Sound = 1106,
        Hide = 1107,
        Exit = 1108,
        FilterStatic = 1109,
        FilterGif = 1110,
        FilterVideo = 1111,
    };

    ModernMainWindow() = default;
    ~ModernMainWindow();

    ModernMainWindow(const ModernMainWindow&) = delete;
    ModernMainWindow& operator=(const ModernMainWindow&) = delete;

    bool Create(HWND parent, HINSTANCE instance);
    void Layout();
    void DpiChanged();
    void Paint(HDC deviceContext, const RECT& paintRectangle) const;
    bool DrawItem(const DRAWITEMSTRUCT& draw) const;
    HBRUSH ColorControl(HDC deviceContext, HWND control) const;
    bool HandleFilterCommand(WORD controlId, WORD notificationCode);

    void SetItems(std::vector<core::WallpaperItem> items);
    void SetActivePath(std::wstring_view path);
    void SetStatus(std::wstring status);
    void SetSoundEnabled(bool enabled);
    std::optional<core::WallpaperItem> SelectedItem() const;
    HWND SearchControl() const noexcept;
    HWND LibraryControl() const noexcept;

private:
    enum class FilterKind {
        All,
        StaticImage,
        AnimatedGif,
        Video,
    };

    void RecreateFonts();
    void RefreshVisibleItems();
    bool MatchesFilter(const core::WallpaperItem& item,
                       std::wstring_view search) const;
    void DrawButton(const DRAWITEMSTRUCT& draw) const;
    void DrawLibraryItem(const DRAWITEMSTRUCT& draw) const;

    HWND parent_ = nullptr;
    HWND search_ = nullptr;
    HWND filter_ = nullptr;
    HWND filterStatic_ = nullptr;
    HWND filterGif_ = nullptr;
    HWND filterVideo_ = nullptr;
    HWND library_ = nullptr;
    HWND import_ = nullptr;
    HWND export_ = nullptr;
    HWND apply_ = nullptr;
    HWND sound_ = nullptr;
    HWND hide_ = nullptr;
    HWND exit_ = nullptr;
    HFONT titleFont_ = nullptr;
    HFONT headingFont_ = nullptr;
    HFONT bodyFont_ = nullptr;
    HFONT smallFont_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    HBRUSH panelBrush_ = nullptr;
    std::vector<core::WallpaperItem> items_;
    std::vector<std::size_t> visibleIndices_;
    std::wstring activePath_;
    std::wstring status_ = L"准备就绪";
    FilterKind filterKind_ = FilterKind::All;
    bool soundEnabled_ = false;
};

}  // namespace lwe::app
