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
        DropdownList = 1109,
        ActiveStatus = 1110,
        ActiveList = 1111,
        ActiveDrawerHeader = 1112,
        DisplayModeChanged = 1113,
        CancelSelectedWallpaper = 1114,
        DisplayMode = 1199,
    };

    static constexpr UINT_PTR AnimationTimerId = 50;

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
    void HandleAnimationTimer();
    void CloseTransientUi();
    std::optional<std::vector<std::wstring>> ChooseDisplayTargets();

    void SetItems(std::vector<core::WallpaperItem> items);
    void SetActivePaths(std::vector<std::wstring> paths);
    void SetStatus(std::wstring status);
    void SetSoundEnabled(bool enabled);
    void SetDisplayOptions(std::vector<DisplayOption> displays, bool spanDisplays);
    void SetResourceUsage(std::wstring usage);
    [[nodiscard]] bool SpanAcrossDisplays() const noexcept;
    std::optional<core::WallpaperItem> SelectedItem() const;
    std::optional<core::WallpaperItem> SelectedActiveItem() const;
    bool SelectItemAtScreenPoint(POINT screenPoint);
    bool SelectActiveItemAtScreenPoint(POINT screenPoint);
    void BeginRenameSelected();
    void BeginRenameActiveSelected();
    std::optional<std::pair<core::WallpaperItem, std::wstring>> FinishRename();
    HWND SearchControl() const noexcept;
    HWND LibraryControl() const noexcept;
    HWND ActiveLibraryControl() const noexcept;

private:
    enum class FilterKind {
        All,
        StaticImage,
        AnimatedGif,
        Video,
    };

    enum class DropdownKind {
        None,
        Filter,
        DisplayMode,
    };

    void RecreateFonts();
    void RefreshVisibleItems();
    void RefreshActiveItems();
    void InvalidateFooter() const;
    bool ShowFilterMenu();
    bool ShowDisplayModeMenu();
    bool ToggleDropdown(DropdownKind kind);
    void HideDropdown();
    void ToggleActiveDrawer();
    void HideActiveDrawer();
    bool MatchesFilter(const core::WallpaperItem& item,
                       std::wstring_view search) const;
    void DrawButton(const DRAWITEMSTRUCT& draw) const;
    void DrawLibraryItem(const DRAWITEMSTRUCT& draw) const;
    void DrawActiveItem(const DRAWITEMSTRUCT& draw) const;
    void DrawDropdownItem(const DRAWITEMSTRUCT& draw) const;
    void DrawWallpaperCard(const DRAWITEMSTRUCT& draw,
                           const core::WallpaperItem& item, bool active,
                           bool showCancelButton, float hoverProgress) const;
    void BeginRenameItem(const core::WallpaperItem& item, HWND list,
                         LRESULT selection);
    bool HitLibraryActiveBadge(POINT clientPoint, UINT& itemIndex) const;
    bool HitActiveCancelButton(POINT clientPoint, UINT& itemIndex) const;
    void SetControlHovered(HWND control, bool hovered);
    void SetListHovered(HWND list, POINT clientPoint, bool hovered);
    void StartHoverAnimation();
    float ControlHoverProgress(HWND control) const;
    HBITMAP LoadThumbnail(std::wstring_view path) const;
    void ClearThumbnails();
    void CancelRename();
    static LRESULT CALLBACK RenameEditProcedure(HWND window, UINT message,
                                                WPARAM wParam, LPARAM lParam,
                                                UINT_PTR subclassId,
                                                DWORD_PTR referenceData);
    static LRESULT CALLBACK InteractiveControlProcedure(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR subclassId, DWORD_PTR referenceData);

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
    HWND displayMode_ = nullptr;
    HWND dropdownList_ = nullptr;
    HWND activeStatus_ = nullptr;
    HWND activeList_ = nullptr;
    HWND activeDrawerHeader_ = nullptr;
    std::vector<DisplayOption> displayOptions_;
    HFONT titleFont_ = nullptr;
    HFONT headingFont_ = nullptr;
    HFONT bodyFont_ = nullptr;
    HFONT smallFont_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    HBRUSH panelBrush_ = nullptr;
    HICON appIcon_ = nullptr;
    std::vector<core::WallpaperItem> items_;
    std::vector<std::size_t> visibleIndices_;
    std::vector<std::size_t> activeVisibleIndices_;
    std::vector<std::wstring> activePaths_;
    std::wstring status_ = L"准备就绪";
    std::wstring resourceUsage_ = L"CPU --  GPU --  内存 --  显存 --";
    std::wstring renamingPath_;
    std::unordered_map<std::wstring, HBITMAP> thumbnails_;
    FilterKind filterKind_ = FilterKind::All;
    bool soundEnabled_ = false;
    bool spanAcrossDisplays_ = true;
    bool activeDrawerVisible_ = false;
    bool dropdownUpdating_ = false;
    bool dropdownSuppressSelectionNotification_ = false;
    DropdownKind dropdownKind_ = DropdownKind::None;
    std::vector<std::wstring> dropdownLabels_;
    std::vector<std::wstring> dropdownDescriptions_;
    std::unordered_map<HWND, float> hoverProgress_;
    std::unordered_map<HWND, bool> hoverTargets_;
    int libraryHoverIndex_ = -1;
    int activeHoverIndex_ = -1;
    int dropdownHoverIndex_ = -1;
    int libraryBadgeHoverIndex_ = -1;
    int activeCancelHoverIndex_ = -1;
    float libraryHoverProgress_ = 0.0F;
    float activeHoverProgress_ = 0.0F;
    float dropdownHoverProgress_ = 0.0F;
    bool libraryHoverTarget_ = false;
    bool activeHoverTarget_ = false;
    bool dropdownHoverTarget_ = false;

    void UpdateFilterSelectorText();
    void UpdateDisplayModeText();
};

}  // namespace lwe::app
