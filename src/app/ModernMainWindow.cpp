#include "app/ModernMainWindow.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <string>
#include <utility>

#include <commctrl.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <wrl/client.h>
#include <windowsx.h>

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
constexpr wchar_t kScreenSelectionWindowClass[] =
    L"LiveWallpaperEngine.ScreenSelection";
constexpr int kScreenSelectionList = 3100;
constexpr int kScreenSelectionApply = 3101;
constexpr int kScreenSelectionCancel = 3102;
constexpr wchar_t kImportChoiceWindowClass[] =
    L"LiveWallpaperEngine.ImportChoice";
constexpr int kImportChoiceMedia = 3200;
constexpr int kImportChoicePackage = 3201;

COLORREF BlendColor(const COLORREF from, const COLORREF to, const float amount) {
    const float value = std::clamp(amount, 0.0F, 1.0F);
    const auto channel = [value](const BYTE start, const BYTE end) {
        return static_cast<BYTE>(std::lround(
            static_cast<float>(start) +
            (static_cast<float>(end) - static_cast<float>(start)) * value));
    };
    return RGB(channel(GetRValue(from), GetRValue(to)),
               channel(GetGValue(from), GetGValue(to)),
               channel(GetBValue(from), GetBValue(to)));
}

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

RECT SearchPanelRectangle(const HWND parent) {
    RECT client{};
    GetClientRect(parent, &client);
    const int contentLeft = Scale(parent, 246);
    const int contentRight = client.right - Scale(parent, 30);
    const int gap = Scale(parent, 12);
    const int filterWidth = Scale(parent, 166);
    const int width = std::max(1, contentRight - contentLeft - filterWidth - gap);
    const int top = Scale(parent, 28);
    return RECT{contentLeft, top, contentLeft + width, top + Scale(parent, 40)};
}

int FontPixelHeight(const HWND window, const HFONT font) {
    const HDC context = GetDC(window);
    if (context == nullptr) {
        return Scale(window, 17);
    }
    const HGDIOBJ previous = SelectObject(context, font);
    TEXTMETRICW metrics{};
    const bool measured = GetTextMetricsW(context, &metrics) != FALSE;
    SelectObject(context, previous);
    ReleaseDC(window, context);
    return measured ? metrics.tmHeight : Scale(window, 17);
}

struct ScreenSelectionDialogState final {
    HINSTANCE instance = nullptr;
    HWND owner = nullptr;
    HWND window = nullptr;
    HWND list = nullptr;
    HWND apply = nullptr;
    HWND cancel = nullptr;
    const std::vector<ModernMainWindow::DisplayOption>* options = nullptr;
    std::vector<std::wstring> result;
    HFONT headingFont = nullptr;
    HFONT bodyFont = nullptr;
    HFONT detailFont = nullptr;
    HBRUSH panelBrush = nullptr;
    bool replaceSelected = false;
    bool accepted = false;
    bool complete = false;
};

void RecreateScreenSelectionFonts(ScreenSelectionDialogState& state) {
    if (state.headingFont != nullptr) {
        DeleteObject(state.headingFont);
    }
    if (state.bodyFont != nullptr) {
        DeleteObject(state.bodyFont);
    }
    if (state.detailFont != nullptr) {
        DeleteObject(state.detailFont);
    }
    const int dpi = static_cast<int>(GetDpiForWindow(state.window));
    state.headingFont = CreateFontW(
        -MulDiv(18, dpi, 96), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Text");
    state.bodyFont = CreateFontW(
        -MulDiv(14, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Text");
    state.detailFont = CreateFontW(
        -MulDiv(12, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Text");
    for (const HWND control : {state.list, state.apply, state.cancel}) {
        SetControlFont(control, state.bodyFont);
    }
}

int ScreenSelectionItemHeight(const HWND window,
                              const ModernMainWindow::DisplayOption& option) {
    return Scale(window, option.Occupied() ? 78 : 56);
}

void UpdateScreenSelectionAction(ScreenSelectionDialogState& state) {
    if (!IsWindow(state.list) || !IsWindow(state.apply) || state.options == nullptr) {
        return;
    }
    bool hasSelection = false;
    bool replacesWallpaper = false;
    for (std::size_t index = 0; index < state.options->size(); ++index) {
        if (SendMessageW(state.list, LB_GETSEL, index, 0) <= 0) {
            continue;
        }
        hasSelection = true;
        replacesWallpaper =
            replacesWallpaper || (*state.options)[index].Occupied();
    }
    state.replaceSelected = replacesWallpaper;
    SetWindowTextW(state.apply, replacesWallpaper ? L"替换壁纸到屏幕"
                                                  : L"应用到所选屏幕");
    EnableWindow(state.apply, hasSelection ? TRUE : FALSE);
    InvalidateRect(state.apply, nullptr, FALSE);
}

void LayoutScreenSelectionDialog(ScreenSelectionDialogState& state) {
    RECT client{};
    GetClientRect(state.window, &client);
    const int margin = Scale(state.window, 24);
    const int listTop = Scale(state.window, 70);
    const int footerHeight = Scale(state.window, 64);
    const int gap = Scale(state.window, 10);
    const int cancelWidth = Scale(state.window, 104);
    const int applyWidth = Scale(state.window, 166);
    const int buttonHeight = Scale(state.window, 38);
    MoveWindow(state.list, margin, listTop,
               static_cast<int>(client.right) - margin * 2,
               std::max(1, static_cast<int>(client.bottom) - listTop -
                               footerHeight),
               TRUE);
    MoveWindow(state.cancel,
               client.right - margin - applyWidth - gap - cancelWidth,
               client.bottom - margin - buttonHeight, cancelWidth, buttonHeight,
               TRUE);
    MoveWindow(state.apply, client.right - margin - applyWidth,
               client.bottom - margin - buttonHeight, applyWidth, buttonHeight,
               TRUE);
}

void DrawScreenSelectionItem(const DRAWITEMSTRUCT& draw,
                             const ScreenSelectionDialogState& state) {
    FillRectangle(draw.hDC, draw.rcItem, kBackground);
    if (draw.itemID == static_cast<UINT>(-1) || state.options == nullptr ||
        draw.itemID >= state.options->size()) {
        return;
    }
    const ModernMainWindow::DisplayOption& option =
        (*state.options)[draw.itemID];
    const bool selected =
        SendMessageW(draw.hwndItem, LB_GETSEL, draw.itemID, 0) > 0;
    RECT card = draw.rcItem;
    InflateRect(&card, -Scale(state.window, 3), -Scale(state.window, 4));
    COLORREF fill = selected ? kPanelHover : kPanel;
    COLORREF outline = selected ? kAccent : kBorder;
    if (option.Occupied()) {
        fill = selected ? RGB(54, 36, 44) : RGB(34, 37, 53);
        outline = selected ? RGB(193, 74, 91) : RGB(92, 83, 139);
    }
    FillRoundedRectangle(draw.hDC, card, fill, outline, Scale(state.window, 10),
                         selected ? 2 : 1);

    const int boxSize = Scale(state.window, 20);
    RECT box{card.left + Scale(state.window, 14),
             card.top + (card.bottom - card.top - boxSize) / 2,
             card.left + Scale(state.window, 14) + boxSize,
             card.top + (card.bottom - card.top + boxSize) / 2};
    FillRoundedRectangle(draw.hDC, box, selected ? kAccent : kPanel,
                         selected ? kAccent : kTextSecondary,
                         Scale(state.window, 5));
    if (selected) {
        const HPEN pen = CreatePen(PS_SOLID, std::max(1, Scale(state.window, 2)),
                                   RGB(255, 255, 255));
        const HGDIOBJ previous = SelectObject(draw.hDC, pen);
        MoveToEx(draw.hDC, box.left + Scale(state.window, 5),
                 box.top + Scale(state.window, 10), nullptr);
        LineTo(draw.hDC, box.left + Scale(state.window, 9),
               box.top + Scale(state.window, 14));
        LineTo(draw.hDC, box.left + Scale(state.window, 16),
               box.top + Scale(state.window, 6));
        SelectObject(draw.hDC, previous);
        DeleteObject(pen);
    }

    RECT label = card;
    label.left = box.right + Scale(state.window, 14);
    label.right -= Scale(state.window, 12);
    if (option.Occupied()) {
        label.top += Scale(state.window, 5);
        label.bottom = label.top + Scale(state.window, 28);
        DrawTextLine(draw.hDC, option.label, label, state.bodyFont, kTextPrimary,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT detail = label;
        detail.top = label.bottom - Scale(state.window, 2);
        detail.bottom = card.bottom - Scale(state.window, 5);
        DrawTextLine(draw.hDC, L"正在使用：" + option.activeWallpaperName,
                     detail, state.detailFont,
                     selected ? RGB(244, 157, 169) : RGB(180, 172, 219),
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        DrawTextLine(draw.hDC, option.label, label, state.bodyFont, kTextPrimary,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

void DrawScreenSelectionButton(const DRAWITEMSTRUCT& draw,
                               const ScreenSelectionDialogState& state) {
    wchar_t text[64]{};
    GetWindowTextW(draw.hwndItem, text, static_cast<int>(std::size(text)));
    const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
    const bool disabled = (draw.itemState & ODS_DISABLED) != 0;
    COLORREF fill = kPanel;
    COLORREF outline = kBorder;
    if (draw.CtlID == kScreenSelectionApply) {
        if (disabled) {
            fill = RGB(39, 45, 57);
            outline = RGB(58, 67, 84);
        } else if (state.replaceSelected) {
            fill = pressed ? RGB(146, 46, 63) : RGB(190, 61, 82);
            outline = fill;
        } else {
            fill = pressed ? RGB(72, 99, 207) : kAccent;
            outline = fill;
        }
    } else if (pressed) {
        fill = kPanelHover;
    }
    FillRectangle(draw.hDC, draw.rcItem, kBackground);
    RECT button = draw.rcItem;
    InflateRect(&button, -1, -1);
    FillRoundedRectangle(draw.hDC, button, fill, outline,
                         Scale(state.window, 10));
    DrawTextLine(draw.hDC, text, button, state.bodyFont,
                 disabled ? RGB(121, 132, 153) : kTextPrimary,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

LRESULT CALLBACK ScreenSelectionWindowProcedure(const HWND window,
                                                const UINT message,
                                                const WPARAM wParam,
                                                const LPARAM lParam) {
    auto* state = reinterpret_cast<ScreenSelectionDialogState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = static_cast<ScreenSelectionDialogState*>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    switch (message) {
        case WM_CREATE: {
            state->list = CreateWindowExW(
                0, L"LISTBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                    LBS_HASSTRINGS | LBS_MULTIPLESEL | LBS_OWNERDRAWVARIABLE |
                    LBS_NOINTEGRALHEIGHT | LBS_NOTIFY,
                0, 0, 1, 1, window,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(kScreenSelectionList)),
                state->instance, nullptr);
            state->cancel = CreateWindowExW(
                0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                                BS_OWNERDRAW,
                0, 0, 1, 1, window,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(kScreenSelectionCancel)),
                state->instance, nullptr);
            state->apply = CreateWindowExW(
                0, L"BUTTON", L"应用到所选屏幕",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 1, 1,
                window,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(kScreenSelectionApply)),
                state->instance, nullptr);
            if (state->list == nullptr || state->apply == nullptr ||
                state->cancel == nullptr || state->options == nullptr) {
                return -1;
            }
            SetWindowTheme(state->list, L"DarkMode_Explorer", nullptr);
            state->panelBrush = CreateSolidBrush(kPanel);
            RecreateScreenSelectionFonts(*state);
            for (std::size_t index = 0; index < state->options->size(); ++index) {
                SendMessageW(state->list, LB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(
                                 (*state->options)[index].label.c_str()));
                if (!(*state->options)[index].Occupied()) {
                    SendMessageW(state->list, LB_SETSEL, TRUE, index);
                }
            }
            UpdateScreenSelectionAction(*state);
            LayoutScreenSelectionDialog(*state);
            return 0;
        }
        case WM_SIZE:
            LayoutScreenSelectionDialog(*state);
            return 0;
        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOACTIVATE | SWP_NOZORDER);
            RecreateScreenSelectionFonts(*state);
            for (std::size_t index = 0; index < state->options->size(); ++index) {
                SendMessageW(
                    state->list, LB_SETITEMHEIGHT, index,
                    ScreenSelectionItemHeight(window, (*state->options)[index]));
            }
            return 0;
        }
        case WM_COMMAND: {
            const int identifier = LOWORD(wParam);
            if (identifier == kScreenSelectionApply) {
                state->result.clear();
                for (std::size_t index = 0; index < state->options->size(); ++index) {
                    if (SendMessageW(state->list, LB_GETSEL, index, 0) > 0) {
                        state->result.push_back((*state->options)[index].id);
                    }
                }
                if (state->result.empty()) {
                    MessageBeep(MB_ICONWARNING);
                    return 0;
                }
                state->accepted = true;
                DestroyWindow(window);
                return 0;
            }
            if (identifier == kScreenSelectionCancel) {
                DestroyWindow(window);
                return 0;
            }
            if (identifier == kScreenSelectionList &&
                HIWORD(wParam) == LBN_SELCHANGE) {
                UpdateScreenSelectionAction(*state);
                InvalidateRect(state->list, nullptr, FALSE);
                return 0;
            }
            break;
        }
        case WM_MEASUREITEM: {
            auto& measure = *reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            if (measure.CtlID == kScreenSelectionList && state->options != nullptr) {
                const std::size_t index = measure.itemID;
                measure.itemHeight = index < state->options->size()
                                         ? ScreenSelectionItemHeight(
                                               window, (*state->options)[index])
                                         : Scale(window, 56);
                return TRUE;
            }
            break;
        }
        case WM_DRAWITEM: {
            const auto& draw = *reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (draw.CtlID == kScreenSelectionList) {
                DrawScreenSelectionItem(draw, *state);
                return TRUE;
            }
            if (draw.CtlID == kScreenSelectionApply ||
                draw.CtlID == kScreenSelectionCancel) {
                DrawScreenSelectionButton(draw, *state);
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLORLISTBOX:
            SetBkColor(reinterpret_cast<HDC>(wParam), kPanel);
            SetTextColor(reinterpret_cast<HDC>(wParam), kTextPrimary);
            return reinterpret_cast<LRESULT>(state->panelBrush);
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const HDC context = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            FillRectangle(context, client, kBackground);
            RECT title{Scale(window, 24), Scale(window, 12),
                       client.right - Scale(window, 24), Scale(window, 42)};
            DrawTextLine(context, L"应用到哪些屏幕？", title,
                         state->headingFont, kTextPrimary,
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            RECT subtitle{title.left, Scale(window, 40), title.right,
                          Scale(window, 66)};
            DrawTextLine(context, L"可以选择一个或多个屏幕", subtitle,
                         state->bodyFont, kTextSecondary,
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            state->complete = true;
            return 0;
        case WM_NCDESTROY:
            if (state->headingFont != nullptr) {
                DeleteObject(state->headingFont);
                state->headingFont = nullptr;
            }
            if (state->bodyFont != nullptr) {
                DeleteObject(state->bodyFont);
                state->bodyFont = nullptr;
            }
            if (state->detailFont != nullptr) {
                DeleteObject(state->detailFont);
                state->detailFont = nullptr;
            }
            if (state->panelBrush != nullptr) {
                DeleteObject(state->panelBrush);
                state->panelBrush = nullptr;
            }
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool RegisterScreenSelectionWindowClass(const HINSTANCE instance) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &ScreenSelectionWindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kScreenSelectionWindowClass;
    return RegisterClassExW(&windowClass) != 0 ||
           GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

std::optional<std::vector<std::wstring>> ShowScreenSelectionDialog(
    const HWND owner, const HINSTANCE instance,
    const std::vector<ModernMainWindow::DisplayOption>& options) {
    if (!IsWindow(owner) || options.empty() ||
        !RegisterScreenSelectionWindowClass(instance)) {
        return std::nullopt;
    }

    ScreenSelectionDialogState state;
    state.instance = instance;
    state.owner = owner;
    state.options = &options;
    const int visibleRows = std::clamp(static_cast<int>(options.size()), 1, 5);
    int visibleHeight = 0;
    for (int index = 0; index < visibleRows; ++index) {
        visibleHeight += options[static_cast<std::size_t>(index)].Occupied()
                             ? 78
                             : 56;
    }
    const int clientWidth = Scale(owner, 540);
    const int clientHeight = Scale(owner, 70 + visibleHeight + 64);
    RECT outer{0, 0, clientWidth, clientHeight};
    AdjustWindowRectExForDpi(&outer, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE,
                             WS_EX_DLGMODALFRAME, GetDpiForWindow(owner));
    RECT ownerRectangle{};
    GetWindowRect(owner, &ownerRectangle);
    const int width = outer.right - outer.left;
    const int height = outer.bottom - outer.top;
    const int left = ownerRectangle.left +
                     (ownerRectangle.right - ownerRectangle.left - width) / 2;
    const int top = ownerRectangle.top +
                    (ownerRectangle.bottom - ownerRectangle.top - height) / 2;
    const HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME, kScreenSelectionWindowClass, L"选择应用屏幕",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, left, top, width, height, owner,
        nullptr, instance, &state);
    if (dialog == nullptr) {
        return std::nullopt;
    }
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(dialog, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark,
                          sizeof(dark));

    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOWNORMAL);
    SetForegroundWindow(dialog);
    SetFocus(state.list);
    bool receivedQuit = false;
    WPARAM quitCode = 0;
    MSG message{};
    while (!state.complete && GetMessageW(&message, nullptr, 0, 0) > 0) {
        const bool belongsToDialog =
            message.hwnd == dialog || IsChild(dialog, message.hwnd);
        if (belongsToDialog && message.message == WM_KEYDOWN &&
            (message.wParam == VK_RETURN || message.wParam == VK_ESCAPE)) {
            PostMessageW(dialog, WM_COMMAND,
                         MAKEWPARAM(message.wParam == VK_RETURN
                                        ? kScreenSelectionApply
                                        : kScreenSelectionCancel,
                                    BN_CLICKED),
                         0);
            continue;
        }
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (message.message == WM_QUIT) {
        receivedQuit = true;
        quitCode = message.wParam;
    }
    if (IsWindow(dialog)) {
        DestroyWindow(dialog);
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (receivedQuit) {
        PostQuitMessage(static_cast<int>(quitCode));
    }
    if (!state.accepted) {
        return std::nullopt;
    }
    return state.result;
}

struct ImportChoiceDialogState final {
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    HWND media = nullptr;
    HWND package = nullptr;
    HFONT headingFont = nullptr;
    HFONT bodyFont = nullptr;
    HFONT detailFont = nullptr;
    int hoveredControl = 0;
    std::optional<ModernMainWindow::ImportChoice> result;
    bool complete = false;
};

void RecreateImportChoiceFonts(ImportChoiceDialogState& state) {
    for (HFONT* font : {&state.headingFont, &state.bodyFont, &state.detailFont}) {
        if (*font != nullptr) {
            DeleteObject(*font);
        }
    }
    const int dpi = static_cast<int>(GetDpiForWindow(state.window));
    state.headingFont = CreateFontW(
        -MulDiv(21, dpi, 96), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Display");
    state.bodyFont = CreateFontW(
        -MulDiv(16, dpi, 96), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Text");
    state.detailFont = CreateFontW(
        -MulDiv(12, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Text");
    SetControlFont(state.media, state.bodyFont);
    SetControlFont(state.package, state.bodyFont);
}

void LayoutImportChoiceDialog(ImportChoiceDialogState& state) {
    RECT client{};
    GetClientRect(state.window, &client);
    const int margin = Scale(state.window, 24);
    const int gap = Scale(state.window, 14);
    const int top = Scale(state.window, 84);
    const int height = std::max(1, static_cast<int>(client.bottom) - top - margin);
    const int width =
        std::max(1, (static_cast<int>(client.right) - margin * 2 - gap) / 2);
    MoveWindow(state.media, margin, top, width, height, TRUE);
    MoveWindow(state.package, margin + width + gap, top,
               client.right - (margin + width + gap) - margin, height, TRUE);
}

void DrawImportChoiceButton(const DRAWITEMSTRUCT& draw,
                            const ImportChoiceDialogState& state) {
    FillRectangle(draw.hDC, draw.rcItem, kBackground);
    const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
    const bool hovered = state.hoveredControl == static_cast<int>(draw.CtlID);
    RECT card = draw.rcItem;
    InflateRect(&card, -1, -1);
    const COLORREF fill = pressed
                              ? RGB(40, 51, 76)
                              : (hovered ? kPanelHover : kPanel);
    FillRoundedRectangle(draw.hDC, card, fill,
                         hovered || pressed ? kAccent : kBorder,
                         Scale(state.window, 12), hovered || pressed ? 2 : 1);

    RECT icon{card.left + Scale(state.window, 18),
              card.top + Scale(state.window, 20),
              card.left + Scale(state.window, 54),
              card.top + Scale(state.window, 56)};
    FillRoundedRectangle(draw.hDC, icon,
                         draw.CtlID == kImportChoiceMedia
                             ? RGB(47, 79, 133)
                             : RGB(75, 60, 125),
                         draw.CtlID == kImportChoiceMedia
                             ? RGB(82, 134, 207)
                             : RGB(126, 103, 196),
                         Scale(state.window, 8));
    const HPEN iconPen = CreatePen(PS_SOLID, std::max(1, Scale(state.window, 2)),
                                   RGB(222, 229, 247));
    const HGDIOBJ previousPen = SelectObject(draw.hDC, iconPen);
    if (draw.CtlID == kImportChoiceMedia) {
        MoveToEx(draw.hDC, icon.left + Scale(state.window, 7),
                 icon.bottom - Scale(state.window, 9), nullptr);
        LineTo(draw.hDC, icon.left + Scale(state.window, 15),
               icon.top + Scale(state.window, 18));
        LineTo(draw.hDC, icon.left + Scale(state.window, 21),
               icon.top + Scale(state.window, 24));
        LineTo(draw.hDC, icon.right - Scale(state.window, 6),
               icon.top + Scale(state.window, 12));
    } else {
        const HGDIOBJ previousBrush =
            SelectObject(draw.hDC, GetStockObject(HOLLOW_BRUSH));
        Rectangle(draw.hDC, icon.left + Scale(state.window, 8),
                  icon.top + Scale(state.window, 10),
                  icon.right - Scale(state.window, 8),
                  icon.bottom - Scale(state.window, 8));
        SelectObject(draw.hDC, previousBrush);
        MoveToEx(draw.hDC, icon.left + Scale(state.window, 8),
                 icon.top + Scale(state.window, 17), nullptr);
        LineTo(draw.hDC, icon.right - Scale(state.window, 8),
               icon.top + Scale(state.window, 17));
    }
    SelectObject(draw.hDC, previousPen);
    DeleteObject(iconPen);

    const std::wstring_view title =
        draw.CtlID == kImportChoiceMedia ? L"导入图片 / 视频"
                                         : L"导入分享包";
    const std::wstring_view description =
        draw.CtlID == kImportChoiceMedia
            ? L"JPG、PNG、GIF、MP4 等常见格式"
            : L"ZIP 分享包或解压后的 .lwewall";
    RECT titleRectangle{icon.right + Scale(state.window, 14), icon.top,
                        card.right - Scale(state.window, 14),
                        icon.top + Scale(state.window, 22)};
    DrawTextLine(draw.hDC, title, titleRectangle, state.bodyFont, kTextPrimary,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT descriptionRectangle{titleRectangle.left,
                              titleRectangle.bottom + Scale(state.window, 2),
                              titleRectangle.right, icon.bottom};
    DrawTextLine(draw.hDC, description, descriptionRectangle, state.detailFont,
                 kTextSecondary,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

LRESULT CALLBACK ImportChoiceButtonProcedure(
    const HWND window, const UINT message, const WPARAM wParam,
    const LPARAM lParam, const UINT_PTR, const DWORD_PTR referenceData) {
    auto* state = reinterpret_cast<ImportChoiceDialogState*>(referenceData);
    if (state != nullptr && message == WM_MOUSEMOVE) {
        const int identifier = GetDlgCtrlID(window);
        if (state->hoveredControl != identifier) {
            state->hoveredControl = identifier;
            InvalidateRect(state->media, nullptr, FALSE);
            InvalidateRect(state->package, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
    } else if (state != nullptr && message == WM_MOUSELEAVE) {
        if (state->hoveredControl == GetDlgCtrlID(window)) {
            state->hoveredControl = 0;
            InvalidateRect(window, nullptr, FALSE);
        }
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

LRESULT CALLBACK ImportChoiceWindowProcedure(const HWND window,
                                             const UINT message,
                                             const WPARAM wParam,
                                             const LPARAM lParam) {
    auto* state = reinterpret_cast<ImportChoiceDialogState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = static_cast<ImportChoiceDialogState*>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }
    switch (message) {
        case WM_CREATE:
            state->media = CreateWindowExW(
                0, L"BUTTON", L"导入图片 / 视频",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 1, 1,
                window,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(kImportChoiceMedia)),
                state->instance, nullptr);
            state->package = CreateWindowExW(
                0, L"BUTTON", L"导入分享包",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 1, 1,
                window,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(kImportChoicePackage)),
                state->instance, nullptr);
            if (state->media == nullptr || state->package == nullptr) {
                return -1;
            }
            for (const HWND control : {state->media, state->package}) {
                SetWindowSubclass(control, &ImportChoiceButtonProcedure, 1,
                                  reinterpret_cast<DWORD_PTR>(state));
            }
            RecreateImportChoiceFonts(*state);
            LayoutImportChoiceDialog(*state);
            return 0;
        case WM_SIZE:
            LayoutImportChoiceDialog(*state);
            return 0;
        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOACTIVATE | SWP_NOZORDER);
            RecreateImportChoiceFonts(*state);
            LayoutImportChoiceDialog(*state);
            InvalidateRect(window, nullptr, TRUE);
            return 0;
        }
        case WM_COMMAND:
            if (HIWORD(wParam) == BN_CLICKED &&
                (LOWORD(wParam) == kImportChoiceMedia ||
                 LOWORD(wParam) == kImportChoicePackage)) {
                state->result = LOWORD(wParam) == kImportChoiceMedia
                                    ? ModernMainWindow::ImportChoice::MediaFiles
                                    : ModernMainWindow::ImportChoice::SharePackage;
                DestroyWindow(window);
                return 0;
            }
            break;
        case WM_DRAWITEM: {
            const auto& draw = *reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (draw.CtlID == kImportChoiceMedia ||
                draw.CtlID == kImportChoicePackage) {
                DrawImportChoiceButton(draw, *state);
                return TRUE;
            }
            break;
        }
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const HDC context = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            FillRectangle(context, client, kBackground);
            RECT title{Scale(window, 24), Scale(window, 14),
                       client.right - Scale(window, 24), Scale(window, 46)};
            DrawTextLine(context, L"导入壁纸", title, state->headingFont,
                         kTextPrimary,
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            RECT subtitle{title.left, Scale(window, 44), title.right,
                          Scale(window, 76)};
            DrawTextLine(context, L"选择要导入的内容类型", subtitle,
                         state->detailFont, kTextSecondary,
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            state->complete = true;
            return 0;
        case WM_NCDESTROY:
            for (const HWND control : {state->media, state->package}) {
                if (control != nullptr) {
                    RemoveWindowSubclass(control, &ImportChoiceButtonProcedure, 1);
                }
            }
            for (HFONT* font : {&state->headingFont, &state->bodyFont,
                                &state->detailFont}) {
                if (*font != nullptr) {
                    DeleteObject(*font);
                    *font = nullptr;
                }
            }
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool RegisterImportChoiceWindowClass(const HINSTANCE instance) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &ImportChoiceWindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kImportChoiceWindowClass;
    return RegisterClassExW(&windowClass) != 0 ||
           GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

std::optional<ModernMainWindow::ImportChoice> ShowImportChoiceDialog(
    const HWND owner, const HINSTANCE instance) {
    if (!IsWindow(owner) || !RegisterImportChoiceWindowClass(instance)) {
        return std::nullopt;
    }
    ImportChoiceDialogState state;
    state.instance = instance;
    const int clientWidth = Scale(owner, 620);
    const int clientHeight = Scale(owner, 218);
    RECT outer{0, 0, clientWidth, clientHeight};
    AdjustWindowRectExForDpi(&outer, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE,
                             WS_EX_DLGMODALFRAME, GetDpiForWindow(owner));
    RECT ownerRectangle{};
    GetWindowRect(owner, &ownerRectangle);
    const int width = outer.right - outer.left;
    const int height = outer.bottom - outer.top;
    const int left = ownerRectangle.left +
                     (ownerRectangle.right - ownerRectangle.left - width) / 2;
    const int top = ownerRectangle.top +
                    (ownerRectangle.bottom - ownerRectangle.top - height) / 2;
    const HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME, kImportChoiceWindowClass, L"导入壁纸",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, left, top, width, height, owner,
        nullptr, instance, &state);
    if (dialog == nullptr) {
        return std::nullopt;
    }
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(dialog, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark,
                          sizeof(dark));
    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOWNORMAL);
    SetForegroundWindow(dialog);
    SetFocus(state.media);
    bool receivedQuit = false;
    WPARAM quitCode = 0;
    MSG message{};
    while (!state.complete && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
            PostMessageW(dialog, WM_CLOSE, 0, 0);
            continue;
        }
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (message.message == WM_QUIT) {
        receivedQuit = true;
        quitCode = message.wParam;
    }
    if (IsWindow(dialog)) {
        DestroyWindow(dialog);
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (receivedQuit) {
        PostQuitMessage(static_cast<int>(quitCode));
    }
    return state.result;
}

}  // namespace

ModernMainWindow::~ModernMainWindow() {
    if (renameEdit_ != nullptr) {
        RemoveWindowSubclass(renameEdit_, &ModernMainWindow::RenameEditProcedure, 1);
    }
    if (groupRenameEdit_ != nullptr) {
        RemoveWindowSubclass(groupRenameEdit_, &ModernMainWindow::RenameEditProcedure, 1);
    }
    for (const HWND control : {filter_, library_, import_, export_, sound_,
                               displayMode_, dropdownList_, activeStatus_,
                               activeList_, exportSelectAll_, exportClearAll_,
                               exportConfirm_, exportCancel_, groupAll_,
                               groupFavorites_, groupList_, groupCreate_}) {
        if (control != nullptr) {
            RemoveWindowSubclass(control,
                                 &ModernMainWindow::InteractiveControlProcedure, 2);
        }
    }
    ClearThumbnails();
    for (const HFONT font :
         {titleFont_, headingFont_, bodyFont_, smallFont_, badgeFont_}) {
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
    if (sidebarBrush_ != nullptr) {
        DeleteObject(sidebarBrush_);
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
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_VSCROLL | LBS_NOTIFY |
            LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT,
        0, 0, 1, 1, parent_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(Library)), instance, nullptr);
    dropdownList_ = CreateWindowExW(
        WS_EX_TOOLWINDOW, L"LISTBOX", L"",
        WS_CHILD | WS_CLIPSIBLINGS | WS_VSCROLL | LBS_HASSTRINGS |
            LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT,
        0, 0, 1, 1, parent_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(DropdownList)), instance,
        nullptr);
    groupList_ = CreateWindowExW(
        0, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_VSCROLL | LBS_NOTIFY |
            LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT,
        0, 0, 1, 1, parent_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(GroupList)), instance,
        nullptr);
    activeList_ = CreateWindowExW(
        0, L"LISTBOX", L"",
        WS_CHILD | WS_CLIPSIBLINGS | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED |
            LBS_NOINTEGRALHEIGHT,
        0, 0, 1, 1, parent_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ActiveList)), instance,
        nullptr);

    const auto createButton = [&](const int identifier, const wchar_t* text) {
        return CreateWindowExW(
            0, L"BUTTON", text,
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW, 0, 0, 1, 1,
            parent_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(identifier)), instance,
            nullptr);
    };
    filter_ = createButton(Filter, L"分类：全部");
    import_ = createButton(Import, L"＋  导入壁纸");
    export_ = createButton(Export, L"导出分享包");
    sound_ = createButton(Sound, L"声音：关闭");
    displayMode_ = createButton(DisplayMode, L"显示方式：跨屏扩展");
    activeStatus_ = createButton(ActiveStatus, L"");
    groupAll_ = createButton(GroupAll, L"全部壁纸");
    groupFavorites_ = createButton(GroupFavorites, L"最爱壁纸");
    groupCreate_ = createButton(GroupCreate, L"＋  新建分组");
    exportSelectAll_ = createButton(ExportSelectAll, L"全选");
    exportClearAll_ = createButton(ExportClearAll, L"取消全选");
    exportConfirm_ = createButton(ExportConfirm, L"批量操作（0）");
    exportCancel_ = createButton(ExportCancel, L"退出选择");
    for (const HWND control : {exportSelectAll_, exportClearAll_, exportConfirm_,
                               exportCancel_}) {
        ShowWindow(control, SW_HIDE);
    }
    renameEdit_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 1, 1,
        parent_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(RenameCommit)),
        instance, nullptr);
    groupRenameEdit_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 1, 1,
        parent_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(GroupRenameCommit)),
        instance, nullptr);

    if (search_ == nullptr || filter_ == nullptr || library_ == nullptr ||
        import_ == nullptr || export_ == nullptr || sound_ == nullptr ||
        displayMode_ == nullptr || renameEdit_ == nullptr ||
        dropdownList_ == nullptr || activeStatus_ == nullptr ||
        activeList_ == nullptr || exportSelectAll_ == nullptr ||
        exportClearAll_ == nullptr || exportConfirm_ == nullptr ||
        exportCancel_ == nullptr || groupAll_ == nullptr ||
        groupFavorites_ == nullptr || groupList_ == nullptr ||
        groupCreate_ == nullptr || groupRenameEdit_ == nullptr) {
        return false;
    }

    SetWindowTheme(search_, L"DarkMode_Explorer", nullptr);
    SetWindowTheme(library_, L"DarkMode_Explorer", nullptr);
    SetWindowTheme(dropdownList_, L"DarkMode_Explorer", nullptr);
    SetWindowTheme(activeList_, L"DarkMode_Explorer", nullptr);
    SetWindowTheme(groupList_, L"DarkMode_Explorer", nullptr);
    SetWindowTheme(renameEdit_, L"DarkMode_Explorer", nullptr);
    SetWindowTheme(groupRenameEdit_, L"DarkMode_Explorer", nullptr);
    SetWindowSubclass(renameEdit_, &ModernMainWindow::RenameEditProcedure, 1,
                      reinterpret_cast<DWORD_PTR>(this));
    SetWindowSubclass(groupRenameEdit_, &ModernMainWindow::RenameEditProcedure, 1,
                      reinterpret_cast<DWORD_PTR>(this));
    for (const HWND control : {filter_, library_, import_, export_, sound_,
                               displayMode_, dropdownList_, activeStatus_,
                               activeList_, exportSelectAll_, exportClearAll_,
                               exportConfirm_, exportCancel_, groupAll_,
                               groupFavorites_, groupList_, groupCreate_}) {
        SetWindowSubclass(control, &ModernMainWindow::InteractiveControlProcedure, 2,
                          reinterpret_cast<DWORD_PTR>(this));
        hoverProgress_.emplace(control, 0.0F);
        hoverTargets_.emplace(control, false);
    }
    SendMessageW(search_, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"搜索名称、格式或路径"));

    editBrush_ = CreateSolidBrush(kPanel);
    panelBrush_ = CreateSolidBrush(kPanel);
    sidebarBrush_ = CreateSolidBrush(kSidebar);
    RecreateFonts();
    Layout();
    return true;
}

void ModernMainWindow::RecreateFonts() {
    for (HFONT* font :
         {&titleFont_, &headingFont_, &bodyFont_, &smallFont_, &badgeFont_}) {
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
    badgeFont_ = CreateFontW(-MulDiv(9, dpi, 96), 0, 0, 0, FW_SEMIBOLD, FALSE,
                             FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Segoe UI Variable Text");

    for (const HWND control : {search_, filter_, library_, import_, export_, sound_,
                               displayMode_, renameEdit_, dropdownList_, activeStatus_,
                               activeList_, exportSelectAll_, exportClearAll_,
                               exportConfirm_, exportCancel_, groupAll_,
                               groupFavorites_, groupList_, groupCreate_,
                               groupRenameEdit_}) {
        SetControlFont(control, bodyFont_);
    }
    SendMessageW(library_, LB_SETITEMHEIGHT, 0, Scale(parent_, 78));
    SendMessageW(activeList_, LB_SETITEMHEIGHT, 0, Scale(parent_, 78));
    SendMessageW(groupList_, LB_SETITEMHEIGHT, 0, Scale(parent_, 42));
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
    const int searchTop = Scale(parent_, 28);
    const int controlHeight = Scale(parent_, 40);
    const int filterWidth = Scale(parent_, 166);
    const RECT searchPanel = SearchPanelRectangle(parent_);

    // A native single-line EDIT pins text to its own client area and ignores
    // EM_SETRECT. Keep the searchable/cue-banner control single-line, but size
    // that inner client from the actual font metrics and center it inside the
    // painted 40-DIP search container. This keeps both cue text and the caret
    // vertically aligned at every DPI without losing EM_SETCUEBANNER support.
    const int searchInset = Scale(parent_, 12);
    const int searchPanelHeight = searchPanel.bottom - searchPanel.top;
    const int searchPanelWidth = searchPanel.right - searchPanel.left;
    const int searchEditHeight = std::min(
        searchPanelHeight, FontPixelHeight(parent_, bodyFont_) + Scale(parent_, 6));
    MoveWindow(search_, searchPanel.left + searchInset,
               searchPanel.top + (searchPanelHeight - searchEditHeight) / 2,
               std::max(1, searchPanelWidth - searchInset * 2),
               searchEditHeight, TRUE);
    MoveWindow(filter_, searchPanel.right + gap, searchTop, filterWidth,
               controlHeight, TRUE);

    const int actionTop = searchTop + controlHeight + gap;
    if (exportSelectionMode_) {
        for (const HWND control : {import_, export_, displayMode_}) {
            ShowWindow(control, SW_HIDE);
        }
        const int selectionWidth = std::max(1, (contentWidth - gap * 3) / 4);
        const std::array selectionControls{exportSelectAll_, exportClearAll_,
                                           exportConfirm_, exportCancel_};
        for (std::size_t index = 0; index < selectionControls.size(); ++index) {
            const int left = contentLeft +
                             static_cast<int>(index) * (selectionWidth + gap);
            const int widthForControl =
                index + 1 == selectionControls.size()
                    ? contentLeft + contentWidth - left
                    : selectionWidth;
            SetWindowPos(selectionControls[index], HWND_TOP, left, actionTop,
                         widthForControl, controlHeight, SWP_SHOWWINDOW);
        }
    } else {
        for (const HWND control : {exportSelectAll_, exportClearAll_, exportConfirm_,
                                   exportCancel_}) {
            ShowWindow(control, SW_HIDE);
        }
        const int actionWidth = std::max(1, (contentWidth - gap * 2) / 3);
        SetWindowPos(import_, HWND_TOP, contentLeft, actionTop, actionWidth,
                     controlHeight, SWP_SHOWWINDOW);
        SetWindowPos(export_, HWND_TOP, contentLeft + actionWidth + gap, actionTop,
                     actionWidth, controlHeight, SWP_SHOWWINDOW);
        SetWindowPos(displayMode_, HWND_TOP,
                     contentLeft + (actionWidth + gap) * 2, actionTop,
                     contentWidth - (actionWidth + gap) * 2, controlHeight,
                     SWP_SHOWWINDOW);
    }

    const int statusHeight = Scale(parent_, 62);
    const int listTop = actionTop + controlHeight + Scale(parent_, 16);
    const int listBottom = height - margin - statusHeight;
    MoveWindow(library_, contentLeft, listTop, contentWidth,
               std::max(1, listBottom - listTop - gap), TRUE);

    const int sidebarButtonWidth = sidebar - Scale(parent_, 32);
    const int sidebarButtonLeft = Scale(parent_, 16);
    const int sidebarButtonHeight = Scale(parent_, 40);
    const int resourceBottom = height - Scale(parent_, 16);
    const int resourceTop = resourceBottom - Scale(parent_, 62);
    const int soundTop = resourceTop - Scale(parent_, 12) - sidebarButtonHeight;
    const int createGroupTop = soundTop - Scale(parent_, 12) - sidebarButtonHeight;
    MoveWindow(groupAll_, sidebarButtonLeft, Scale(parent_, 28),
               sidebarButtonWidth, sidebarButtonHeight, TRUE);
    MoveWindow(groupFavorites_, sidebarButtonLeft, Scale(parent_, 76),
               sidebarButtonWidth, sidebarButtonHeight, TRUE);
    MoveWindow(groupList_, sidebarButtonLeft, Scale(parent_, 158),
               sidebarButtonWidth,
               std::max(1, createGroupTop - Scale(parent_, 170)), TRUE);
    MoveWindow(groupCreate_, sidebarButtonLeft, createGroupTop,
               sidebarButtonWidth, sidebarButtonHeight, TRUE);
    MoveWindow(sound_, sidebarButtonLeft, soundTop,
               sidebarButtonWidth, sidebarButtonHeight, TRUE);

    const int statusTop = height - Scale(parent_, 82);
    const int statusRight = contentLeft + contentWidth;
    const int statusCardHeight = Scale(parent_, 54);
    MoveWindow(activeStatus_, contentLeft, statusTop,
               statusRight - contentLeft, statusCardHeight, TRUE);

    if (activeDrawerVisible_ && !activeVisibleIndices_.empty()) {
        const int rowHeight = Scale(parent_, 78);
        const int visibleRows = std::clamp(
            static_cast<int>(activeVisibleIndices_.size()), 1, 4);
        const int drawerHeight = rowHeight * visibleRows;
        const int drawerBottom = statusTop - Scale(parent_, 8);
        const int drawerTop = std::max(listTop, drawerBottom - drawerHeight);
        SetWindowPos(activeList_, HWND_TOP, contentLeft, drawerTop,
                     statusRight - contentLeft,
                     drawerBottom - drawerTop, SWP_SHOWWINDOW);
    } else {
        ShowWindow(activeList_, SW_HIDE);
    }

    if (dropdownKind_ != DropdownKind::None) {
        const HWND anchor = dropdownKind_ == DropdownKind::Filter
                                ? filter_
                                : displayMode_;
        RECT anchorRectangle{};
        GetWindowRect(anchor, &anchorRectangle);
        MapWindowPoints(nullptr, parent_,
                        reinterpret_cast<POINT*>(&anchorRectangle), 2);
        const int rows = static_cast<int>(dropdownLabels_.size());
        const int rowHeight = dropdownKind_ == DropdownKind::DisplayMode
                                  ? Scale(parent_, 58)
                                  : Scale(parent_, 42);
        SendMessageW(dropdownList_, LB_SETITEMHEIGHT, 0, rowHeight);
        SetWindowPos(dropdownList_, HWND_TOP, anchorRectangle.left,
                     anchorRectangle.bottom + Scale(parent_, 4),
                     anchorRectangle.right - anchorRectangle.left,
                     rowHeight * rows + Scale(parent_, 4), SWP_SHOWWINDOW);
    }
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

    const HPEN groupDivider = CreatePen(
        PS_SOLID, std::max(1, Scale(parent_, 1)),
        BlendColor(kSidebar, kBorder, 0.78F));
    const HGDIOBJ previousGroupPen = SelectObject(deviceContext, groupDivider);
    MoveToEx(deviceContext, Scale(parent_, 28), Scale(parent_, 128), nullptr);
    LineTo(deviceContext, Scale(parent_, 188), Scale(parent_, 128));
    SelectObject(deviceContext, previousGroupPen);
    DeleteObject(groupDivider);
    RECT groupHeading{Scale(parent_, 26), Scale(parent_, 132),
                      Scale(parent_, 190), Scale(parent_, 154)};
    DrawTextLine(deviceContext, L"自定义分组", groupHeading, smallFont_,
                 kTextSecondary,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const RECT searchPanel = SearchPanelRectangle(parent_);
    FillRoundedRectangle(deviceContext, searchPanel, kPanel, kPanel,
                         Scale(parent_, 8));

    RECT resourceCard{Scale(parent_, 16),
                      client.bottom - Scale(parent_, 78),
                      Scale(parent_, 200),
                      client.bottom - Scale(parent_, 16)};
    FillRoundedRectangle(deviceContext, resourceCard, kPanel, kBorder,
                         Scale(parent_, 12));
    const int resourceCenterX = (resourceCard.left + resourceCard.right) / 2;
    const int resourceCenterY = (resourceCard.top + resourceCard.bottom) / 2;
    const HPEN divider = CreatePen(PS_SOLID, std::max(1, Scale(parent_, 1)),
                                   BlendColor(kPanel, kBorder, 0.72F));
    const HGDIOBJ previousPen = SelectObject(deviceContext, divider);
    MoveToEx(deviceContext, resourceCenterX,
             resourceCard.top + Scale(parent_, 9), nullptr);
    LineTo(deviceContext, resourceCenterX,
           resourceCard.bottom - Scale(parent_, 9));
    MoveToEx(deviceContext, resourceCard.left + Scale(parent_, 12),
             resourceCenterY, nullptr);
    LineTo(deviceContext, resourceCard.right - Scale(parent_, 12),
           resourceCenterY);
    SelectObject(deviceContext, previousPen);
    DeleteObject(divider);

    std::array<std::wstring_view, 4> resourceCells{};
    std::wstring_view remaining = resourceUsage_;
    for (std::size_t index = 0; index < resourceCells.size(); ++index) {
        const std::size_t delimiter = remaining.find_first_of(L"\t\n");
        resourceCells[index] = remaining.substr(0, delimiter);
        if (delimiter == std::wstring_view::npos) {
            remaining = {};
        } else {
            remaining.remove_prefix(delimiter + 1U);
        }
    }
    const int cellInset = Scale(parent_, 5);
    const std::array<RECT, 4> cellRectangles{
        RECT{resourceCard.left + cellInset, resourceCard.top + Scale(parent_, 2),
             resourceCenterX - cellInset, resourceCenterY},
        RECT{resourceCenterX + cellInset, resourceCard.top + Scale(parent_, 2),
             resourceCard.right - cellInset, resourceCenterY},
        RECT{resourceCard.left + cellInset, resourceCenterY,
             resourceCenterX - cellInset, resourceCard.bottom - Scale(parent_, 2)},
        RECT{resourceCenterX + cellInset, resourceCenterY,
             resourceCard.right - cellInset,
             resourceCard.bottom - Scale(parent_, 2)}};
    for (std::size_t index = 0; index < resourceCells.size(); ++index) {
        DrawTextLine(deviceContext, resourceCells[index], cellRectangles[index],
                     smallFont_, kTextSecondary,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

bool ModernMainWindow::DrawItem(const DRAWITEMSTRUCT& draw) const {
    const auto drawDirect = [&](const DRAWITEMSTRUCT& target) {
        if (target.CtlID == Library) {
            DrawLibraryItem(target);
            return true;
        }
        if (target.CtlID == ActiveList) {
            DrawActiveItem(target);
            return true;
        }
        if (target.CtlID == DropdownList) {
            DrawDropdownItem(target);
            return true;
        }
        if (target.CtlID == GroupList) {
            DrawGroupItem(target);
            return true;
        }
        if (target.CtlType == ODT_BUTTON) {
            DrawButton(target);
            return true;
        }
        return false;
    };

    const int width = draw.rcItem.right - draw.rcItem.left;
    const int height = draw.rcItem.bottom - draw.rcItem.top;
    if (width <= 0 || height <= 0) {
        return drawDirect(draw);
    }
    const HDC buffer = CreateCompatibleDC(draw.hDC);
    const HBITMAP bitmap = CreateCompatibleBitmap(draw.hDC, width, height);
    if (buffer == nullptr || bitmap == nullptr) {
        if (bitmap != nullptr) {
            DeleteObject(bitmap);
        }
        if (buffer != nullptr) {
            DeleteDC(buffer);
        }
        return drawDirect(draw);
    }
    const HGDIOBJ previousBitmap = SelectObject(buffer, bitmap);
    SetViewportOrgEx(buffer, -draw.rcItem.left, -draw.rcItem.top, nullptr);
    DRAWITEMSTRUCT buffered = draw;
    buffered.hDC = buffer;
    const bool handled = drawDirect(buffered);
    SetViewportOrgEx(buffer, 0, 0, nullptr);
    if (handled) {
        BitBlt(draw.hDC, draw.rcItem.left, draw.rcItem.top, width, height,
               buffer, 0, 0, SRCCOPY);
    }
    SelectObject(buffer, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(buffer);
    return handled;
}

void ModernMainWindow::DrawButton(const DRAWITEMSTRUCT& draw) const {
    wchar_t text[128]{};
    GetWindowTextW(draw.hwndItem, text, static_cast<int>(std::size(text)));
    const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
    const bool disabled = (draw.itemState & ODS_DISABLED) != 0;
    const float hover = ControlHoverProgress(draw.hwndItem);
    COLORREF fill = kPanel;
    COLORREF outline = kBorder;
    COLORREF foreground = disabled ? RGB(90, 98, 114) : kTextPrimary;
    const bool fixedGroupSelected =
        (draw.CtlID == GroupAll && CurrentGroupIsAll()) ||
        (draw.CtlID == GroupFavorites && CurrentGroupIsFavorites());
    if (draw.CtlID == Import || draw.CtlID == ExportConfirm) {
        fill = pressed ? RGB(72, 99, 207)
                       : BlendColor(kAccent, kAccentHover, hover);
        outline = fill;
    } else if (fixedGroupSelected) {
        fill = BlendColor(RGB(38, 45, 62), kAccent, 0.12F);
        outline = fill;
    } else if (pressed) {
        fill = kPanelHover;
    } else {
        fill = BlendColor(kPanel, kPanelHover, hover);
    }
    const bool sidebarControl = draw.CtlID == Sound || draw.CtlID == GroupAll ||
                                draw.CtlID == GroupFavorites ||
                                draw.CtlID == GroupCreate;
    FillRectangle(draw.hDC, draw.rcItem,
                  sidebarControl ? kSidebar : kBackground);
    RECT button = draw.rcItem;
    InflateRect(&button, -1, -1);
    FillRoundedRectangle(draw.hDC, button, fill, outline, Scale(parent_, 10));
    RECT textRectangle = button;
    if (draw.CtlID == ActiveStatus) {
        textRectangle.left += Scale(parent_, 16);
        textRectangle.right -= Scale(parent_, 42);
        DrawTextLine(draw.hDC, status_, textRectangle, smallFont_, kTextSecondary,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        const int centerX = button.right - Scale(parent_, 20);
        const int centerY = (button.top + button.bottom) / 2;
        const int halfWidth = Scale(parent_, 5);
        const int halfHeight = Scale(parent_, 3);
        const HPEN pen = CreatePen(PS_SOLID, std::max(1, Scale(parent_, 2)),
                                   kTextSecondary);
        const HGDIOBJ previous = SelectObject(draw.hDC, pen);
        MoveToEx(draw.hDC, centerX - halfWidth,
                  centerY + (activeDrawerVisible_ ? -halfHeight : halfHeight),
                  nullptr);
        LineTo(draw.hDC, centerX,
                centerY + (activeDrawerVisible_ ? halfHeight : -halfHeight));
        LineTo(draw.hDC, centerX + halfWidth,
                centerY + (activeDrawerVisible_ ? -halfHeight : halfHeight));
        SelectObject(draw.hDC, previous);
        DeleteObject(pen);
        return;
    }
    if (draw.CtlID == GroupAll || draw.CtlID == GroupFavorites) {
        RECT icon{button.left + Scale(parent_, 13),
                  button.top + (button.bottom - button.top - Scale(parent_, 24)) / 2,
                  button.left + Scale(parent_, 37),
                  button.top + (button.bottom - button.top + Scale(parent_, 24)) / 2};
        FillRoundedRectangle(draw.hDC, icon,
                             fixedGroupSelected
                                 ? BlendColor(kAccent, kPanel, 0.74F)
                                 : kPanel,
                             fixedGroupSelected ? kAccent : kBorder,
                             Scale(parent_, 7));
        if (draw.CtlID == GroupFavorites) {
            RECT heart = icon;
            heart.top += Scale(parent_, 1);
            DrawTextLine(draw.hDC, L"♥", heart, smallFont_,
                         fixedGroupSelected ? kAccentHover : kTextSecondary,
                         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            const int dot = Scale(parent_, 2);
            const int centerX = (icon.left + icon.right) / 2;
            const int centerY = (icon.top + icon.bottom) / 2;
            for (const POINT offset : {POINT{-4, -4}, POINT{4, -4},
                                       POINT{-4, 4}, POINT{4, 4}}) {
                RECT mark{centerX + Scale(parent_, offset.x) - dot,
                          centerY + Scale(parent_, offset.y) - dot,
                          centerX + Scale(parent_, offset.x) + dot,
                          centerY + Scale(parent_, offset.y) + dot};
                FillRoundedRectangle(
                    draw.hDC, mark,
                    fixedGroupSelected ? kAccentHover : kTextSecondary,
                    fixedGroupSelected ? kAccentHover : kTextSecondary,
                    Scale(parent_, 2));
            }
        }
        RECT label = button;
        label.left = icon.right + Scale(parent_, 10);
        label.right -= Scale(parent_, 42);
        DrawTextLine(draw.hDC, text, label, bodyFont_, foreground,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        const std::wstring countText = std::to_wstring(
            draw.CtlID == GroupAll ? items_.size() : favorites_.size());
        RECT count{button.right - Scale(parent_, 34),
                   button.top + (button.bottom - button.top - Scale(parent_, 22)) / 2,
                   button.right - Scale(parent_, 10),
                   button.top + (button.bottom - button.top + Scale(parent_, 22)) / 2};
        FillRoundedRectangle(draw.hDC, count, kPanel,
                             fixedGroupSelected ? kAccent : kBorder,
                             Scale(parent_, 11));
        DrawTextLine(draw.hDC, countText, count, badgeFont_, kTextSecondary,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    const bool dropdown = draw.CtlID == Filter || draw.CtlID == DisplayMode;
    if (dropdown) {
        const int arrowSpace = Scale(parent_, 38);
        textRectangle.left += arrowSpace;
        textRectangle.right -= arrowSpace;
    }
    DrawTextLine(draw.hDC, text, textRectangle, bodyFont_, foreground,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (dropdown) {
        const int centerX = button.right - Scale(parent_, 20);
        const int centerY = (button.top + button.bottom) / 2;
        const int halfWidth = Scale(parent_, 5);
        const int halfHeight = Scale(parent_, 3);
        const HPEN pen = CreatePen(PS_SOLID, std::max(1, Scale(parent_, 2)),
                                   foreground);
        const HGDIOBJ previous = SelectObject(draw.hDC, pen);
        const bool open = (draw.CtlID == Filter &&
                           dropdownKind_ == DropdownKind::Filter) ||
                          (draw.CtlID == DisplayMode &&
                           dropdownKind_ == DropdownKind::DisplayMode);
        MoveToEx(draw.hDC, centerX - halfWidth,
                 centerY + (open ? halfHeight : -halfHeight), nullptr);
        LineTo(draw.hDC, centerX, centerY + (open ? -halfHeight : halfHeight));
        LineTo(draw.hDC, centerX + halfWidth,
               centerY + (open ? halfHeight : -halfHeight));
        SelectObject(draw.hDC, previous);
        DeleteObject(pen);
    }
}

void ModernMainWindow::DrawGroupItem(const DRAWITEMSTRUCT& draw) const {
    FillRectangle(draw.hDC, draw.rcItem, kSidebar);
    if (draw.itemID == static_cast<UINT>(-1) || draw.itemID >= groups_.size()) {
        return;
    }
    RECT row = draw.rcItem;
    row.left += Scale(parent_, 1);
    row.right -= Scale(parent_, 1);
    InflateRect(&row, 0, -2);
    const bool selected = _wcsicmp(groups_[draw.itemID].id.c_str(),
                                   currentGroupId_.c_str()) == 0;
    const float hover = static_cast<int>(draw.itemID) == groupHoverIndex_
                            ? groupHoverProgress_
                            : 0.0F;
    const COLORREF fill = selected
                              ? BlendColor(RGB(38, 45, 62), kAccent, 0.10F)
                              : BlendColor(kSidebar, kPanelHover, hover);
    FillRoundedRectangle(draw.hDC, row, fill, fill, Scale(parent_, 10));
    if (selected) {
        RECT accent{row.left + Scale(parent_, 2), row.top + Scale(parent_, 8),
                    row.left + Scale(parent_, 5), row.bottom - Scale(parent_, 8)};
        FillRoundedRectangle(draw.hDC, accent, kAccent, kAccent,
                             Scale(parent_, 3));
    }
    RECT icon{row.left + Scale(parent_, 13),
              row.top + (row.bottom - row.top - Scale(parent_, 24)) / 2,
              row.left + Scale(parent_, 37),
              row.top + (row.bottom - row.top + Scale(parent_, 24)) / 2};
    const COLORREF iconFill = selected
                                  ? BlendColor(kAccent, kPanel, 0.74F)
                                  : BlendColor(kPanel, kAccent, hover * 0.10F);
    FillRoundedRectangle(draw.hDC, icon, iconFill,
                         selected ? kAccent : kBorder, Scale(parent_, 7));
    const int dot = Scale(parent_, 2);
    const int centerX = (icon.left + icon.right) / 2;
    const int centerY = (icon.top + icon.bottom) / 2;
    for (const POINT offset : {POINT{-4, -4}, POINT{4, -4},
                               POINT{-4, 4}, POINT{4, 4}}) {
        RECT mark{centerX + Scale(parent_, offset.x) - dot,
                  centerY + Scale(parent_, offset.y) - dot,
                  centerX + Scale(parent_, offset.x) + dot,
                  centerY + Scale(parent_, offset.y) + dot};
        FillRoundedRectangle(draw.hDC, mark,
                             selected ? kAccentHover : kTextSecondary,
                             selected ? kAccentHover : kTextSecondary,
                             Scale(parent_, 2));
    }
    RECT label = row;
    label.left = icon.right + Scale(parent_, 10);
    label.right -= Scale(parent_, 42);
    DrawTextLine(draw.hDC, groups_[draw.itemID].name, label, bodyFont_,
                 kTextPrimary,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    const std::wstring countText =
        std::to_wstring(groups_[draw.itemID].fileNames.size());
    RECT count{row.right - Scale(parent_, 34),
               row.top + (row.bottom - row.top - Scale(parent_, 22)) / 2,
               row.right - Scale(parent_, 10),
               row.top + (row.bottom - row.top + Scale(parent_, 22)) / 2};
    FillRoundedRectangle(draw.hDC, count,
                         selected ? BlendColor(kAccent, kPanel, 0.80F) : kPanel,
                         selected ? BlendColor(kAccent, kBorder, 0.35F) : kBorder,
                         Scale(parent_, 11));
    DrawTextLine(draw.hDC, countText, count, badgeFont_,
                 selected ? kTextPrimary : kTextSecondary,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (groupDragActive_ &&
        static_cast<int>(draw.itemID) == groupDragTargetIndex_) {
        const int y = groupDragInsertAfter_ ? draw.rcItem.bottom - Scale(parent_, 2)
                                            : draw.rcItem.top + Scale(parent_, 2);
        const HPEN marker = CreatePen(PS_SOLID, Scale(parent_, 2), kAccent);
        const HGDIOBJ previous = SelectObject(draw.hDC, marker);
        MoveToEx(draw.hDC, draw.rcItem.left + Scale(parent_, 8), y, nullptr);
        LineTo(draw.hDC, draw.rcItem.right - Scale(parent_, 8), y);
        SelectObject(draw.hDC, previous);
        DeleteObject(marker);
    }
}

void ModernMainWindow::DrawLibraryItem(const DRAWITEMSTRUCT& draw) const {
    ++libraryDrawCount_;
    FillRectangle(draw.hDC, draw.rcItem, kBackground);
    if (draw.itemID == static_cast<UINT>(-1) ||
        draw.itemID >= visibleIndices_.size()) {
        return;
    }
    const core::WallpaperItem& item = items_[visibleIndices_[draw.itemID]];
    const bool selected = (draw.itemState & ODS_SELECTED) != 0;
    const ActiveWallpaperInfo* activeInfo =
        FindActiveWallpaper(item.path.native());
    const bool active = activeInfo != nullptr;
    const float hover = static_cast<int>(draw.itemID) == libraryHoverIndex_
                            ? libraryHoverProgress_
                            : 0.0F;
    const bool exportChecked =
        exportSelectedPaths_.contains(item.path.native());
    DrawWallpaperCard(draw, item, active,
                      active ? activeInfo->displayLabel : std::wstring_view{},
                      active ? std::span<const DisplayBadge>(
                                   activeInfo->displayBadges)
                             : std::span<const DisplayBadge>{},
                      false,
                      std::max(hover, selected ? 1.0F : 0.0F),
                      exportSelectionMode_, exportChecked);
    if (libraryDragActive_ &&
        static_cast<int>(draw.itemID) == libraryDragTargetVisibleIndex_) {
        RECT marker = draw.rcItem;
        marker.left += Scale(parent_, 8);
        marker.right -= Scale(parent_, 8);
        const int y = libraryDragInsertAfter_ ? marker.bottom - Scale(parent_, 2)
                                              : marker.top;
        const HPEN pen = CreatePen(PS_SOLID, std::max(2, Scale(parent_, 3)),
                                   kAccentHover);
        const HGDIOBJ previous = SelectObject(draw.hDC, pen);
        MoveToEx(draw.hDC, marker.left, y, nullptr);
        LineTo(draw.hDC, marker.right, y);
        SelectObject(draw.hDC, previous);
        DeleteObject(pen);
    }
}

void ModernMainWindow::DrawActiveItem(const DRAWITEMSTRUCT& draw) const {
    FillRectangle(draw.hDC, draw.rcItem, kBackground);
    if (draw.itemID == static_cast<UINT>(-1) ||
        draw.itemID >= activeVisibleIndices_.size()) {
        return;
    }
    const core::WallpaperItem& item = items_[activeVisibleIndices_[draw.itemID]];
    const ActiveWallpaperInfo* activeInfo =
        FindActiveWallpaper(item.path.native());
    const bool selected = (draw.itemState & ODS_SELECTED) != 0;
    const float hover = static_cast<int>(draw.itemID) == activeHoverIndex_
                            ? activeHoverProgress_
                            : 0.0F;
    DrawWallpaperCard(draw, item, true,
                      activeInfo != nullptr ? activeInfo->displayLabel
                                            : std::wstring_view{},
                      activeInfo != nullptr
                          ? std::span<const DisplayBadge>(
                                activeInfo->displayBadges)
                          : std::span<const DisplayBadge>{},
                      true,
                      std::max(hover, selected ? 1.0F : 0.0F));
}

void ModernMainWindow::DrawWallpaperCard(
    const DRAWITEMSTRUCT& draw, const core::WallpaperItem& item, const bool active,
    const std::wstring_view displayLabel,
    const std::span<const DisplayBadge> displayBadges,
    const bool showCancelButton, const float hoverProgress,
    const bool showSelectionBox, const bool selectionChecked) const {
    RECT card = draw.rcItem;
    InflateRect(&card, -Scale(parent_, 4), -Scale(parent_, 5));
    const COLORREF cardFill = BlendColor(kPanel, kPanelHover, hoverProgress);
    FillRoundedRectangle(draw.hDC, card, cardFill,
                         active ? kAccent : kBorder, Scale(parent_, 12),
                         active ? 2 : 1);

    int contentLeft = card.left + Scale(parent_, 12);
    if (showSelectionBox) {
        const int boxSize = Scale(parent_, 20);
        RECT box{contentLeft,
                 card.top + (card.bottom - card.top - boxSize) / 2,
                 contentLeft + boxSize,
                 card.top + (card.bottom - card.top + boxSize) / 2};
        FillRoundedRectangle(draw.hDC, box,
                             selectionChecked ? kAccent : kPanel,
                             selectionChecked ? kAccent : kTextSecondary,
                             Scale(parent_, 5));
        if (selectionChecked) {
            const HPEN pen = CreatePen(PS_SOLID, std::max(1, Scale(parent_, 2)),
                                       RGB(255, 255, 255));
            const HGDIOBJ previous = SelectObject(draw.hDC, pen);
            MoveToEx(draw.hDC, box.left + Scale(parent_, 5),
                     box.top + Scale(parent_, 10), nullptr);
            LineTo(draw.hDC, box.left + Scale(parent_, 9),
                   box.top + Scale(parent_, 14));
            LineTo(draw.hDC, box.left + Scale(parent_, 16),
                   box.top + Scale(parent_, 6));
            SelectObject(draw.hDC, previous);
            DeleteObject(pen);
        }
        contentLeft = box.right + Scale(parent_, 12);
    }

    RECT icon{contentLeft, card.top + Scale(parent_, 9),
              contentLeft + Scale(parent_, 82), card.bottom - Scale(parent_, 9)};
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

    int nameLeft = icon.right + Scale(parent_, 14);
    if (!displayBadges.empty()) {
        const int badgeWidth = Scale(parent_, 66);
        const int badgeGap = Scale(parent_, 6);
        const int nameMinimumWidth = Scale(parent_, 130);
        const int nameRight = card.right - Scale(parent_, 120);
        const int badgeLimit = std::max(
            nameLeft,
            nameRight - nameMinimumWidth);
        for (std::size_t index = 0; index < displayBadges.size(); ++index) {
            if (nameLeft + badgeWidth > badgeLimit) {
                break;
            }
            const DisplayBadge& display = displayBadges[index];
            const int badgeHeight = Scale(parent_, 34);
            const int badgeTop =
                card.top + (card.bottom - card.top - badgeHeight) / 2;
            RECT badge{nameLeft, badgeTop, nameLeft + badgeWidth,
                       badgeTop + badgeHeight};
            FillRoundedRectangle(draw.hDC, badge, RGB(36, 45, 64),
                                 RGB(76, 96, 139), Scale(parent_, 8));
            RECT badgeLabel = badge;
            if (display.primary) {
                badgeLabel.right -= Scale(parent_, 19);
            }
            DrawTextLine(draw.hDC, display.label, badgeLabel, smallFont_,
                         RGB(211, 221, 244),
                         DT_CENTER | DT_VCENTER | DT_SINGLELINE |
                             DT_END_ELLIPSIS);
            if (display.primary) {
                const int primaryHeight = Scale(parent_, 24);
                const int primaryTop =
                    badge.top + (badge.bottom - badge.top - primaryHeight) / 2;
                RECT primary{badge.right - Scale(parent_, 19), primaryTop,
                             badge.right - Scale(parent_, 3),
                             primaryTop + primaryHeight / 2};
                DrawTextLine(draw.hDC, L"主", primary, badgeFont_,
                             RGB(241, 103, 115),
                             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                primary.top = primary.bottom;
                primary.bottom = primaryTop + primaryHeight;
                DrawTextLine(draw.hDC, L"屏", primary, badgeFont_,
                             RGB(241, 103, 115),
                             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            nameLeft = badge.right + badgeGap;
        }
    }

    RECT name{nameLeft, card.top + Scale(parent_, 9),
              card.right - Scale(parent_, 120), card.top + Scale(parent_, 38)};
    DrawTextLine(draw.hDC, item.displayName, name, bodyFont_, kTextPrimary,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    std::wstring details;
    if (active && displayBadges.empty() && !displayLabel.empty()) {
        details = L"应用到：" + std::wstring(displayLabel) + L"  ·  ";
    }
    details += item.formatLabel;
    if (item.width > 0 && item.height > 0) {
        details += L"  ·  " + std::to_wstring(item.width) + L"×" +
                   std::to_wstring(item.height);
    }
    details += L"  ·  " + FormatBytes(item.fileSize);
    if (item.kind == media::WallpaperKind::Video) {
        details += item.hasAudio ? L"  ·  含音轨" : L"  ·  无音轨";
    }
    RECT detail{name.left, card.top + Scale(parent_, 37),
                card.right - Scale(parent_,
                                   showCancelButton ? 126 : (active ? 112 : 20)),
                card.bottom - Scale(parent_, 7)};
    DrawTextLine(draw.hDC, details, detail, smallFont_, kTextSecondary,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (showCancelButton) {
        RECT cancel{card.right - Scale(parent_, 112),
                    card.top + Scale(parent_, 17),
                    card.right - Scale(parent_, 14),
                    card.bottom - Scale(parent_, 17)};
        FillRoundedRectangle(draw.hDC, cancel,
                             BlendColor(
                                 RGB(45, 37, 48), RGB(82, 43, 55),
                                 static_cast<int>(draw.itemID) ==
                                         activeCancelHoverIndex_
                                     ? 1.0F
                                     : hoverProgress * 0.35F),
                             RGB(90, 55, 67), Scale(parent_, 8));
        DrawTextLine(draw.hDC, L"取消应用", cancel, smallFont_, kDanger,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else if (active) {
        RECT badge{card.right - Scale(parent_, 92), card.top + Scale(parent_, 20),
                   card.right - Scale(parent_, 14), card.top + Scale(parent_, 48)};
        const COLORREF badgeFill =
            static_cast<int>(draw.itemID) == libraryBadgeHoverIndex_
                ? RGB(38, 126, 91)
                : RGB(30, 101, 76);
        const COLORREF badgeOutline =
            static_cast<int>(draw.itemID) == libraryBadgeHoverIndex_
                ? RGB(74, 169, 126)
                : RGB(52, 137, 103);
        FillRoundedRectangle(draw.hDC, badge, badgeFill, badgeOutline,
                             Scale(parent_, 9));
        DrawTextLine(draw.hDC, L"使用中", badge, smallFont_, RGB(205, 247, 226),
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

void ModernMainWindow::DrawDropdownItem(const DRAWITEMSTRUCT& draw) const {
    FillRectangle(draw.hDC, draw.rcItem, kPanel);
    if (draw.itemID == static_cast<UINT>(-1) ||
        draw.itemID >= dropdownLabels_.size()) {
        return;
    }
    const bool selected = (draw.itemState & ODS_SELECTED) != 0;
    const float hover = static_cast<int>(draw.itemID) == dropdownHoverIndex_
                            ? dropdownHoverProgress_
                            : 0.0F;
    RECT row = draw.rcItem;
    InflateRect(&row, -Scale(parent_, 4), -Scale(parent_, 3));
    FillRoundedRectangle(draw.hDC, row,
                         BlendColor(kPanel, RGB(42, 50, 68),
                                    std::max(hover, selected ? 1.0F : 0.0F)),
                         selected ? kAccent : kPanel, Scale(parent_, 8));

    const bool checked =
        (dropdownKind_ == DropdownKind::Filter &&
         draw.itemID == static_cast<UINT>(filterKind_)) ||
        (dropdownKind_ == DropdownKind::DisplayMode &&
         draw.itemID == static_cast<UINT>(spanAcrossDisplays_ ? 0 : 1));
    RECT indicator{row.left + Scale(parent_, 14),
                   row.top + (row.bottom - row.top - Scale(parent_, 16)) / 2,
                   row.left + Scale(parent_, 30),
                   row.top + (row.bottom - row.top + Scale(parent_, 16)) / 2};
    FillRoundedRectangle(draw.hDC, indicator, checked ? kAccent : kPanel,
                         checked ? kAccent : kTextSecondary,
                         Scale(parent_, 8));
    if (checked) {
        RECT dot = indicator;
        InflateRect(&dot, -Scale(parent_, 5), -Scale(parent_, 5));
        FillRoundedRectangle(draw.hDC, dot, RGB(255, 255, 255),
                             RGB(255, 255, 255), Scale(parent_, 4));
    }

    RECT label{indicator.right + Scale(parent_, 12), row.top,
               row.right - Scale(parent_, 12), row.bottom};
    if (dropdownKind_ == DropdownKind::DisplayMode &&
        draw.itemID < dropdownDescriptions_.size()) {
        label.bottom = row.top + (row.bottom - row.top) / 2 + Scale(parent_, 4);
        DrawTextLine(draw.hDC, dropdownLabels_[draw.itemID], label, bodyFont_,
                     kTextPrimary,
                     DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT description = label;
        description.top = label.bottom - Scale(parent_, 2);
        description.bottom = row.bottom;
        DrawTextLine(draw.hDC, dropdownDescriptions_[draw.itemID], description,
                     smallFont_, kTextSecondary,
                     DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        DrawTextLine(draw.hDC, dropdownLabels_[draw.itemID], label, bodyFont_,
                     kTextPrimary,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

HBRUSH ModernMainWindow::ColorControl(const HDC deviceContext, const HWND control) const {
    SetBkMode(deviceContext, TRANSPARENT);
    if (control == search_ || control == renameEdit_ ||
        control == groupRenameEdit_) {
        SetTextColor(deviceContext, kTextPrimary);
        SetBkColor(deviceContext, kPanel);
        return editBrush_;
    }
    if (control == groupList_) {
        SetTextColor(deviceContext, kTextPrimary);
        SetBkColor(deviceContext, kSidebar);
        return sidebarBrush_;
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
    if (controlId == Filter) {
        if (notificationCode == BN_CLICKED || notificationCode == BN_DBLCLK) {
            ShowFilterMenu();
        }
        return true;
    }
    if (controlId == DisplayMode) {
        if (notificationCode == BN_CLICKED || notificationCode == BN_DBLCLK) {
            ShowDisplayModeMenu();
        }
        return true;
    }
    if (controlId == ActiveStatus) {
        if (notificationCode == BN_CLICKED || notificationCode == BN_DBLCLK) {
            ToggleActiveDrawer();
        }
        return true;
    }
    if (controlId == GroupAll &&
        (notificationCode == BN_CLICKED || notificationCode == BN_DBLCLK)) {
        SelectGroup(AllGroupId);
        return true;
    }
    if (controlId == GroupFavorites &&
        (notificationCode == BN_CLICKED || notificationCode == BN_DBLCLK)) {
        SelectGroup(FavoritesGroupId);
        return true;
    }
    if (controlId == GroupList && notificationCode == LBN_SELCHANGE) {
        const LRESULT selection = SendMessageW(groupList_, LB_GETCURSEL, 0, 0);
        if (selection >= 0 && static_cast<std::size_t>(selection) < groups_.size()) {
            SelectGroup(groups_[static_cast<std::size_t>(selection)].id);
        }
        return true;
    }
    // The focused list emits LBN_SETFOCUS/LBN_KILLFOCUS through WM_COMMAND.
    // These belong to the custom dropdown itself. Letting them fall through to
    // the application's generic command handler immediately closes a dropdown
    // opened by a real foreground mouse click.
    if (controlId == DropdownList) {
        if (notificationCode == LBN_KILLFOCUS &&
            dropdownKind_ != DropdownKind::None) {
            const HWND anchor = dropdownKind_ == DropdownKind::Filter
                                    ? filter_
                                    : displayMode_;
            const HWND nextFocus = GetFocus();
            if (nextFocus != dropdownList_ && nextFocus != anchor) {
                HideDropdown();
            }
        }
        return true;
    }
    return false;
}

bool ModernMainWindow::ShowFilterMenu() {
    return ToggleDropdown(DropdownKind::Filter);
}

bool ModernMainWindow::ShowDisplayModeMenu() {
    return ToggleDropdown(DropdownKind::DisplayMode);
}

bool ModernMainWindow::ToggleDropdown(const DropdownKind kind) {
    if (!IsWindow(dropdownList_)) {
        return false;
    }
    if (dropdownKind_ == kind && IsWindowVisible(dropdownList_)) {
        HideDropdown();
        return false;
    }

    dropdownKind_ = kind;
    dropdownLabels_.clear();
    dropdownDescriptions_.clear();
    if (kind == DropdownKind::Filter) {
        dropdownLabels_ = {L"全部", L"图片", L"GIF", L"视频"};
    } else {
        dropdownLabels_ = {L"跨屏扩展", L"分屏显示"};
        dropdownDescriptions_ = {L"一个画面横跨全部屏幕",
                                 L"应用时选择一个或多个屏幕"};
    }

    SendMessageW(dropdownList_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(dropdownList_, LB_RESETCONTENT, 0, 0);
    for (const std::wstring& label : dropdownLabels_) {
        SendMessageW(dropdownList_, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(label.c_str()));
    }
    const int selection = kind == DropdownKind::Filter
                              ? static_cast<int>(filterKind_)
                              : (spanAcrossDisplays_ ? 0 : 1);
    SendMessageW(dropdownList_, LB_SETCURSEL, selection, 0);
    SendMessageW(dropdownList_, WM_SETREDRAW, TRUE, 0);
    dropdownHoverIndex_ = -1;
    dropdownHoverProgress_ = 0.0F;
    dropdownHoverTarget_ = false;
    Layout();
    InvalidateRect(dropdownList_, nullptr, FALSE);
    InvalidateRect(kind == DropdownKind::Filter ? filter_ : displayMode_, nullptr,
                   FALSE);
    StartHoverAnimation();
    return true;
}

void ModernMainWindow::SelectDropdownItem(const std::size_t index) {
    if (dropdownKind_ == DropdownKind::None || index >= dropdownLabels_.size()) {
        return;
    }
    const DropdownKind kind = dropdownKind_;
    HideDropdown();
    if (kind == DropdownKind::Filter) {
        filterKind_ = static_cast<FilterKind>(index);
        UpdateFilterSelectorText();
        RefreshVisibleItems();
        return;
    }

    const bool nextSpan = index == 0;
    if (nextSpan == spanAcrossDisplays_) {
        return;
    }
    spanAcrossDisplays_ = nextSpan;
    UpdateDisplayModeText();
    PostMessageW(parent_, WM_COMMAND,
                 MAKEWPARAM(DisplayModeChanged, BN_CLICKED),
                 reinterpret_cast<LPARAM>(displayMode_));
}

void ModernMainWindow::HideDropdown() {
    if (dropdownKind_ == DropdownKind::None) {
        return;
    }
    const DropdownKind previous = dropdownKind_;
    dropdownKind_ = DropdownKind::None;
    ShowWindow(dropdownList_, SW_HIDE);
    InvalidateRect(previous == DropdownKind::Filter ? filter_ : displayMode_,
                   nullptr, FALSE);
}

void ModernMainWindow::CloseTransientUi() {
    HideDropdown();
}

std::optional<std::vector<std::wstring>>
ModernMainWindow::ChooseDisplayTargets() {
    const auto selected = ShowScreenSelectionDialog(
        parent_, reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent_, GWLP_HINSTANCE)),
        displayOptions_);
    if (!selected.has_value()) {
        return std::nullopt;
    }
    for (DisplayOption& option : displayOptions_) {
        option.selected = std::ranges::any_of(
            *selected, [&](const std::wstring& identifier) {
                return _wcsicmp(identifier.c_str(), option.id.c_str()) == 0;
            });
    }
    return selected;
}

std::optional<ModernMainWindow::ImportChoice>
ModernMainWindow::ChooseImportSource() {
    CloseTransientUi();
    return ShowImportChoiceDialog(
        parent_, reinterpret_cast<HINSTANCE>(
                     GetWindowLongPtrW(parent_, GWLP_HINSTANCE)));
}

void ModernMainWindow::SetItems(std::vector<core::WallpaperItem> items) {
    std::unordered_set<std::wstring> currentPaths;
    for (const core::WallpaperItem& item : items) {
        currentPaths.insert(item.path.native());
    }
    for (auto thumbnail = thumbnails_.begin(); thumbnail != thumbnails_.end();) {
        if (!currentPaths.contains(thumbnail->first)) {
            if (thumbnail->second != nullptr) {
                DeleteObject(thumbnail->second);
            }
            thumbnail = thumbnails_.erase(thumbnail);
        } else {
            ++thumbnail;
        }
    }
    std::erase_if(exportSelectedPaths_, [&](const std::wstring& path) {
        return !currentPaths.contains(path);
    });
    items_ = std::move(items);
    for (const core::WallpaperItem& item : items_) {
        const std::wstring key = item.path.native();
        if (!thumbnails_.contains(key)) {
            thumbnails_.emplace(key, LoadThumbnail(key));
        }
    }
    RefreshVisibleItems();
    RefreshActiveItems();
    UpdateExportSelectionControls();
    InvalidateRect(groupAll_, nullptr, FALSE);
}

void ModernMainWindow::SetGroups(std::vector<core::WallpaperGroup> groups,
                                 std::vector<std::wstring> favorites,
                                 const std::wstring_view selectedGroupId) {
    groups_ = std::move(groups);
    favorites_ = std::move(favorites);
    if (!selectedGroupId.empty()) {
        currentGroupId_ = selectedGroupId;
    }
    const bool currentExists = CurrentGroupIsAll() || CurrentGroupIsFavorites() ||
        std::ranges::any_of(groups_, [&](const core::WallpaperGroup& group) {
            return _wcsicmp(group.id.c_str(), currentGroupId_.c_str()) == 0;
        });
    if (!currentExists) {
        currentGroupId_ = AllGroupId;
    }
    RefreshGroupItems();
    RefreshVisibleItems();
    InvalidateRect(groupAll_, nullptr, FALSE);
    InvalidateRect(groupFavorites_, nullptr, FALSE);
}

void ModernMainWindow::SetActiveWallpapers(
    std::vector<ActiveWallpaperInfo> wallpapers) {
    activeWallpapers_ = std::move(wallpapers);
    EnableWindow(activeStatus_, !activeWallpapers_.empty());
    RefreshActiveItems();
    if (activeWallpapers_.empty()) {
        HideActiveDrawer();
    }
    InvalidateRect(library_, nullptr, FALSE);
    InvalidateRect(activeStatus_, nullptr, FALSE);
}

const ModernMainWindow::ActiveWallpaperInfo*
ModernMainWindow::FindActiveWallpaper(const std::wstring_view path) const {
    const std::wstring requestedPath(path);
    const auto found = std::ranges::find_if(
        activeWallpapers_, [&](const ActiveWallpaperInfo& wallpaper) {
            return _wcsicmp(wallpaper.path.c_str(), requestedPath.c_str()) == 0;
        });
    return found == activeWallpapers_.end() ? nullptr : &*found;
}

void ModernMainWindow::SetStatus(std::wstring status) {
    if (status_ == status) {
        return;
    }
    status_ = std::move(status);
    InvalidateFooter();
    InvalidateRect(activeStatus_, nullptr, FALSE);
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

    UpdateDisplayModeText();
    Layout();
}

void ModernMainWindow::UpdateFilterSelectorText() {
    constexpr std::array labels{L"全部", L"图片", L"GIF", L"视频"};
    const std::wstring text =
        std::wstring(L"分类：") +
        labels[static_cast<std::size_t>(filterKind_)];
    SetWindowTextW(filter_, text.c_str());
    InvalidateRect(filter_, nullptr, FALSE);
}

void ModernMainWindow::UpdateDisplayModeText() {
    const wchar_t* text = spanAcrossDisplays_ ? L"显示方式：跨屏扩展"
                                             : L"显示方式：分屏显示";
    SetWindowTextW(displayMode_, text);
    InvalidateRect(displayMode_, nullptr, FALSE);
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
    RECT resource{Scale(parent_, 14), client.bottom - Scale(parent_, 80),
                  Scale(parent_, 202), client.bottom - Scale(parent_, 14)};
    InvalidateRect(parent_, &resource, FALSE);
    InvalidateRect(activeStatus_, nullptr, FALSE);
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

std::optional<core::WallpaperItem> ModernMainWindow::SelectedActiveItem() const {
    const LRESULT selection = SendMessageW(activeList_, LB_GETCURSEL, 0, 0);
    if (selection < 0 ||
        static_cast<std::size_t>(selection) >= activeVisibleIndices_.size()) {
        return std::nullopt;
    }
    return items_[activeVisibleIndices_[static_cast<std::size_t>(selection)]];
}

void ModernMainWindow::BeginExportSelection(
    const std::wstring_view initiallySelectedPath) {
    if (exportSelectionMode_) {
        return;
    }
    CloseTransientUi();
    HideActiveDrawer();
    CancelRename();
    exportSelectionMode_ = true;
    exportSelectedPaths_.clear();
    if (!initiallySelectedPath.empty()) {
        exportSelectedPaths_.emplace(initiallySelectedPath);
    }
    UpdateExportSelectionControls();
    Layout();
    InvalidateRect(library_, nullptr, FALSE);
}

void ModernMainWindow::EndExportSelection() {
    if (!exportSelectionMode_) {
        return;
    }
    exportSelectionMode_ = false;
    exportSelectedPaths_.clear();
    UpdateExportSelectionControls();
    Layout();
    InvalidateRect(library_, nullptr, FALSE);
}

void ModernMainWindow::SelectAllVisibleForExport() {
    if (!exportSelectionMode_) {
        return;
    }
    for (const std::size_t itemIndex : visibleIndices_) {
        exportSelectedPaths_.insert(items_[itemIndex].path.native());
    }
    UpdateExportSelectionControls();
    InvalidateRect(library_, nullptr, FALSE);
}

void ModernMainWindow::ClearExportSelection() {
    if (!exportSelectionMode_ || exportSelectedPaths_.empty()) {
        return;
    }
    exportSelectedPaths_.clear();
    UpdateExportSelectionControls();
    InvalidateRect(library_, nullptr, FALSE);
}

bool ModernMainWindow::ExportSelectionActive() const noexcept {
    return exportSelectionMode_;
}

std::vector<core::WallpaperItem> ModernMainWindow::SelectedExportItems() const {
    std::vector<core::WallpaperItem> selected;
    for (const core::WallpaperItem& item : items_) {
        if (exportSelectedPaths_.contains(item.path.native())) {
            selected.push_back(item);
        }
    }
    return selected;
}

std::optional<std::vector<core::WallpaperItem>>
ModernMainWindow::TakePendingLibraryOrder() {
    std::optional order = std::move(pendingLibraryOrder_);
    pendingLibraryOrder_.reset();
    return order;
}

void ModernMainWindow::UpdateExportSelectionControls() {
    if (exportConfirm_ == nullptr) {
        return;
    }
    const std::wstring label = L"批量操作（" +
                                std::to_wstring(exportSelectedPaths_.size()) +
                                L"）";
    SetWindowTextW(exportConfirm_, label.c_str());
    EnableWindow(exportConfirm_, !exportSelectedPaths_.empty());
    EnableWindow(exportClearAll_, !exportSelectedPaths_.empty());
    EnableWindow(exportSelectAll_, !visibleIndices_.empty());
    InvalidateRect(exportConfirm_, nullptr, FALSE);
    InvalidateRect(exportClearAll_, nullptr, FALSE);
    InvalidateRect(exportSelectAll_, nullptr, FALSE);
}

std::wstring ModernMainWindow::CurrentGroupId() const {
    return currentGroupId_;
}

bool ModernMainWindow::CurrentGroupIsAll() const noexcept {
    return _wcsicmp(currentGroupId_.c_str(), L"all") == 0;
}

bool ModernMainWindow::CurrentGroupIsFavorites() const noexcept {
    return _wcsicmp(currentGroupId_.c_str(), L"favorites") == 0;
}

std::optional<core::WallpaperGroup> ModernMainWindow::SelectedCustomGroup() const {
    const auto found = std::ranges::find_if(
        groups_, [&](const core::WallpaperGroup& group) {
            return _wcsicmp(group.id.c_str(), currentGroupId_.c_str()) == 0;
        });
    return found == groups_.end() ? std::nullopt
                                  : std::optional<core::WallpaperGroup>(*found);
}

std::optional<std::vector<std::wstring>>
ModernMainWindow::TakePendingGroupOrder() {
    std::optional order = std::move(pendingGroupOrder_);
    pendingGroupOrder_.reset();
    return order;
}

bool ModernMainWindow::SelectCustomGroupAtScreenPoint(POINT screenPoint) {
    if (!ScreenToClient(groupList_, &screenPoint)) {
        return false;
    }
    const LRESULT hit = SendMessageW(groupList_, LB_ITEMFROMPOINT, 0,
                                     MAKELPARAM(screenPoint.x, screenPoint.y));
    const UINT index = LOWORD(hit);
    if (HIWORD(hit) != 0 || index >= groups_.size()) {
        return false;
    }
    SendMessageW(groupList_, LB_SETCURSEL, index, 0);
    SelectGroup(groups_[index].id);
    return true;
}

void ModernMainWindow::BeginRenameSelectedGroup() {
    const auto selected = SelectedCustomGroup();
    if (!selected.has_value()) {
        return;
    }
    const auto found = std::ranges::find_if(groups_, [&](const auto& group) {
        return _wcsicmp(group.id.c_str(), selected->id.c_str()) == 0;
    });
    if (found == groups_.end()) {
        return;
    }
    const LRESULT index = std::distance(groups_.begin(), found);
    RECT item{};
    if (SendMessageW(groupList_, LB_GETITEMRECT, index,
                     reinterpret_cast<LPARAM>(&item)) == LB_ERR) {
        return;
    }
    MapWindowPoints(groupList_, parent_, reinterpret_cast<POINT*>(&item), 2);
    renamingGroupId_ = selected->id;
    SetWindowTextW(groupRenameEdit_, selected->name.c_str());
    SetWindowPos(groupRenameEdit_, HWND_TOP, item.left + Scale(parent_, 8),
                 item.top + Scale(parent_, 5),
                 std::max(1, static_cast<int>(item.right - item.left) -
                                 Scale(parent_, 16)),
                 Scale(parent_, 32), SWP_SHOWWINDOW);
    SetFocus(groupRenameEdit_);
    SendMessageW(groupRenameEdit_, EM_SETSEL, 0, -1);
}

std::optional<std::pair<std::wstring, std::wstring>>
ModernMainWindow::FinishGroupRename() {
    if (renamingGroupId_.empty() || !IsWindowVisible(groupRenameEdit_)) {
        return std::nullopt;
    }
    wchar_t name[128]{};
    GetWindowTextW(groupRenameEdit_, name, static_cast<int>(std::size(name)));
    ShowWindow(groupRenameEdit_, SW_HIDE);
    const std::wstring id = std::exchange(renamingGroupId_, {});
    if (name[0] == L'\0') {
        return std::nullopt;
    }
    return std::pair{id, std::wstring(name)};
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

bool ModernMainWindow::SelectActiveItemAtScreenPoint(const POINT screenPoint) {
    POINT clientPoint = screenPoint;
    if (!ScreenToClient(activeList_, &clientPoint)) {
        return false;
    }
    const LRESULT itemFromPoint = SendMessageW(
        activeList_, LB_ITEMFROMPOINT, 0,
        MAKELPARAM(clientPoint.x, clientPoint.y));
    const UINT index = LOWORD(itemFromPoint);
    const bool outside = HIWORD(itemFromPoint) != 0;
    if (outside || index >= activeVisibleIndices_.size()) {
        return false;
    }
    SendMessageW(activeList_, LB_SETCURSEL, index, 0);
    InvalidateRect(activeList_, nullptr, FALSE);
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
    BeginRenameItem(item, library_, selection);
}

void ModernMainWindow::BeginRenameActiveSelected() {
    const LRESULT selection = SendMessageW(activeList_, LB_GETCURSEL, 0, 0);
    if (selection < 0 ||
        static_cast<std::size_t>(selection) >= activeVisibleIndices_.size()) {
        return;
    }
    const core::WallpaperItem& item =
        items_[activeVisibleIndices_[static_cast<std::size_t>(selection)]];
    BeginRenameItem(item, activeList_, selection);
}

void ModernMainWindow::BeginRenameItem(const core::WallpaperItem& item,
                                       const HWND list,
                                       const LRESULT selection) {
    RECT itemRectangle{};
    if (SendMessageW(list, LB_GETITEMRECT, selection,
                     reinterpret_cast<LPARAM>(&itemRectangle)) == LB_ERR) {
        return;
    }
    MapWindowPoints(list, parent_, reinterpret_cast<POINT*>(&itemRectangle), 2);
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

HWND ModernMainWindow::ActiveLibraryControl() const noexcept {
    return activeList_;
}

HWND ModernMainWindow::GroupListControl() const noexcept {
    return groupList_;
}

HWND ModernMainWindow::BatchActionsControl() const noexcept {
    return exportConfirm_;
}

std::uint64_t ModernMainWindow::LibraryDrawCount() const noexcept {
    return libraryDrawCount_;
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

void ModernMainWindow::BeginLibraryDrag(const POINT clientPoint) {
    if (exportSelectionMode_) {
        return;
    }
    const LRESULT hit = SendMessageW(
        library_, LB_ITEMFROMPOINT, 0,
        MAKELPARAM(clientPoint.x, clientPoint.y));
    const UINT visibleIndex = LOWORD(hit);
    if (HIWORD(hit) != 0 || visibleIndex >= visibleIndices_.size() ||
        items_[visibleIndices_[visibleIndex]].external) {
        libraryDragSourceVisibleIndex_ = -1;
        return;
    }
    libraryDragStart_ = clientPoint;
    libraryDragSourceVisibleIndex_ = static_cast<int>(visibleIndex);
    libraryDragTargetVisibleIndex_ = static_cast<int>(visibleIndex);
    libraryDragInsertAfter_ = false;
    libraryDragActive_ = false;
    libraryDragScrollDirection_ = 0;
}

void ModernMainWindow::UpdateLibraryDrag(const POINT clientPoint) {
    if (libraryDragSourceVisibleIndex_ < 0 || exportSelectionMode_) {
        return;
    }
    if (!libraryDragActive_) {
        const int thresholdX = std::max(4, GetSystemMetrics(SM_CXDRAG));
        const int thresholdY = std::max(4, GetSystemMetrics(SM_CYDRAG));
        if (std::abs(clientPoint.x - libraryDragStart_.x) < thresholdX &&
            std::abs(clientPoint.y - libraryDragStart_.y) < thresholdY) {
            return;
        }
        if (GetCapture() != library_) {
            SetCapture(library_);
        }
        libraryDragActive_ = true;
        SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
    }

    RECT client{};
    GetClientRect(library_, &client);
    const int edge = Scale(parent_, 34);
    const int nextScrollDirection = clientPoint.y < client.top + edge
                                        ? -1
                                        : (clientPoint.y > client.bottom - edge
                                               ? 1
                                               : 0);
    if (nextScrollDirection != libraryDragScrollDirection_) {
        libraryDragScrollDirection_ = nextScrollDirection;
        if (libraryDragScrollDirection_ == 0) {
            KillTimer(library_, DragScrollTimerId);
        } else {
            SetTimer(library_, DragScrollTimerId, 60, nullptr);
        }
    }
    UpdateLibraryDragTarget(clientPoint);
}

void ModernMainWindow::UpdateLibraryDragTarget(POINT clientPoint) {
    if (!libraryDragActive_ || visibleIndices_.empty()) {
        return;
    }
    RECT client{};
    GetClientRect(library_, &client);
    clientPoint.x = std::clamp(clientPoint.x, client.left,
                               std::max(client.left, client.right - 1));
    clientPoint.y = std::clamp(clientPoint.y, client.top,
                               std::max(client.top, client.bottom - 1));
    const LRESULT hit = SendMessageW(
        library_, LB_ITEMFROMPOINT, 0,
        MAKELPARAM(clientPoint.x, clientPoint.y));
    const UINT visibleIndex = LOWORD(hit);
    if (visibleIndex >= visibleIndices_.size() ||
        items_[visibleIndices_[visibleIndex]].external) {
        return;
    }
    RECT itemRectangle{};
    if (SendMessageW(library_, LB_GETITEMRECT, visibleIndex,
                     reinterpret_cast<LPARAM>(&itemRectangle)) == LB_ERR) {
        return;
    }
    const bool insertAfter =
        clientPoint.y >= (itemRectangle.top + itemRectangle.bottom) / 2;
    if (libraryDragTargetVisibleIndex_ == static_cast<int>(visibleIndex) &&
        libraryDragInsertAfter_ == insertAfter) {
        return;
    }
    const int previousTarget = libraryDragTargetVisibleIndex_;
    libraryDragTargetVisibleIndex_ = static_cast<int>(visibleIndex);
    libraryDragInsertAfter_ = insertAfter;
    for (const int target : {previousTarget, libraryDragTargetVisibleIndex_}) {
        RECT rectangle{};
        if (target >= 0 &&
            SendMessageW(library_, LB_GETITEMRECT, target,
                         reinterpret_cast<LPARAM>(&rectangle)) != LB_ERR) {
            InvalidateRect(library_, &rectangle, FALSE);
        }
    }
}

void ModernMainWindow::ScrollLibraryDuringDrag() {
    if (!libraryDragActive_ || libraryDragScrollDirection_ == 0) {
        return;
    }
    const int count = static_cast<int>(
        SendMessageW(library_, LB_GETCOUNT, 0, 0));
    const int top = static_cast<int>(
        SendMessageW(library_, LB_GETTOPINDEX, 0, 0));
    RECT client{};
    GetClientRect(library_, &client);
    const int rowHeight = std::max(
        1, static_cast<int>(SendMessageW(library_, LB_GETITEMHEIGHT, 0, 0)));
    const int visibleRows =
        std::max(1, (static_cast<int>(client.bottom - client.top) + rowHeight - 1) /
                        rowHeight);
    const int maximumTop = std::max(0, count - visibleRows);
    const int next = std::clamp(top + libraryDragScrollDirection_, 0,
                                maximumTop);
    if (next != top) {
        SendMessageW(library_, LB_SETTOPINDEX, next, 0);
        InvalidateRect(library_, nullptr, FALSE);
    }
    POINT cursor{};
    GetCursorPos(&cursor);
    ScreenToClient(library_, &cursor);
    UpdateLibraryDragTarget(cursor);
}

void ModernMainWindow::FinishLibraryDrag(const POINT clientPoint) {
    if (!libraryDragActive_ || libraryDragSourceVisibleIndex_ < 0 ||
        libraryDragTargetVisibleIndex_ < 0 ||
        static_cast<std::size_t>(libraryDragSourceVisibleIndex_) >=
            visibleIndices_.size() ||
        static_cast<std::size_t>(libraryDragTargetVisibleIndex_) >=
            visibleIndices_.size()) {
        CancelLibraryDrag();
        return;
    }
    UpdateLibraryDragTarget(clientPoint);
    const std::size_t sourceIndex =
        visibleIndices_[static_cast<std::size_t>(libraryDragSourceVisibleIndex_)];
    const std::size_t targetIndex =
        visibleIndices_[static_cast<std::size_t>(libraryDragTargetVisibleIndex_)];
    std::vector<core::WallpaperItem> reordered = items_;
    core::WallpaperItem moving = reordered[sourceIndex];
    reordered.erase(reordered.begin() + static_cast<std::ptrdiff_t>(sourceIndex));
    std::size_t insertion = targetIndex + (libraryDragInsertAfter_ ? 1U : 0U);
    if (sourceIndex < insertion) {
        --insertion;
    }
    insertion = std::min(insertion, reordered.size());
    reordered.insert(reordered.begin() + static_cast<std::ptrdiff_t>(insertion),
                     std::move(moving));

    std::vector<core::WallpaperItem> localOrder;
    for (const core::WallpaperItem& item : reordered) {
        if (!item.external) {
            localOrder.push_back(item);
        }
    }
    const bool changed = sourceIndex != insertion;
    CancelLibraryDrag();
    if (changed) {
        pendingLibraryOrder_ = std::move(localOrder);
        PostMessageW(parent_, WM_COMMAND,
                     MAKEWPARAM(LibraryReordered, BN_CLICKED),
                     reinterpret_cast<LPARAM>(library_));
    }
}

void ModernMainWindow::CancelLibraryDrag() {
    const int previousTarget = libraryDragTargetVisibleIndex_;
    libraryDragSourceVisibleIndex_ = -1;
    libraryDragTargetVisibleIndex_ = -1;
    libraryDragScrollDirection_ = 0;
    libraryDragActive_ = false;
    libraryDragInsertAfter_ = false;
    KillTimer(library_, DragScrollTimerId);
    if (GetCapture() == library_) {
        ReleaseCapture();
    }
    RECT rectangle{};
    if (previousTarget >= 0 &&
        SendMessageW(library_, LB_GETITEMRECT, previousTarget,
                     reinterpret_cast<LPARAM>(&rectangle)) != LB_ERR) {
        InvalidateRect(library_, &rectangle, FALSE);
    }
}

void ModernMainWindow::BeginGroupDrag(const POINT clientPoint) {
    const LRESULT hit = SendMessageW(groupList_, LB_ITEMFROMPOINT, 0,
                                     MAKELPARAM(clientPoint.x, clientPoint.y));
    const UINT index = LOWORD(hit);
    if (HIWORD(hit) != 0 || index >= groups_.size()) {
        groupDragSourceIndex_ = -1;
        return;
    }
    groupDragStart_ = clientPoint;
    groupDragSourceIndex_ = static_cast<int>(index);
    groupDragTargetIndex_ = static_cast<int>(index);
    groupDragScrollDirection_ = 0;
    groupDragActive_ = false;
    groupDragInsertAfter_ = false;
}

void ModernMainWindow::UpdateGroupDrag(const POINT clientPoint) {
    if (groupDragSourceIndex_ < 0) {
        return;
    }
    if (!groupDragActive_) {
        if (std::abs(clientPoint.x - groupDragStart_.x) <
                std::max(4, GetSystemMetrics(SM_CXDRAG)) &&
            std::abs(clientPoint.y - groupDragStart_.y) <
                std::max(4, GetSystemMetrics(SM_CYDRAG))) {
            return;
        }
        SetCapture(groupList_);
        groupDragActive_ = true;
        SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
    }
    RECT client{};
    GetClientRect(groupList_, &client);
    const int edge = Scale(parent_, 28);
    const int nextDirection = clientPoint.y < client.top + edge
                                  ? -1
                                  : (clientPoint.y > client.bottom - edge ? 1 : 0);
    if (nextDirection != groupDragScrollDirection_) {
        groupDragScrollDirection_ = nextDirection;
        if (nextDirection == 0) {
            KillTimer(groupList_, GroupDragScrollTimerId);
        } else {
            SetTimer(groupList_, GroupDragScrollTimerId, 60, nullptr);
        }
    }
    UpdateGroupDragTarget(clientPoint);
}

void ModernMainWindow::UpdateGroupDragTarget(POINT clientPoint) {
    if (!groupDragActive_ || groups_.empty()) {
        return;
    }
    RECT client{};
    GetClientRect(groupList_, &client);
    clientPoint.x = std::clamp(clientPoint.x, client.left,
                               std::max(client.left, client.right - 1));
    clientPoint.y = std::clamp(clientPoint.y, client.top,
                               std::max(client.top, client.bottom - 1));
    const LRESULT hit = SendMessageW(groupList_, LB_ITEMFROMPOINT, 0,
                                     MAKELPARAM(clientPoint.x, clientPoint.y));
    const UINT index = LOWORD(hit);
    if (index >= groups_.size()) {
        return;
    }
    RECT item{};
    if (SendMessageW(groupList_, LB_GETITEMRECT, index,
                     reinterpret_cast<LPARAM>(&item)) == LB_ERR) {
        return;
    }
    const bool insertAfter = clientPoint.y >= (item.top + item.bottom) / 2;
    if (groupDragTargetIndex_ == static_cast<int>(index) &&
        groupDragInsertAfter_ == insertAfter) {
        return;
    }
    groupDragTargetIndex_ = static_cast<int>(index);
    groupDragInsertAfter_ = insertAfter;
    InvalidateRect(groupList_, nullptr, FALSE);
}

void ModernMainWindow::FinishGroupDrag(const POINT clientPoint) {
    UpdateGroupDragTarget(clientPoint);
    if (!groupDragActive_ || groupDragSourceIndex_ < 0 ||
        groupDragTargetIndex_ < 0 ||
        static_cast<std::size_t>(groupDragSourceIndex_) >= groups_.size() ||
        static_cast<std::size_t>(groupDragTargetIndex_) >= groups_.size()) {
        CancelGroupDrag();
        return;
    }
    std::vector<std::wstring> order;
    order.reserve(groups_.size());
    for (const auto& group : groups_) {
        order.push_back(group.id);
    }
    std::wstring moving = order[static_cast<std::size_t>(groupDragSourceIndex_)];
    order.erase(order.begin() + groupDragSourceIndex_);
    std::size_t insertion = static_cast<std::size_t>(groupDragTargetIndex_) +
                            (groupDragInsertAfter_ ? 1U : 0U);
    if (static_cast<std::size_t>(groupDragSourceIndex_) < insertion) {
        --insertion;
    }
    insertion = std::min(insertion, order.size());
    const bool changed = insertion != static_cast<std::size_t>(groupDragSourceIndex_);
    order.insert(order.begin() + static_cast<std::ptrdiff_t>(insertion),
                 std::move(moving));
    CancelGroupDrag();
    if (changed) {
        pendingGroupOrder_ = std::move(order);
        PostMessageW(parent_, WM_COMMAND,
                     MAKEWPARAM(GroupReordered, BN_CLICKED),
                     reinterpret_cast<LPARAM>(groupList_));
    }
}

void ModernMainWindow::CancelGroupDrag() {
    groupDragSourceIndex_ = -1;
    groupDragTargetIndex_ = -1;
    groupDragScrollDirection_ = 0;
    groupDragActive_ = false;
    groupDragInsertAfter_ = false;
    KillTimer(groupList_, GroupDragScrollTimerId);
    if (GetCapture() == groupList_) {
        ReleaseCapture();
    }
    InvalidateRect(groupList_, nullptr, FALSE);
}

void ModernMainWindow::ScrollGroupsDuringDrag() {
    if (!groupDragActive_ || groupDragScrollDirection_ == 0) {
        return;
    }
    const int count = static_cast<int>(groups_.size());
    const int top = static_cast<int>(SendMessageW(groupList_, LB_GETTOPINDEX, 0, 0));
    RECT client{};
    GetClientRect(groupList_, &client);
    const int rowHeight = std::max(
        1, static_cast<int>(SendMessageW(groupList_, LB_GETITEMHEIGHT, 0, 0)));
    const int visibleRows = std::max(
        1, (static_cast<int>(client.bottom) + rowHeight - 1) / rowHeight);
    const int next = std::clamp(top + groupDragScrollDirection_, 0,
                                std::max(0, count - visibleRows));
    if (next != top) {
        SendMessageW(groupList_, LB_SETTOPINDEX, next, 0);
        InvalidateRect(groupList_, nullptr, FALSE);
    }
    POINT cursor{};
    GetCursorPos(&cursor);
    ScreenToClient(groupList_, &cursor);
    UpdateGroupDragTarget(cursor);
}

LRESULT CALLBACK ModernMainWindow::RenameEditProcedure(
    const HWND window, const UINT message, const WPARAM wParam, const LPARAM lParam,
    const UINT_PTR, const DWORD_PTR referenceData) {
    auto* self = reinterpret_cast<ModernMainWindow*>(referenceData);
    if (self != nullptr && message == WM_KEYDOWN) {
        if (wParam == VK_RETURN) {
            PostMessageW(self->parent_, WM_COMMAND,
                         MAKEWPARAM(window == self->groupRenameEdit_
                                        ? GroupRenameCommit
                                        : RenameCommit,
                                    BN_CLICKED),
                         reinterpret_cast<LPARAM>(window));
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            if (window == self->groupRenameEdit_) {
                ShowWindow(self->groupRenameEdit_, SW_HIDE);
                self->renamingGroupId_.clear();
                SetFocus(self->groupList_);
            } else {
                self->CancelRename();
            }
            return 0;
        }
    }
    if (message == WM_GETDLGCODE) {
        return DLGC_WANTALLKEYS;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

LRESULT CALLBACK ModernMainWindow::InteractiveControlProcedure(
    const HWND window, const UINT message, const WPARAM wParam,
    const LPARAM lParam, const UINT_PTR, const DWORD_PTR referenceData) {
    auto* self = reinterpret_cast<ModernMainWindow*>(referenceData);
    if (self == nullptr) {
        return DefSubclassProc(window, message, wParam, lParam);
    }

    if (message == WM_LBUTTONDOWN && window == self->library_) {
        self->BeginLibraryDrag(
            POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
    } else if (message == WM_LBUTTONDOWN && window == self->groupList_) {
        self->BeginGroupDrag(
            POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
    } else if (message == WM_MOUSEMOVE) {
        POINT cursor{};
        if (GetCursorPos(&cursor) && WindowFromPoint(cursor) == window) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
            TrackMouseEvent(&tracking);
        }
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (window == self->library_ && (wParam & MK_LBUTTON) != 0 &&
            self->libraryDragSourceVisibleIndex_ >= 0) {
            self->UpdateLibraryDrag(point);
            if (self->libraryDragActive_) {
                return 0;
            }
        }
        if (window == self->groupList_ && (wParam & MK_LBUTTON) != 0 &&
            self->groupDragSourceIndex_ >= 0) {
            self->UpdateGroupDrag(point);
            if (self->groupDragActive_) {
                return 0;
            }
        }
        if (window == self->library_ || window == self->activeList_ ||
            window == self->dropdownList_ || window == self->groupList_) {
            self->SetListHovered(window, point, true);
            // The stock list box repaints owner-draw rows while processing
            // ordinary mouse movement even though selection did not change.
            // Our hover state already owns that visual update. Preserve the
            // native path only while the list has mouse capture for dragging.
            if (GetCapture() != window) {
                return 0;
            }
        } else {
            self->SetControlHovered(window, true);
        }
    } else if (message == WM_MOUSELEAVE) {
        if (window == self->library_ || window == self->activeList_ ||
            window == self->dropdownList_ || window == self->groupList_) {
            self->SetListHovered(window, POINT{}, false);
        } else {
            self->SetControlHovered(window, false);
        }
    } else if (message == WM_LBUTTONUP && window == self->library_) {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (self->libraryDragActive_) {
            self->FinishLibraryDrag(point);
            return 0;
        }
        self->CancelLibraryDrag();
        UINT index = 0;
        if (self->exportSelectionMode_) {
            const LRESULT hit = SendMessageW(
                window, LB_ITEMFROMPOINT, 0, MAKELPARAM(point.x, point.y));
            index = LOWORD(hit);
            if (HIWORD(hit) == 0 && index < self->visibleIndices_.size()) {
                SendMessageW(window, LB_SETCURSEL, index, 0);
                self->ToggleExportSelectionAt(index);
                return 0;
            }
        }
        if (self->HitLibraryActiveBadge(point, index)) {
            SendMessageW(window, LB_SETCURSEL, index, 0);
            InvalidateRect(window, nullptr, FALSE);
            PostMessageW(self->parent_, WM_COMMAND,
                         MAKEWPARAM(CancelSelectedWallpaper, BN_CLICKED),
                         reinterpret_cast<LPARAM>(window));
            return 0;
        }
    } else if (message == WM_LBUTTONUP && window == self->groupList_) {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (self->groupDragActive_) {
            self->FinishGroupDrag(point);
            return 0;
        }
        self->CancelGroupDrag();
    } else if (message == WM_LBUTTONUP && window == self->activeList_) {
        UINT index = 0;
        if (self->HitActiveCancelButton(
                POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}, index)) {
            SendMessageW(window, LB_SETCURSEL, index, 0);
            InvalidateRect(window, nullptr, FALSE);
            PostMessageW(self->parent_, WM_COMMAND,
                         MAKEWPARAM(CancelSelectedWallpaper, BN_CLICKED),
                         reinterpret_cast<LPARAM>(window));
            return 0;
        }
    } else if (message == WM_LBUTTONUP && window == self->dropdownList_) {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const LRESULT item = SendMessageW(
            window, LB_ITEMFROMPOINT, 0, MAKELPARAM(point.x, point.y));
        if (HIWORD(item) == 0) {
            self->SelectDropdownItem(LOWORD(item));
        }
        return 0;
    } else if (message == WM_KILLFOCUS &&
               ((window == self->filter_ &&
                 self->dropdownKind_ == DropdownKind::Filter) ||
                (window == self->displayMode_ &&
                 self->dropdownKind_ == DropdownKind::DisplayMode))) {
        if (reinterpret_cast<HWND>(wParam) != self->dropdownList_) {
            self->HideDropdown();
        }
    } else if (message == WM_KEYDOWN &&
               ((window == self->filter_ &&
                 self->dropdownKind_ == DropdownKind::Filter) ||
                (window == self->displayMode_ &&
                 self->dropdownKind_ == DropdownKind::DisplayMode))) {
        if (wParam == VK_RETURN) {
            const LRESULT selected =
                SendMessageW(self->dropdownList_, LB_GETCURSEL, 0, 0);
            if (selected >= 0) {
                self->SelectDropdownItem(static_cast<std::size_t>(selected));
            }
            return 0;
        }
        if (wParam == VK_UP || wParam == VK_DOWN || wParam == VK_HOME ||
            wParam == VK_END) {
            const int count = static_cast<int>(SendMessageW(
                self->dropdownList_, LB_GETCOUNT, 0, 0));
            int selected = static_cast<int>(SendMessageW(
                self->dropdownList_, LB_GETCURSEL, 0, 0));
            if (count > 0) {
                if (wParam == VK_HOME) {
                    selected = 0;
                } else if (wParam == VK_END) {
                    selected = count - 1;
                } else {
                    selected = std::clamp(
                        selected + (wParam == VK_DOWN ? 1 : -1), 0, count - 1);
                }
                SendMessageW(self->dropdownList_, LB_SETCURSEL, selected, 0);
                InvalidateRect(self->dropdownList_, nullptr, FALSE);
            }
            return 0;
        }
    } else if (message == WM_KEYDOWN && window == self->dropdownList_ &&
               wParam == VK_RETURN) {
        const LRESULT selected = SendMessageW(window, LB_GETCURSEL, 0, 0);
        if (selected >= 0) {
            self->SelectDropdownItem(static_cast<std::size_t>(selected));
        }
        return 0;
    } else if (message == WM_TIMER && window == self->library_ &&
               wParam == DragScrollTimerId) {
        self->ScrollLibraryDuringDrag();
        return 0;
    } else if (message == WM_TIMER && window == self->groupList_ &&
               wParam == GroupDragScrollTimerId) {
        self->ScrollGroupsDuringDrag();
        return 0;
    } else if (message == WM_CAPTURECHANGED && window == self->library_) {
        self->CancelLibraryDrag();
    } else if (message == WM_CAPTURECHANGED && window == self->groupList_) {
        self->CancelGroupDrag();
    } else if (message == WM_SETCURSOR && window == self->library_ &&
               self->libraryDragActive_) {
        SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
        return TRUE;
    } else if (message == WM_SETCURSOR && window == self->groupList_ &&
               self->groupDragActive_) {
        SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
        return TRUE;
    } else if (message == WM_KEYDOWN && wParam == VK_ESCAPE) {
        if (self->exportSelectionMode_) {
            self->EndExportSelection();
        }
        self->HideDropdown();
        return 0;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

void ModernMainWindow::SetControlHovered(const HWND control, const bool hovered) {
    const auto target = hoverTargets_.find(control);
    if (target == hoverTargets_.end() || target->second == hovered) {
        return;
    }
    target->second = hovered;
    StartHoverAnimation();
}

void ModernMainWindow::SetListHovered(const HWND list, const POINT clientPoint,
                                      const bool hovered) {
    int hitIndex = -1;
    if (hovered) {
        const LRESULT item = SendMessageW(
            list, LB_ITEMFROMPOINT, 0,
            MAKELPARAM(clientPoint.x, clientPoint.y));
        if (HIWORD(item) == 0) {
            hitIndex = LOWORD(item);
        }
    }

    int* hoveredIndex = nullptr;
    float* progress = nullptr;
    bool* target = nullptr;
    int* specialHoverIndex = nullptr;
    int nextSpecialHoverIndex = -1;
    if (list == library_) {
        hoveredIndex = &libraryHoverIndex_;
        progress = &libraryHoverProgress_;
        target = &libraryHoverTarget_;
        UINT badgeIndex = 0;
        specialHoverIndex = &libraryBadgeHoverIndex_;
        nextSpecialHoverIndex =
            hovered && HitLibraryActiveBadge(clientPoint, badgeIndex)
                ? static_cast<int>(badgeIndex)
                : -1;
    } else if (list == activeList_) {
        hoveredIndex = &activeHoverIndex_;
        progress = &activeHoverProgress_;
        target = &activeHoverTarget_;
        UINT cancelIndex = 0;
        specialHoverIndex = &activeCancelHoverIndex_;
        nextSpecialHoverIndex =
            hovered && HitActiveCancelButton(clientPoint, cancelIndex)
                ? static_cast<int>(cancelIndex)
                : -1;
    } else if (list == dropdownList_) {
        hoveredIndex = &dropdownHoverIndex_;
        progress = &dropdownHoverProgress_;
        target = &dropdownHoverTarget_;
    } else if (list == groupList_) {
        hoveredIndex = &groupHoverIndex_;
        progress = &groupHoverProgress_;
        target = &groupHoverTarget_;
    }

    if (hoveredIndex == nullptr) {
        return;
    }

    const auto invalidateItem = [&](const int index) {
        if (index < 0) {
            return;
        }
        RECT itemRectangle{};
        if (SendMessageW(list, LB_GETITEMRECT, index,
                         reinterpret_cast<LPARAM>(&itemRectangle)) != LB_ERR) {
            InvalidateRect(list, &itemRectangle, FALSE);
        }
    };

    bool animationChanged = false;
    if (hitIndex >= 0 && hitIndex != *hoveredIndex) {
        invalidateItem(*hoveredIndex);
        *hoveredIndex = hitIndex;
        *progress = 0.0F;
        animationChanged = true;
    }
    const bool nextTarget = hovered && hitIndex >= 0;
    if (*target != nextTarget) {
        *target = nextTarget;
        animationChanged = true;
    }

    if (specialHoverIndex != nullptr &&
        *specialHoverIndex != nextSpecialHoverIndex) {
        const int previousSpecial = *specialHoverIndex;
        *specialHoverIndex = nextSpecialHoverIndex;
        invalidateItem(previousSpecial);
        invalidateItem(nextSpecialHoverIndex);
    }
    if (!animationChanged) {
        return;
    }
    invalidateItem(*hoveredIndex);
    StartHoverAnimation();
}

void ModernMainWindow::StartHoverAnimation() {
    if (IsWindow(parent_)) {
        SetTimer(parent_, AnimationTimerId, 16, nullptr);
    }
}

void ModernMainWindow::HandleAnimationTimer() {
    bool animating = false;
    constexpr float step = 0.18F;
    for (auto& [control, progress] : hoverProgress_) {
        const bool target = hoverTargets_.contains(control) && hoverTargets_[control];
        const float next = std::clamp(progress + (target ? step : -step),
                                      0.0F, 1.0F);
        if (next != progress) {
            progress = next;
            InvalidateRect(control, nullptr, FALSE);
        }
        animating = animating || (target ? progress < 1.0F : progress > 0.0F);
    }

    const auto animateList = [&](const HWND list, float& progress,
                                 const bool target, int& index) {
        const float next = std::clamp(progress + (target ? step : -step),
                                      0.0F, 1.0F);
        if (next != progress) {
            progress = next;
            RECT itemRectangle{};
            if (index >= 0 &&
                SendMessageW(list, LB_GETITEMRECT, index,
                             reinterpret_cast<LPARAM>(&itemRectangle)) != LB_ERR) {
                InvalidateRect(list, &itemRectangle, FALSE);
            }
        }
        if (!target && progress == 0.0F) {
            index = -1;
        }
        return target ? progress < 1.0F : progress > 0.0F;
    };
    animating = animateList(library_, libraryHoverProgress_, libraryHoverTarget_,
                            libraryHoverIndex_) ||
                animating;
    animating = animateList(activeList_, activeHoverProgress_, activeHoverTarget_,
                            activeHoverIndex_) ||
                animating;
    animating = animateList(dropdownList_, dropdownHoverProgress_,
                             dropdownHoverTarget_, dropdownHoverIndex_) ||
                 animating;
    animating = animateList(groupList_, groupHoverProgress_, groupHoverTarget_,
                            groupHoverIndex_) ||
                animating;
    if (!animating && IsWindow(parent_)) {
        KillTimer(parent_, AnimationTimerId);
    }
}

float ModernMainWindow::ControlHoverProgress(const HWND control) const {
    const auto found = hoverProgress_.find(control);
    return found == hoverProgress_.end() ? 0.0F : found->second;
}

bool ModernMainWindow::HitLibraryActiveBadge(const POINT clientPoint,
                                             UINT& itemIndex) const {
    const LRESULT item = SendMessageW(
        library_, LB_ITEMFROMPOINT, 0,
        MAKELPARAM(clientPoint.x, clientPoint.y));
    itemIndex = LOWORD(item);
    if (HIWORD(item) != 0 || itemIndex >= visibleIndices_.size()) {
        return false;
    }
    const core::WallpaperItem& wallpaper = items_[visibleIndices_[itemIndex]];
    const bool active = FindActiveWallpaper(wallpaper.path.native()) != nullptr;
    RECT itemRectangle{};
    if (!active || SendMessageW(library_, LB_GETITEMRECT, itemIndex,
                                reinterpret_cast<LPARAM>(&itemRectangle)) == LB_ERR) {
        return false;
    }
    RECT card = itemRectangle;
    InflateRect(&card, -Scale(parent_, 4), -Scale(parent_, 5));
    const RECT badge{card.right - Scale(parent_, 92),
                     card.top + Scale(parent_, 20),
                     card.right - Scale(parent_, 14),
                     card.top + Scale(parent_, 48)};
    return PtInRect(&badge, clientPoint) != FALSE;
}

bool ModernMainWindow::HitLibraryExportCheckbox(const POINT clientPoint,
                                                UINT& itemIndex) const {
    if (!exportSelectionMode_) {
        return false;
    }
    const LRESULT item = SendMessageW(
        library_, LB_ITEMFROMPOINT, 0,
        MAKELPARAM(clientPoint.x, clientPoint.y));
    itemIndex = LOWORD(item);
    if (HIWORD(item) != 0 || itemIndex >= visibleIndices_.size()) {
        return false;
    }
    RECT itemRectangle{};
    if (SendMessageW(library_, LB_GETITEMRECT, itemIndex,
                     reinterpret_cast<LPARAM>(&itemRectangle)) == LB_ERR) {
        return false;
    }
    RECT card = itemRectangle;
    InflateRect(&card, -Scale(parent_, 4), -Scale(parent_, 5));
    const int boxSize = Scale(parent_, 20);
    const RECT box{card.left + Scale(parent_, 12),
                   card.top + (card.bottom - card.top - boxSize) / 2,
                   card.left + Scale(parent_, 12) + boxSize,
                   card.top + (card.bottom - card.top + boxSize) / 2};
    RECT hitBox = box;
    InflateRect(&hitBox, Scale(parent_, 6), Scale(parent_, 6));
    return PtInRect(&hitBox, clientPoint) != FALSE;
}

void ModernMainWindow::ToggleExportSelectionAt(const UINT visibleIndex) {
    if (!exportSelectionMode_ || visibleIndex >= visibleIndices_.size()) {
        return;
    }
    const std::wstring path = items_[visibleIndices_[visibleIndex]].path.native();
    if (exportSelectedPaths_.contains(path)) {
        exportSelectedPaths_.erase(path);
    } else {
        exportSelectedPaths_.insert(path);
    }
    RECT itemRectangle{};
    if (SendMessageW(library_, LB_GETITEMRECT, visibleIndex,
                     reinterpret_cast<LPARAM>(&itemRectangle)) != LB_ERR) {
        InvalidateRect(library_, &itemRectangle, FALSE);
    }
    UpdateExportSelectionControls();
}

bool ModernMainWindow::HitActiveCancelButton(const POINT clientPoint,
                                             UINT& itemIndex) const {
    const LRESULT item = SendMessageW(
        activeList_, LB_ITEMFROMPOINT, 0,
        MAKELPARAM(clientPoint.x, clientPoint.y));
    itemIndex = LOWORD(item);
    if (HIWORD(item) != 0 || itemIndex >= activeVisibleIndices_.size()) {
        return false;
    }
    RECT itemRectangle{};
    if (SendMessageW(activeList_, LB_GETITEMRECT, itemIndex,
                     reinterpret_cast<LPARAM>(&itemRectangle)) == LB_ERR) {
        return false;
    }
    RECT card = itemRectangle;
    InflateRect(&card, -Scale(parent_, 4), -Scale(parent_, 5));
    const RECT cancel{card.right - Scale(parent_, 112),
                      card.top + Scale(parent_, 17),
                      card.right - Scale(parent_, 14),
                      card.bottom - Scale(parent_, 17)};
    return PtInRect(&cancel, clientPoint) != FALSE;
}

void ModernMainWindow::RefreshVisibleItems() {
    wchar_t searchText[512]{};
    GetWindowTextW(search_, searchText, static_cast<int>(std::size(searchText)));
    const std::wstring search = Lowercase(searchText);

    visibleIndices_.clear();
    SendMessageW(library_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(library_, LB_RESETCONTENT, 0, 0);
    for (std::size_t index = 0; index < items_.size(); ++index) {
        if (!ItemBelongsToCurrentGroup(items_[index]) ||
            !MatchesFilter(items_[index], search)) {
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
    UpdateExportSelectionControls();
}

void ModernMainWindow::SelectGroup(const std::wstring_view groupId) {
    if (_wcsicmp(currentGroupId_.c_str(), std::wstring(groupId).c_str()) == 0) {
        return;
    }
    if (exportSelectionMode_) {
        EndExportSelection();
    }
    currentGroupId_ = groupId;
    if (CurrentGroupIsAll() || CurrentGroupIsFavorites()) {
        SendMessageW(groupList_, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    } else {
        const auto found = std::ranges::find_if(groups_, [&](const auto& group) {
            return _wcsicmp(group.id.c_str(), currentGroupId_.c_str()) == 0;
        });
        if (found != groups_.end()) {
            SendMessageW(groupList_, LB_SETCURSEL,
                         std::distance(groups_.begin(), found), 0);
        }
    }
    RefreshVisibleItems();
    InvalidateRect(groupAll_, nullptr, FALSE);
    InvalidateRect(groupFavorites_, nullptr, FALSE);
    InvalidateRect(groupList_, nullptr, FALSE);
    PostMessageW(parent_, WM_COMMAND, MAKEWPARAM(GroupChanged, BN_CLICKED), 0);
}

bool ModernMainWindow::ItemBelongsToCurrentGroup(
    const core::WallpaperItem& item) const {
    if (CurrentGroupIsAll()) {
        return true;
    }
    if (item.external) {
        return false;
    }
    const std::wstring fileName = item.path.filename().native();
    const auto containsName = [&](const std::vector<std::wstring>& values) {
        return std::ranges::any_of(values, [&](const std::wstring& value) {
            return _wcsicmp(value.c_str(), fileName.c_str()) == 0;
        });
    };
    if (CurrentGroupIsFavorites()) {
        return containsName(favorites_);
    }
    const auto group = std::ranges::find_if(groups_, [&](const auto& value) {
        return _wcsicmp(value.id.c_str(), currentGroupId_.c_str()) == 0;
    });
    return group != groups_.end() && containsName(group->fileNames);
}

void ModernMainWindow::RefreshGroupItems() {
    SendMessageW(groupList_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(groupList_, LB_RESETCONTENT, 0, 0);
    int selection = -1;
    for (std::size_t index = 0; index < groups_.size(); ++index) {
        SendMessageW(groupList_, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L""));
        if (_wcsicmp(groups_[index].id.c_str(), currentGroupId_.c_str()) == 0) {
            selection = static_cast<int>(index);
        }
    }
    SendMessageW(groupList_, LB_SETCURSEL, selection, 0);
    SendMessageW(groupList_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(groupList_, nullptr, TRUE);
}

void ModernMainWindow::RefreshActiveItems() {
    activeVisibleIndices_.clear();
    SendMessageW(activeList_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(activeList_, LB_RESETCONTENT, 0, 0);
    for (const ActiveWallpaperInfo& activeWallpaper : activeWallpapers_) {
        const auto item = std::ranges::find_if(
            items_, [&](const core::WallpaperItem& candidate) {
                return _wcsicmp(candidate.path.c_str(),
                                activeWallpaper.path.c_str()) == 0;
            });
        if (item == items_.end()) {
            continue;
        }
        const std::size_t index =
            static_cast<std::size_t>(std::distance(items_.begin(), item));
        if (SendMessageW(activeList_, LB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(L"")) >= 0) {
            activeVisibleIndices_.push_back(index);
        }
    }
    SendMessageW(activeList_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(activeList_, nullptr, TRUE);
    if (activeDrawerVisible_) {
        Layout();
    }
}

void ModernMainWindow::ToggleActiveDrawer() {
    if (activeVisibleIndices_.empty()) {
        return;
    }
    HideDropdown();
    activeDrawerVisible_ = !activeDrawerVisible_;
    Layout();
    InvalidateRect(activeStatus_, nullptr, FALSE);
}

void ModernMainWindow::HideActiveDrawer() {
    if (!activeDrawerVisible_) {
        return;
    }
    activeDrawerVisible_ = false;
    ShowWindow(activeList_, SW_HIDE);
    InvalidateRect(activeStatus_, nullptr, FALSE);
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
