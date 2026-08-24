#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>

#include "core/WallpaperLibrary.h"

namespace lwe::app {

class ModernMainWindow final {
public:
    struct DisplayOption final {
        std::wstring id;
        std::wstring label;
        bool selected = false;
    };

    enum ControlId : int {
        Search = 1100,
        Filter = 1101,
        Library = 1102,
        Import = 1103,
        Export = 1104,
        Apply = 1105,
        Sound = 1106,
        CancelApplication = 1107,
        RenameCommit = 1108,
        DisplaySelector = 1199,
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
    bool ShowDisplaySelectorMenu();

    void SetItems(std::vector<core::WallpaperItem> items);
    void SetActivePaths(std::vector<std::wstring> paths);
    void SetStatus(std::wstring status);
    void SetSoundEnabled(bool enabled);
    void SetDisplayOptions(std::vector<DisplayOption> displays, bool spanDisplays);
    void SetResourceUsage(std::wstring usage);
    [[nodiscard]] std::vector<std::wstring> SelectedDisplayIds() const;
    [[nodiscard]] bool SpanAcrossDisplays() const noexcept;
    std::optional<core::WallpaperItem> SelectedItem() const;
    bool SelectItemAtScreenPoint(POINT screenPoint);
    void BeginRenameSelected();
    std::optional<std::pair<core::WallpaperItem, std::wstring>> FinishRename();
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
    void InvalidateFooter() const;
    bool ShowFilterMenu();
    bool MatchesFilter(const core::WallpaperItem& item,
                       std::wstring_view search) const;
    void DrawButton(const DRAWITEMSTRUCT& draw) const;
    void DrawLibraryItem(const DRAWITEMSTRUCT& draw) const;
    HBITMAP LoadThumbnail(std::wstring_view path) const;
    void ClearThumbnails();
    void CancelRename();
    static LRESULT CALLBACK RenameEditProcedure(HWND window, UINT message,
                                                WPARAM wParam, LPARAM lParam,
                                                UINT_PTR subclassId,
                                                DWORD_PTR referenceData);

    HWND parent_ = nullptr;
    HWND search_ = nullptr;
    HWND filter_ = nullptr;
    HWND library_ = nullptr;
    HWND import_ = nullptr;
    HWND export_ = nullptr;
    HWND apply_ = nullptr;
    HWND sound_ = nullptr;
    HWND cancelApplication_ = nullptr;
    HWND renameEdit_ = nullptr;
    HWND displaySelector_ = nullptr;
    std::vector<DisplayOption> displayOptions_;
    HFONT titleFont_ = nullptr;
    HFONT headingFont_ = nullptr;
    HFONT bodyFont_ = nullptr;
    HFONT smallFont_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    HBRUSH panelBrush_ = nullptr;
    std::vector<core::WallpaperItem> items_;
    std::vector<std::size_t> visibleIndices_;
    std::vector<std::wstring> activePaths_;
    std::wstring status_ = L"准备就绪";
    std::wstring resourceUsage_ = L"CPU --  GPU --  内存 --  显存 --";
    std::wstring renamingPath_;
    std::unordered_map<std::wstring, HBITMAP> thumbnails_;
    FilterKind filterKind_ = FilterKind::All;
    bool soundEnabled_ = false;
    bool spanAcrossDisplays_ = true;

    void UpdateFilterSelectorText();
    void UpdateDisplaySelectorText();
};

}  // namespace lwe::app
