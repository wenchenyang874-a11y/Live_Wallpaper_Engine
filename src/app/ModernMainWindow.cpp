#include "app/ModernMainWindow.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <string>

#include <commctrl.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <wrl/client.h>

namespace lwe::app {
namespace {

constexpr COLORREF kBackground = RGB(17, 20, 27);
constexpr COLORREF kSidebar = RGB(22, 26, 35);
constexpr COLORREF kPanel = RGB(28, 33, 44);
constexpr COLORREF kPanelHover = RGB(35, 42, 56);
constexpr COLORREF kAccent = RGB(92, 124, 250);
constexpr COLORREF kAccentHover = RGB(112, 142, 255);
constexpr COLORREF kTextPrimary = RGB(241, 244, 250);
constexpr COLORREF kTextSecondary = RGB(158, 168, 188);
constexpr COLORREF kBorder = RGB(54, 63, 82);
constexpr COLORREF kDanger = RGB(222, 92, 106);

int Scale(const HWND window, const int value) {
    return MulDiv(value, GetDpiForWindow(window), 96);
}

void SetControlFont(const HWND control, const HFONT font) {
    if (control != nullptr && font != nullptr) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

void FillRectangle(const HDC context, const RECT& rectangle, const COLORREF color) {
    const HBRUSH brush = CreateSolidBrush(color);
    FillRect(context, &rectangle, brush);
    DeleteObject(brush);
}

void FillRoundedRectangle(const HDC context, const RECT& rectangle,
                          const COLORREF fill, const COLORREF outline,
                          const int radius, const int outlineWidth = 1) {
    const HBRUSH brush = CreateSolidBrush(fill);
    const HPEN pen = CreatePen(PS_SOLID, outlineWidth, outline);
    const HGDIOBJ oldBrush = SelectObject(context, brush);
    const HGDIOBJ oldPen = SelectObject(context, pen);
    RoundRect(context, rectangle.left, rectangle.top, rectangle.right, rectangle.bottom,
              radius, radius);
    SelectObject(context, oldPen);
    SelectObject(context, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void DrawTextLine(const HDC context, const std::wstring_view text, RECT rectangle,
                  const HFONT font, const COLORREF color, const UINT format) {
    const HGDIOBJ oldFont = SelectObject(context, font);
    SetBkMode(context, TRANSPARENT);
    SetTextColor(context, color);
    DrawTextW(context, text.data(), static_cast<int>(text.size()), &rectangle, format);
    SelectObject(context, oldFont);
}

std::wstring Lowercase(std::wstring value) {
    std::ranges::transform(value, value.begin(), [](const wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

std::wstring FormatBytes(const std::uint64_t bytes) {
    constexpr std::uint64_t mebibyte = 1024ULL * 1024ULL;
    if (bytes >= mebibyte) {
        const double value = static_cast<double>(bytes) / mebibyte;
        wchar_t output[32]{};
        swprintf_s(output, L"%.1f MiB", value);
        return output;
    }
    const std::uint64_t kibibytes = (bytes + 1023ULL) / 1024ULL;
    return std::to_wstring(kibibytes) + L" KiB";
}

}  // namespace

ModernMainWindow::~ModernMainWindow() {
    if (renameEdit_ != nullptr) {
        RemoveWindowSubclass(renameEdit_, &ModernMainWindow::RenameEditProcedure, 1);
    }
    ClearThumbnails();
    for (const HFONT font : {titleFont_, headingFont_, bodyFont_, smallFont_}) {
        if (font != nullptr) {
            DeleteObject(font);
        }
    }
    if (editBrush_ != nullptr) {
        DeleteObject(editBrush_);
    }
    if (panelBrush_ != nullptr) {
        DeleteObject(panelBrush_);
    }
}

bool ModernMainWindow::Create(const HWND parent, const HINSTANCE instance) {
    parent_ = parent;
    if (!IsWindow(parent_)) {
        return false;
    }

    const BOOL dark = TRUE;
    DwmSetWindowAttribute(parent_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    search_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                              0, 0, 1, 1, parent_,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(Search)),
                              instance, nullptr);
    library_ = CreateWindowExW(
        0, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED |
            LBS_NOINTEGRALHEIGHT,
        0, 0, 1, 1, parent_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(Library)), instance, nullptr);

    const auto createButton = [&](const int identifier, const wchar_t* text) {
        return CreateWindowExW(
            0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 1, 1,
            parent_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(identifier)), instance,
            nullptr);
    };
    filter_ = createButton(Filter, L"全部");
    filterStatic_ = createButton(FilterStatic, L"图片");
    filterGif_ = createButton(FilterGif, L"GIF");
    filterVideo_ = createButton(FilterVideo, L"视频");
    import_ = createButton(Import, L"＋  导入壁纸");
    export_ = createButton(Export, L"导出分享包");
    apply_ = createButton(Apply, L"应用到桌面");
    sound_ = createButton(Sound, L"声音：关闭");
    cancelApplication_ = createButton(CancelApplication, L"取消应用");
    displaySelector_ = createButton(DisplaySelector, L"显示位置：跨屏扩展  ▼");
    renameEdit_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 1, 1,
        parent_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(RenameCommit)),
        instance, nullptr);

    if (search_ == nullptr || filter_ == nullptr || filterStatic_ == nullptr ||
        filterGif_ == nullptr || filterVideo_ == nullptr || library_ == nullptr ||
        import_ == nullptr || export_ == nullptr || apply_ == nullptr ||
        sound_ == nullptr || cancelApplication_ == nullptr ||
        displaySelector_ == nullptr || renameEdit_ == nullptr) {
        return false;
    }

    SetWindowTheme(search_, L"DarkMode_Explorer", nullptr);
    SetWindowTheme(library_, L"DarkMode_Explorer", nullptr);
    SetWindowTheme(renameEdit_, L"DarkMode_Explorer", nullptr);
    SetWindowSubclass(renameEdit_, &ModernMainWindow::RenameEditProcedure, 1,
                      reinterpret_cast<DWORD_PTR>(this));
    SendMessageW(search_, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"搜索名称、格式或路径"));

    editBrush_ = CreateSolidBrush(kPanel);
    panelBrush_ = CreateSolidBrush(kPanel);
    RecreateFonts();
    Layout();
    return true;
}

void ModernMainWindow::RecreateFonts() {
    for (HFONT* font : {&titleFont_, &headingFont_, &bodyFont_, &smallFont_}) {
        if (*font != nullptr) {
            DeleteObject(*font);
            *font = nullptr;
        }
    }
    const int dpi = static_cast<int>(GetDpiForWindow(parent_));
    titleFont_ = CreateFontW(-MulDiv(30, dpi, 96), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
                             FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                             L"Segoe UI Variable Display");
    headingFont_ = CreateFontW(-MulDiv(17, dpi, 96), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
                               FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                               L"Segoe UI Variable Text");
    bodyFont_ = CreateFontW(-MulDiv(14, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                            FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                            L"Segoe UI Variable Text");
    smallFont_ = CreateFontW(-MulDiv(12, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                             FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                             L"Segoe UI Variable Text");

    for (const HWND control : {search_, filter_, filterStatic_, filterGif_,
                               filterVideo_, library_, import_, export_, apply_,
                               sound_, cancelApplication_, displaySelector_,
                               renameEdit_}) {
        SetControlFont(control, bodyFont_);
    }
    SendMessageW(library_, LB_SETITEMHEIGHT, 0, Scale(parent_, 78));
}

void ModernMainWindow::Layout() {
    if (!IsWindow(parent_)) {
        return;
    }
    RECT client{};
    GetClientRect(parent_, &client);
    const int width = client.right;
    const int height = client.bottom;
    const int sidebar = Scale(parent_, 216);
    const int margin = Scale(parent_, 30);
    const int gap = Scale(parent_, 12);
    const int contentLeft = sidebar + margin;
    const int contentWidth = std::max(1, width - contentLeft - margin);
    const int searchTop = Scale(parent_, 104);
    const int controlHeight = Scale(parent_, 40);
    const int filterWidth = Scale(parent_, 282);
    const int searchWidth = std::max(1, contentWidth - filterWidth - gap);

    MoveWindow(search_, contentLeft, searchTop, searchWidth, controlHeight, TRUE);
    const int filterLeft = contentLeft + searchWidth + gap;
    const int segmentGap = Scale(parent_, 4);
    const int segmentWidth = std::max(1, (filterWidth - segmentGap * 3) / 4);
    MoveWindow(filter_, filterLeft, searchTop, segmentWidth, controlHeight, TRUE);
    MoveWindow(filterStatic_, filterLeft + segmentWidth + segmentGap, searchTop,
               segmentWidth, controlHeight, TRUE);
    MoveWindow(filterGif_, filterLeft + (segmentWidth + segmentGap) * 2, searchTop,
               segmentWidth, controlHeight, TRUE);
    MoveWindow(filterVideo_, filterLeft + (segmentWidth + segmentGap) * 3, searchTop,
               filterWidth - (segmentWidth + segmentGap) * 3, controlHeight, TRUE);

    const int actionTop = searchTop + controlHeight + gap;
    const int actionWidth = std::max(1, (contentWidth - gap * 2) / 3);
    MoveWindow(import_, contentLeft, actionTop, actionWidth, controlHeight, TRUE);
    MoveWindow(export_, contentLeft + actionWidth + gap, actionTop, actionWidth,
               controlHeight, TRUE);
    MoveWindow(apply_, contentLeft + (actionWidth + gap) * 2, actionTop,
               contentWidth - (actionWidth + gap) * 2, controlHeight, TRUE);

    const int displayTop = actionTop + controlHeight + gap;
    MoveWindow(displaySelector_, contentLeft, displayTop, contentWidth,
               controlHeight, TRUE);

    const int statusHeight = Scale(parent_, 62);
    const int listTop = displayTop + controlHeight + Scale(parent_, 16);
    const int listBottom = height - margin - statusHeight;
    MoveWindow(library_, contentLeft, listTop, contentWidth,
               std::max(1, listBottom - listTop - gap), TRUE);

    const int sidebarButtonWidth = sidebar - Scale(parent_, 32);
    const int sidebarButtonLeft = Scale(parent_, 16);
    const int sidebarButtonHeight = Scale(parent_, 40);
    MoveWindow(sound_, sidebarButtonLeft, height - Scale(parent_, 56),
               sidebarButtonWidth, sidebarButtonHeight, TRUE);

    const int statusTop = height - Scale(parent_, 82);
    const int resourceWidth = std::clamp(contentWidth * 42 / 100,
                                         Scale(parent_, 260),
                                         Scale(parent_, 390));
    const int statusRight = contentLeft + contentWidth - resourceWidth - gap;
    const int cancelWidth = Scale(parent_, 96);
    const int statusCardHeight = Scale(parent_, 54);
    const int cancelHeight = Scale(parent_, 40);
    MoveWindow(cancelApplication_, statusRight - cancelWidth - Scale(parent_, 8),
               statusTop + (statusCardHeight - cancelHeight) / 2, cancelWidth,
               cancelHeight, TRUE);
    InvalidateRect(parent_, nullptr, FALSE);
}

void ModernMainWindow::DpiChanged() {
    RecreateFonts();
    Layout();
}

void ModernMainWindow::Paint(const HDC deviceContext, const RECT&) const {
    RECT client{};
    GetClientRect(parent_, &client);
    FillRectangle(deviceContext, client, kBackground);

    RECT sidebar{0, 0, Scale(parent_, 216), client.bottom};
    FillRectangle(deviceContext, sidebar, kSidebar);

    RECT logo{Scale(parent_, 22), Scale(parent_, 24), Scale(parent_, 62),
              Scale(parent_, 64)};
    FillRoundedRectangle(deviceContext, logo, kAccent, kAccent, Scale(parent_, 14));
    DrawTextLine(deviceContext, L"L", logo, headingFont_, RGB(255, 255, 255),
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT brand{Scale(parent_, 74), Scale(parent_, 23), Scale(parent_, 202),
               Scale(parent_, 66)};
    DrawTextLine(deviceContext, L"Live Wallpaper", brand, headingFont_, kTextPrimary,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT navigation{Scale(parent_, 16), Scale(parent_, 92), Scale(parent_, 200),
                    Scale(parent_, 138)};
    FillRoundedRectangle(deviceContext, navigation, RGB(38, 45, 62), RGB(38, 45, 62),
                         Scale(parent_, 12));
    RECT navigationText = navigation;
    navigationText.left += Scale(parent_, 18);
    DrawTextLine(deviceContext, L"我的壁纸", navigationText, bodyFont_, kTextPrimary,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT title{Scale(parent_, 246), Scale(parent_, 28), client.right - Scale(parent_, 30),
               Scale(parent_, 72)};
    DrawTextLine(deviceContext, L"我的壁纸", title, titleFont_, kTextPrimary,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT subtitle{Scale(parent_, 248), Scale(parent_, 70),
                  client.right - Scale(parent_, 30), Scale(parent_, 98)};
    const std::wstring countText = L"本地、安全地管理与分享 · " +
                                   std::to_wstring(visibleIndices_.size()) + L" 项";
    DrawTextLine(deviceContext, countText, subtitle, smallFont_, kTextSecondary,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const int contentLeft = Scale(parent_, 246);
    const int contentRight = client.right - Scale(parent_, 30);
    const int contentWidth = std::max(1, contentRight - contentLeft);
    const int gap = Scale(parent_, 12);
    const int resourceWidth = std::clamp(contentWidth * 42 / 100,
                                         Scale(parent_, 260),
                                         Scale(parent_, 390));
    RECT statusCard{contentLeft, client.bottom - Scale(parent_, 82),
                    contentRight - resourceWidth - gap,
                    client.bottom - Scale(parent_, 28)};
    FillRoundedRectangle(deviceContext, statusCard, kPanel, kBorder, Scale(parent_, 12));
    RECT statusText = statusCard;
    statusText.left += Scale(parent_, 16);
    statusText.right -= Scale(parent_, 116);
    DrawTextLine(deviceContext, status_, statusText, smallFont_, kTextSecondary,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT resourceCard{statusCard.right + gap, statusCard.top, contentRight,
                      statusCard.bottom};
    FillRoundedRectangle(deviceContext, resourceCard, kPanel, kBorder,
                         Scale(parent_, 12));
    RECT resourceText = resourceCard;
    resourceText.left += Scale(parent_, 12);
    resourceText.right -= Scale(parent_, 12);
    DrawTextLine(deviceContext, resourceUsage_, resourceText, smallFont_,
                 kTextSecondary,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

bool ModernMainWindow::DrawItem(const DRAWITEMSTRUCT& draw) const {
    if (draw.CtlID == Library) {
        DrawLibraryItem(draw);
        return true;
    }
    if (draw.CtlType == ODT_BUTTON) {
        DrawButton(draw);
        return true;
    }
    return false;
}

void ModernMainWindow::DrawButton(const DRAWITEMSTRUCT& draw) const {
    wchar_t text[128]{};
    GetWindowTextW(draw.hwndItem, text, static_cast<int>(std::size(text)));
    const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
    const bool disabled = (draw.itemState & ODS_DISABLED) != 0;
    COLORREF fill = kPanel;
    COLORREF outline = kBorder;
    COLORREF foreground = disabled ? RGB(90, 98, 114) : kTextPrimary;
    const bool filterButton =
        draw.CtlID == Filter || draw.CtlID == FilterStatic ||
        draw.CtlID == FilterGif || draw.CtlID == FilterVideo;
    const bool selectedFilter =
        (draw.CtlID == Filter && filterKind_ == FilterKind::All) ||
        (draw.CtlID == FilterStatic && filterKind_ == FilterKind::StaticImage) ||
        (draw.CtlID == FilterGif && filterKind_ == FilterKind::AnimatedGif) ||
        (draw.CtlID == FilterVideo && filterKind_ == FilterKind::Video);
    if (filterButton && selectedFilter) {
        fill = pressed ? RGB(72, 99, 207) : RGB(47, 62, 108);
        outline = kAccent;
        foreground = RGB(210, 219, 255);
    } else if (draw.CtlID == Apply || draw.CtlID == Import) {
        fill = pressed ? RGB(72, 99, 207) : kAccent;
        outline = fill;
    } else if (draw.CtlID == CancelApplication) {
        foreground = disabled ? foreground : kDanger;
    } else if (pressed) {
        fill = kPanelHover;
    }
    FillRectangle(draw.hDC, draw.rcItem, kBackground);
    RECT button = draw.rcItem;
    InflateRect(&button, -1, -1);
    FillRoundedRectangle(draw.hDC, button, fill, outline, Scale(parent_, 10));
    DrawTextLine(draw.hDC, text, button, bodyFont_, foreground,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void ModernMainWindow::DrawLibraryItem(const DRAWITEMSTRUCT& draw) const {
    FillRectangle(draw.hDC, draw.rcItem, kBackground);
    if (draw.itemID == static_cast<UINT>(-1) ||
        draw.itemID >= visibleIndices_.size()) {
        return;
    }
    const core::WallpaperItem& item = items_[visibleIndices_[draw.itemID]];
    const bool selected = (draw.itemState & ODS_SELECTED) != 0;
    const bool active = _wcsicmp(item.path.c_str(), activePath_.c_str()) == 0;

    RECT card = draw.rcItem;
    InflateRect(&card, -Scale(parent_, 4), -Scale(parent_, 5));
    FillRoundedRectangle(draw.hDC, card, selected ? kPanelHover : kPanel,
                         active ? kAccent : kBorder, Scale(parent_, 12),
                         active ? 2 : 1);

    RECT icon{card.left + Scale(parent_, 12), card.top + Scale(parent_, 9),
              card.left + Scale(parent_, 94), card.bottom - Scale(parent_, 9)};
    FillRoundedRectangle(draw.hDC, icon, RGB(10, 12, 17), kBorder,
                         Scale(parent_, 8));
    const auto thumbnail = thumbnails_.find(item.path.native());
    if (thumbnail != thumbnails_.end() && thumbnail->second != nullptr) {
        BITMAP bitmap{};
        if (GetObjectW(thumbnail->second, sizeof(bitmap), &bitmap) == sizeof(bitmap) &&
            bitmap.bmWidth > 0 && bitmap.bmHeight > 0) {
            const int boxWidth = icon.right - icon.left - Scale(parent_, 4);
            const int boxHeight = icon.bottom - icon.top - Scale(parent_, 4);
            const double scale = std::min(
                static_cast<double>(boxWidth) / bitmap.bmWidth,
                static_cast<double>(boxHeight) / bitmap.bmHeight);
            const int drawWidth = std::max(1, static_cast<int>(bitmap.bmWidth * scale));
            const int drawHeight =
                std::max(1, static_cast<int>(bitmap.bmHeight * scale));
            const int drawLeft = icon.left + (icon.right - icon.left - drawWidth) / 2;
            const int drawTop = icon.top + (icon.bottom - icon.top - drawHeight) / 2;
            const HDC source = CreateCompatibleDC(draw.hDC);
            const HGDIOBJ previous = SelectObject(source, thumbnail->second);
            SetStretchBltMode(draw.hDC, HALFTONE);
            StretchBlt(draw.hDC, drawLeft, drawTop, drawWidth, drawHeight,
                       source, 0, 0, bitmap.bmWidth, bitmap.bmHeight, SRCCOPY);
            SelectObject(source, previous);
            DeleteDC(source);
        }
    }

    RECT name{icon.right + Scale(parent_, 14), card.top + Scale(parent_, 9),
              card.right - Scale(parent_, 120), card.top + Scale(parent_, 38)};
    DrawTextLine(draw.hDC, item.displayName, name, bodyFont_, kTextPrimary,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    std::wstring details = item.formatLabel;
    if (item.width > 0 && item.height > 0) {
        details += L"  ·  " + std::to_wstring(item.width) + L"×" +
                   std::to_wstring(item.height);
    }
    details += L"  ·  " + FormatBytes(item.fileSize);
    if (item.kind == media::WallpaperKind::Video) {
        details += item.hasAudio ? L"  ·  含音轨" : L"  ·  无音轨";
    }
    RECT detail{name.left, card.top + Scale(parent_, 37),
                card.right - Scale(parent_, 20), card.bottom - Scale(parent_, 7)};
    DrawTextLine(draw.hDC, details, detail, smallFont_, kTextSecondary,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (active) {
        RECT badge{card.right - Scale(parent_, 92), card.top + Scale(parent_, 20),
                   card.right - Scale(parent_, 14), card.top + Scale(parent_, 48)};
        FillRoundedRectangle(draw.hDC, badge, RGB(47, 62, 108), RGB(47, 62, 108),
                             Scale(parent_, 9));
        DrawTextLine(draw.hDC, L"使用中", badge, smallFont_, RGB(190, 204, 255),
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

HBRUSH ModernMainWindow::ColorControl(const HDC deviceContext, const HWND control) const {
    SetBkMode(deviceContext, TRANSPARENT);
    if (control == search_ || control == renameEdit_) {
        SetTextColor(deviceContext, kTextPrimary);
        SetBkColor(deviceContext, kPanel);
        return editBrush_;
    }
    SetTextColor(deviceContext, kTextPrimary);
    SetBkColor(deviceContext, kPanel);
    return panelBrush_;
}

bool ModernMainWindow::HandleFilterCommand(const WORD controlId,
                                           const WORD notificationCode) {
    if (controlId == Search && notificationCode == EN_CHANGE) {
        RefreshVisibleItems();
        return true;
    }
    if (notificationCode == BN_CLICKED &&
        (controlId == Filter || controlId == FilterStatic ||
         controlId == FilterGif || controlId == FilterVideo)) {
        if (controlId == Filter) {
            filterKind_ = FilterKind::All;
        } else if (controlId == FilterStatic) {
            filterKind_ = FilterKind::StaticImage;
        } else if (controlId == FilterGif) {
            filterKind_ = FilterKind::AnimatedGif;
        } else {
            filterKind_ = FilterKind::Video;
        }
        for (const HWND control : {filter_, filterStatic_, filterGif_, filterVideo_}) {
            InvalidateRect(control, nullptr, FALSE);
        }
        RefreshVisibleItems();
        return true;
    }
    return false;
}

bool ModernMainWindow::ShowDisplaySelectorMenu() {
    if (displayOptions_.empty() || !IsWindow(displaySelector_)) {
        return false;
    }
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return false;
    }
    constexpr UINT kSpanCommand = 1;
    constexpr UINT kDisplayCommandBase = 100;
    AppendMenuW(menu, MF_STRING | (spanAcrossDisplays_ ? MF_CHECKED : 0),
                kSpanCommand, L"跨屏扩展（一个画面横跨全部屏幕）");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    for (std::size_t index = 0; index < displayOptions_.size(); ++index) {
        const DisplayOption& option = displayOptions_[index];
        const UINT flags = MF_STRING |
                           (!spanAcrossDisplays_ && option.selected ? MF_CHECKED : 0);
        AppendMenuW(menu, flags, kDisplayCommandBase + static_cast<UINT>(index),
                    option.label.c_str());
    }

    RECT selector{};
    GetWindowRect(displaySelector_, &selector);
    const UINT command = TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN,
        selector.left, selector.bottom, 0, parent_, nullptr);
    DestroyMenu(menu);
    if (command == 0) {
        return false;
    }
    if (command == kSpanCommand) {
        spanAcrossDisplays_ = true;
    } else if (command >= kDisplayCommandBase &&
               command < kDisplayCommandBase + displayOptions_.size()) {
        spanAcrossDisplays_ = false;
        DisplayOption& selected =
            displayOptions_[command - kDisplayCommandBase];
        selected.selected = !selected.selected;
        if (std::ranges::none_of(displayOptions_, [](const DisplayOption& option) {
                return option.selected;
            })) {
            selected.selected = true;
        }
    } else {
        return false;
    }
    UpdateDisplaySelectorText();
    return true;
}

void ModernMainWindow::SetItems(std::vector<core::WallpaperItem> items) {
    items_ = std::move(items);
    for (const core::WallpaperItem& item : items_) {
        const std::wstring key = item.path.native();
        if (!thumbnails_.contains(key)) {
            thumbnails_.emplace(key, LoadThumbnail(key));
        }
    }
    RefreshVisibleItems();
}

void ModernMainWindow::SetActivePath(const std::wstring_view path) {
    activePath_.assign(path);
    EnableWindow(cancelApplication_, !activePath_.empty());
    InvalidateRect(library_, nullptr, FALSE);
}

void ModernMainWindow::SetStatus(std::wstring status) {
    if (status_ == status) {
        return;
    }
    status_ = std::move(status);
    InvalidateFooter();
}

void ModernMainWindow::SetSoundEnabled(const bool enabled) {
    soundEnabled_ = enabled;
    SetWindowTextW(sound_, enabled ? L"声音：开启" : L"声音：关闭");
    InvalidateRect(sound_, nullptr, FALSE);
}

void ModernMainWindow::SetDisplayOptions(std::vector<DisplayOption> displays,
                                         const bool spanDisplays) {
    displayOptions_ = std::move(displays);
    spanAcrossDisplays_ = spanDisplays;
    if (!displayOptions_.empty() &&
        std::ranges::none_of(displayOptions_, [](const DisplayOption& option) {
            return option.selected;
        })) {
        displayOptions_.front().selected = true;
    }

    UpdateDisplaySelectorText();
    Layout();
}

void ModernMainWindow::UpdateDisplaySelectorText() {
    std::wstring text = L"显示位置：";
    if (spanAcrossDisplays_) {
        text += L"跨屏扩展";
    } else {
        bool first = true;
        for (std::size_t index = 0; index < displayOptions_.size(); ++index) {
            if (!displayOptions_[index].selected) {
                continue;
            }
            if (!first) {
                text += L"、";
            }
            text += L"屏幕 " + std::to_wstring(index + 1U);
            first = false;
        }
    }
    text += L"  ▼";
    SetWindowTextW(displaySelector_, text.c_str());
    InvalidateRect(displaySelector_, nullptr, FALSE);
}

void ModernMainWindow::SetResourceUsage(std::wstring usage) {
    if (resourceUsage_ == usage) {
        return;
    }
    resourceUsage_ = std::move(usage);
    InvalidateFooter();
}

void ModernMainWindow::InvalidateFooter() const {
    if (parent_ == nullptr) {
        return;
    }
    RECT client{};
    if (!GetClientRect(parent_, &client)) {
        return;
    }
    RECT footer{Scale(parent_, 246), client.bottom - Scale(parent_, 84),
                client.right - Scale(parent_, 28),
                client.bottom - Scale(parent_, 26)};
    InvalidateRect(parent_, &footer, FALSE);
}

std::vector<std::wstring> ModernMainWindow::SelectedDisplayIds() const {
    std::vector<std::wstring> selected;
    for (const DisplayOption& option : displayOptions_) {
        if (option.selected) {
            selected.push_back(option.id);
        }
    }
    return selected;
}

bool ModernMainWindow::SpanAcrossDisplays() const noexcept {
    return spanAcrossDisplays_;
}

std::optional<core::WallpaperItem> ModernMainWindow::SelectedItem() const {
    const LRESULT selection = SendMessageW(library_, LB_GETCURSEL, 0, 0);
    if (selection < 0 || static_cast<std::size_t>(selection) >= visibleIndices_.size()) {
        return std::nullopt;
    }
    return items_[visibleIndices_[static_cast<std::size_t>(selection)]];
}

bool ModernMainWindow::SelectItemAtScreenPoint(const POINT screenPoint) {
    POINT clientPoint = screenPoint;
    if (!ScreenToClient(library_, &clientPoint)) {
        return false;
    }
    const LRESULT itemFromPoint = SendMessageW(
        library_, LB_ITEMFROMPOINT, 0, MAKELPARAM(clientPoint.x, clientPoint.y));
    const UINT index = LOWORD(itemFromPoint);
    const bool outside = HIWORD(itemFromPoint) != 0;
    if (outside || index >= visibleIndices_.size()) {
        return false;
    }
    SendMessageW(library_, LB_SETCURSEL, index, 0);
    InvalidateRect(library_, nullptr, FALSE);
    return true;
}

void ModernMainWindow::BeginRenameSelected() {
    const LRESULT selection = SendMessageW(library_, LB_GETCURSEL, 0, 0);
    if (selection < 0 ||
        static_cast<std::size_t>(selection) >= visibleIndices_.size()) {
        return;
    }
    const core::WallpaperItem& item =
        items_[visibleIndices_[static_cast<std::size_t>(selection)]];
    RECT itemRectangle{};
    if (SendMessageW(library_, LB_GETITEMRECT, selection,
                     reinterpret_cast<LPARAM>(&itemRectangle)) == LB_ERR) {
        return;
    }
    MapWindowPoints(library_, parent_, reinterpret_cast<POINT*>(&itemRectangle), 2);
    const int left = itemRectangle.left + Scale(parent_, 108);
    const int top = itemRectangle.top + Scale(parent_, 14);
    const int right = itemRectangle.right - Scale(parent_, 122);
    renamingPath_ = item.path.native();
    SetWindowTextW(renameEdit_, item.path.stem().native().c_str());
    SetWindowPos(renameEdit_, HWND_TOP, left, top, std::max(80, right - left),
                 Scale(parent_, 32), SWP_SHOWWINDOW);
    SetFocus(renameEdit_);
    SendMessageW(renameEdit_, EM_SETSEL, 0, -1);
}

std::optional<std::pair<core::WallpaperItem, std::wstring>>
ModernMainWindow::FinishRename() {
    if (renamingPath_.empty() || !IsWindowVisible(renameEdit_)) {
        return std::nullopt;
    }
    wchar_t name[260]{};
    GetWindowTextW(renameEdit_, name, static_cast<int>(std::size(name)));
    const auto item = std::ranges::find_if(items_, [&](const core::WallpaperItem& value) {
        return _wcsicmp(value.path.c_str(), renamingPath_.c_str()) == 0;
    });
    ShowWindow(renameEdit_, SW_HIDE);
    renamingPath_.clear();
    if (item == items_.end() || name[0] == L'\0') {
        return std::nullopt;
    }
    return std::pair{*item, std::wstring(name)};
}

HWND ModernMainWindow::SearchControl() const noexcept {
    return search_;
}

HWND ModernMainWindow::LibraryControl() const noexcept {
    return library_;
}

HBITMAP ModernMainWindow::LoadThumbnail(const std::wstring_view path) const {
    Microsoft::WRL::ComPtr<IShellItem> item;
    const std::wstring filePath(path);
    HRESULT result = SHCreateItemFromParsingName(filePath.c_str(), nullptr,
                                                 IID_PPV_ARGS(&item));
    Microsoft::WRL::ComPtr<IShellItemImageFactory> imageFactory;
    if (SUCCEEDED(result)) {
        result = item.As(&imageFactory);
    }
    HBITMAP bitmap = nullptr;
    const SIZE size{Scale(parent_, 160), Scale(parent_, 90)};
    if (SUCCEEDED(result)) {
        result = imageFactory->GetImage(
            size, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &bitmap);
    }
    if (FAILED(result) && imageFactory) {
        imageFactory->GetImage(size, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK,
                               &bitmap);
    }
    return bitmap;
}

void ModernMainWindow::ClearThumbnails() {
    for (const auto& [path, bitmap] : thumbnails_) {
        static_cast<void>(path);
        if (bitmap != nullptr) {
            DeleteObject(bitmap);
        }
    }
    thumbnails_.clear();
}

void ModernMainWindow::CancelRename() {
    ShowWindow(renameEdit_, SW_HIDE);
    renamingPath_.clear();
    SetFocus(library_);
}

LRESULT CALLBACK ModernMainWindow::RenameEditProcedure(
    const HWND window, const UINT message, const WPARAM wParam, const LPARAM lParam,
    const UINT_PTR, const DWORD_PTR referenceData) {
    auto* self = reinterpret_cast<ModernMainWindow*>(referenceData);
    if (self != nullptr && message == WM_KEYDOWN) {
        if (wParam == VK_RETURN) {
            PostMessageW(self->parent_, WM_COMMAND,
                         MAKEWPARAM(RenameCommit, BN_CLICKED),
                         reinterpret_cast<LPARAM>(window));
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            self->CancelRename();
            return 0;
        }
    }
    if (message == WM_GETDLGCODE) {
        return DLGC_WANTALLKEYS;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

void ModernMainWindow::RefreshVisibleItems() {
    wchar_t searchText[512]{};
    GetWindowTextW(search_, searchText, static_cast<int>(std::size(searchText)));
    const std::wstring search = Lowercase(searchText);

    visibleIndices_.clear();
    SendMessageW(library_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(library_, LB_RESETCONTENT, 0, 0);
    for (std::size_t index = 0; index < items_.size(); ++index) {
        if (!MatchesFilter(items_[index], search)) {
            continue;
        }
        const LRESULT itemIndex = SendMessageW(library_, LB_ADDSTRING, 0,
                                               reinterpret_cast<LPARAM>(L""));
        if (itemIndex >= 0) {
            visibleIndices_.push_back(index);
        }
    }
    SendMessageW(library_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(library_, nullptr, TRUE);
    InvalidateRect(parent_, nullptr, FALSE);
}

bool ModernMainWindow::MatchesFilter(const core::WallpaperItem& item,
                                     const std::wstring_view search) const {
    const bool kindMatches =
        filterKind_ == FilterKind::All ||
        (filterKind_ == FilterKind::StaticImage &&
         item.kind == media::WallpaperKind::StaticImage) ||
        (filterKind_ == FilterKind::AnimatedGif &&
         item.kind == media::WallpaperKind::AnimatedGif) ||
        (filterKind_ == FilterKind::Video && item.kind == media::WallpaperKind::Video);
    if (!kindMatches || search.empty()) {
        return kindMatches;
    }

    std::wstring searchable = item.displayName + L" " + item.formatLabel + L" " +
                              item.path.native();
    searchable = Lowercase(std::move(searchable));
    return searchable.find(search) != std::wstring::npos;
}

}  // namespace lwe::app
