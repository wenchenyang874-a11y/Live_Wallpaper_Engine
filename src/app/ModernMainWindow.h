#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <windows.h>

#include "core/WallpaperLibrary.h"
#include "core/WallpaperGroupStore.h"

namespace lwe::app {

class ModernMainWindow final {
public:
    enum class ImportChoice {
        MediaFiles,
        SharePackage,
    };

    struct DisplayOption final {
        std::wstring id;
        std::wstring label;
        std::wstring activeWallpaperName;
        bool selected = false;

        [[nodiscard]] bool Occupied() const noexcept {
            return !activeWallpaperName.empty();
        }
    };

    struct DisplayBadge final {
        std::wstring label;
        bool primary = false;
    };

    struct ActiveWallpaperInfo final {
        std::wstring path;
        std::wstring displayLabel;
        std::vector<DisplayBadge> displayBadges;
    };

    enum ControlId : int {
        Search = 1100,
        Filter = 1101,
        Library = 1102,
        Import = 1103,
        Export = 1104,
        Sound = 1106,
        RenameCommit = 1108,
        DropdownList = 1109,
        ActiveStatus = 1110,
        ActiveList = 1111,
        DisplayModeChanged = 1113,
        CancelSelectedWallpaper = 1114,
        ExportSelectAll = 1115,
        ExportClearAll = 1116,
        ExportConfirm = 1117,
        ExportCancel = 1118,
        LibraryReordered = 1119,
        GroupAll = 1120,
        GroupFavorites = 1121,
        GroupList = 1122,
        GroupCreate = 1123,
        GroupChanged = 1124,
        GroupReordered = 1125,
        GroupRenameCommit = 1126,
        DisplayMode = 1199,
    };

    static constexpr UINT_PTR AnimationTimerId = 50;
    static constexpr UINT_PTR DragScrollTimerId = 51;
    static constexpr UINT_PTR GroupDragScrollTimerId = 52;
    static constexpr std::wstring_view AllGroupId = L"all";
    static constexpr std::wstring_view FavoritesGroupId = L"favorites";

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
    std::optional<ImportChoice> ChooseImportSource();

    void SetItems(std::vector<core::WallpaperItem> items);
    void SetGroups(std::vector<core::WallpaperGroup> groups,
                   std::vector<std::wstring> favorites,
                   std::wstring_view selectedGroupId = {});
    void SetActiveWallpapers(std::vector<ActiveWallpaperInfo> wallpapers);
    void SetStatus(std::wstring status);
    void SetSoundEnabled(bool enabled);
    void SetDisplayOptions(std::vector<DisplayOption> displays, bool spanDisplays);
    void SetResourceUsage(std::wstring usage);
    [[nodiscard]] bool SpanAcrossDisplays() const noexcept;
    std::optional<core::WallpaperItem> SelectedItem() const;
    std::optional<core::WallpaperItem> SelectedActiveItem() const;
    void BeginExportSelection(std::wstring_view initiallySelectedPath = {});
    void EndExportSelection();
    void SelectAllVisibleForExport();
    void ClearExportSelection();
    [[nodiscard]] bool ExportSelectionActive() const noexcept;
    [[nodiscard]] std::vector<core::WallpaperItem> SelectedExportItems() const;
    [[nodiscard]] std::wstring CurrentGroupId() const;
    [[nodiscard]] bool CurrentGroupIsAll() const noexcept;
    [[nodiscard]] bool CurrentGroupIsFavorites() const noexcept;
    [[nodiscard]] std::optional<core::WallpaperGroup> SelectedCustomGroup() const;
    std::optional<std::vector<std::wstring>> TakePendingGroupOrder();
    bool SelectCustomGroupAtScreenPoint(POINT screenPoint);
    void BeginRenameSelectedGroup();
    std::optional<std::pair<std::wstring, std::wstring>> FinishGroupRename();
    std::optional<std::vector<core::WallpaperItem>> TakePendingLibraryOrder();
    bool SelectItemAtScreenPoint(POINT screenPoint);
    bool SelectActiveItemAtScreenPoint(POINT screenPoint);
    void BeginRenameSelected();
    void BeginRenameActiveSelected();
    std::optional<std::pair<core::WallpaperItem, std::wstring>> FinishRename();
    HWND SearchControl() const noexcept;
    HWND LibraryControl() const noexcept;
    HWND ActiveLibraryControl() const noexcept;
    HWND GroupListControl() const noexcept;
    HWND BatchActionsControl() const noexcept;
    [[nodiscard]] std::uint64_t LibraryDrawCount() const noexcept;

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
    void SelectDropdownItem(std::size_t index);
    void HideDropdown();
    void ToggleActiveDrawer();
    void HideActiveDrawer();
    bool MatchesFilter(const core::WallpaperItem& item,
                       std::wstring_view search) const;
    void DrawButton(const DRAWITEMSTRUCT& draw) const;
    void DrawLibraryItem(const DRAWITEMSTRUCT& draw) const;
    void DrawActiveItem(const DRAWITEMSTRUCT& draw) const;
    void DrawDropdownItem(const DRAWITEMSTRUCT& draw) const;
    void DrawGroupItem(const DRAWITEMSTRUCT& draw) const;
    void DrawWallpaperCard(const DRAWITEMSTRUCT& draw,
                            const core::WallpaperItem& item, bool active,
                            std::wstring_view displayLabel,
                            std::span<const DisplayBadge> displayBadges,
                            bool showCancelButton, float hoverProgress,
                            bool showSelectionBox = false,
                            bool selectionChecked = false) const;
    [[nodiscard]] const ActiveWallpaperInfo* FindActiveWallpaper(
        std::wstring_view path) const;
    void BeginRenameItem(const core::WallpaperItem& item, HWND list,
                         LRESULT selection);
    bool HitLibraryActiveBadge(POINT clientPoint, UINT& itemIndex) const;
    bool HitLibraryExportCheckbox(POINT clientPoint, UINT& itemIndex) const;
    bool HitActiveCancelButton(POINT clientPoint, UINT& itemIndex) const;
    void ToggleExportSelectionAt(UINT visibleIndex);
    void UpdateExportSelectionControls();
    void SelectGroup(std::wstring_view groupId);
    bool ItemBelongsToCurrentGroup(const core::WallpaperItem& item) const;
    void RefreshGroupItems();
    void UpdateGroupTooltip(int groupIndex);
    void BeginGroupDrag(POINT clientPoint);
    void UpdateGroupDrag(POINT clientPoint);
    void UpdateGroupDragTarget(POINT clientPoint);
    void FinishGroupDrag(POINT clientPoint);
    void CancelGroupDrag();
    void ScrollGroupsDuringDrag();
    void BeginLibraryDrag(POINT clientPoint);
    void UpdateLibraryDrag(POINT clientPoint);
    void FinishLibraryDrag(POINT clientPoint);
    void CancelLibraryDrag();
    void ScrollLibraryDuringDrag();
    void UpdateLibraryDragTarget(POINT clientPoint);
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
    static LRESULT CALLBACK GroupTooltipProcedure(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR subclassId, DWORD_PTR referenceData);

    HWND parent_ = nullptr;
    HWND search_ = nullptr;
    HWND filter_ = nullptr;
    HWND library_ = nullptr;
    HWND import_ = nullptr;
    HWND export_ = nullptr;
    HWND sound_ = nullptr;
    HWND renameEdit_ = nullptr;
    HWND displayMode_ = nullptr;
    HWND dropdownList_ = nullptr;
    HWND activeStatus_ = nullptr;
    HWND activeList_ = nullptr;
    HWND exportSelectAll_ = nullptr;
    HWND exportClearAll_ = nullptr;
    HWND exportConfirm_ = nullptr;
    HWND exportCancel_ = nullptr;
    HWND groupAll_ = nullptr;
    HWND groupFavorites_ = nullptr;
    HWND groupList_ = nullptr;
    HWND groupTooltip_ = nullptr;
    HWND groupCreate_ = nullptr;
    HWND groupRenameEdit_ = nullptr;
    std::vector<DisplayOption> displayOptions_;
    HFONT titleFont_ = nullptr;
    HFONT headingFont_ = nullptr;
    HFONT bodyFont_ = nullptr;
    HFONT smallFont_ = nullptr;
    HFONT badgeFont_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    HBRUSH panelBrush_ = nullptr;
    HBRUSH sidebarBrush_ = nullptr;
    std::vector<core::WallpaperItem> items_;
    std::vector<core::WallpaperGroup> groups_;
    std::vector<std::wstring> favorites_;
    std::vector<std::size_t> visibleIndices_;
    std::vector<std::size_t> activeVisibleIndices_;
    std::vector<ActiveWallpaperInfo> activeWallpapers_;
    std::wstring status_ = L"准备就绪";
    std::wstring resourceUsage_ = L"CPU --\tGPU --\n内存 --\t显存 --";
    std::wstring renamingPath_;
    std::wstring currentGroupId_ = std::wstring(AllGroupId);
    std::wstring renamingGroupId_;
    std::unordered_map<std::wstring, HBITMAP> thumbnails_;
    FilterKind filterKind_ = FilterKind::All;
    bool soundEnabled_ = false;
    bool spanAcrossDisplays_ = true;
    bool activeDrawerVisible_ = false;
    bool exportSelectionMode_ = false;
    DropdownKind dropdownKind_ = DropdownKind::None;
    std::vector<std::wstring> dropdownLabels_;
    std::vector<std::wstring> dropdownDescriptions_;
    std::unordered_map<HWND, float> hoverProgress_;
    std::unordered_map<HWND, bool> hoverTargets_;
    int libraryHoverIndex_ = -1;
    int activeHoverIndex_ = -1;
    int dropdownHoverIndex_ = -1;
    int groupHoverIndex_ = -1;
    int groupTooltipIndex_ = -1;
    int libraryBadgeHoverIndex_ = -1;
    int activeCancelHoverIndex_ = -1;
    float libraryHoverProgress_ = 0.0F;
    float activeHoverProgress_ = 0.0F;
    float dropdownHoverProgress_ = 0.0F;
    float groupHoverProgress_ = 0.0F;
    bool libraryHoverTarget_ = false;
    bool activeHoverTarget_ = false;
    bool dropdownHoverTarget_ = false;
    bool groupHoverTarget_ = false;
    std::unordered_set<std::wstring> exportSelectedPaths_;
    POINT libraryDragStart_{};
    int libraryDragSourceVisibleIndex_ = -1;
    int libraryDragTargetVisibleIndex_ = -1;
    int libraryDragScrollDirection_ = 0;
    bool libraryDragActive_ = false;
    bool libraryDragInsertAfter_ = false;
    std::optional<std::vector<core::WallpaperItem>> pendingLibraryOrder_;
    POINT groupDragStart_{};
    int groupDragSourceIndex_ = -1;
    int groupDragTargetIndex_ = -1;
    int groupDragScrollDirection_ = 0;
    bool groupDragActive_ = false;
    bool groupDragInsertAfter_ = false;
    std::optional<std::vector<std::wstring>> pendingGroupOrder_;
    mutable std::uint64_t libraryDrawCount_ = 0;

    void UpdateFilterSelectorText();
    void UpdateDisplayModeText();
};

}  // namespace lwe::app
