#include "app/WallpaperApplication.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <mfapi.h>
#include <mferror.h>
#include <commctrl.h>
#include <powrprof.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <wtsapi32.h>
#include <windowsx.h>
#include <wrl/client.h>

#include "core/Logger.h"
#include "media/MediaProbe.h"
#include "media/video/VideoOptimizer.h"

namespace lwe::app {
namespace {

constexpr wchar_t kControlWindowClass[] = L"LiveWallpaperEngine.Control";
constexpr wchar_t kWallpaperWindowClass[] = L"LiveWallpaperEngine.Wallpaper";
constexpr wchar_t kApplicationTitle[] = L"Live Wallpaper Engine";
constexpr int kApplicationIconResource = 101;
constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kMediaEngineEventMessage = WM_APP + 2;
constexpr UINT kPlaybackFailureMessage = WM_APP + 3;
constexpr UINT kPlaybackFatalMessage = WM_APP + 4;
constexpr UINT kRevealWallpaperMessage = WM_APP + 5;
constexpr UINT kUpdateCheckResultMessage = WM_APP + 6;
constexpr UINT kBeginUpdateCheckMessage = WM_APP + 7;
constexpr UINT kInstallerShutdownMessage = WM_APP + 8;
constexpr UINT kVideoOptimizationResultMessage = WM_APP + 9;
constexpr UINT kShowSettingsMessage = WM_APP + 10;
constexpr UINT kTrayIconId = 1;
constexpr UINT_PTR kExplorerRecoveryTimer = 1;
constexpr UINT_PTR kPlaybackPolicyTimer = 2;
constexpr UINT_PTR kResourceUsageTimer = 3;
constexpr auto kDeepPauseReleaseDelay = std::chrono::seconds(60);
constexpr int kTrayImportCommand = 2100;
constexpr int kTrayShowCommand = 2101;
constexpr int kTraySoundCommand = 2102;
constexpr int kTrayExitCommand = 2103;
constexpr int kTrayCancelCommand = 2104;
constexpr int kTrayPauseCommand = 2105;
constexpr int kControlledDeepPauseToggleCommand = 2193;
constexpr int kControlledDeepPauseReleaseCommand = 2194;
constexpr int kControlledLibraryDrawCountCommand = 2195;
constexpr int kControlledFrameCountCommand = 2196;
constexpr int kControlledTestSaveCommand = 2198;
constexpr int kControlledTestExitCommand = 2199;
constexpr int kLibraryPreviewCommand = 2200;
constexpr int kLibraryRenameCommand = 2201;
constexpr int kLibraryApplyCommand = 2202;
constexpr int kLibraryOpenLocationCommand = 2203;
constexpr int kLibraryExportCommand = 2204;
constexpr int kLibraryRemoveCommand = 2205;
constexpr int kLibraryAddFavoriteCommand = 2206;
constexpr int kLibraryRemoveFavoriteCommand = 2207;
constexpr int kLibraryRemoveFromGroupCommand = 2208;
constexpr int kLibraryMultiSelectCommand = 2209;
constexpr int kGroupRenameCommand = 2210;
constexpr int kGroupDeleteCommand = 2211;
constexpr int kBatchExportCommand = 2212;
constexpr int kBatchRemoveFromGroupCommand = 2213;
constexpr int kBatchAddFavoriteCommand = 2214;
constexpr int kBatchRemoveFavoriteCommand = 2215;
constexpr int kBatchDeleteCommand = 2216;
constexpr int kAddToGroupCommandBase = 2400;
constexpr int kUpdateDialogPrimaryCommand = 2300;
constexpr int kUpdateDialogSecondaryCommand = 2301;
constexpr wchar_t kDesktopCompatibilityMutexName[] =
    L"cxWallpaperEngineGlobalMutex";
constexpr wchar_t kUpdateDialogWindowClass[] =
    L"LiveWallpaperEngine.UpdateResult";
constexpr wchar_t kUpdateButtonWindowClass[] =
    L"LiveWallpaperEngine.UpdateButton";
constexpr wchar_t kSettingsButtonWindowClass[] =
    L"LiveWallpaperEngine.SettingsButton";

constexpr COLORREF kUpdateBackground = RGB(17, 20, 27);
constexpr COLORREF kUpdatePanel = RGB(28, 33, 44);
constexpr COLORREF kUpdatePanelHover = RGB(36, 43, 57);
constexpr COLORREF kUpdateAccent = RGB(92, 124, 250);
constexpr COLORREF kUpdateAccentHover = RGB(112, 142, 255);
constexpr COLORREF kUpdateText = RGB(241, 244, 250);
constexpr COLORREF kUpdateSecondaryText = RGB(158, 168, 188);
constexpr COLORREF kUpdateBorder = RGB(54, 63, 82);

std::wstring TrimWhitespace(std::wstring value) {
    const auto first = std::ranges::find_if_not(value, [](const wchar_t character) {
        return std::iswspace(character) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(),
                                       [](const wchar_t character) {
                                           return std::iswspace(character) != 0;
                                       })
                          .base();
    if (first >= last) {
        return {};
    }
    return std::wstring(first, last);
}

int UpdateUiScale(const HWND window, const int value) {
    return MulDiv(value, GetDpiForWindow(window), 96);
}

void FillUpdateRectangle(const HDC context, const RECT& rectangle,
                         const COLORREF color) {
    const HBRUSH brush = CreateSolidBrush(color);
    FillRect(context, &rectangle, brush);
    DeleteObject(brush);
}

void FillUpdateRoundedRectangle(const HDC context, const RECT& rectangle,
                                const COLORREF fill, const COLORREF outline,
                                const int radius) {
    const HBRUSH brush = CreateSolidBrush(fill);
    const HPEN pen = CreatePen(PS_SOLID, 1, outline);
    const HGDIOBJ previousBrush = SelectObject(context, brush);
    const HGDIOBJ previousPen = SelectObject(context, pen);
    RoundRect(context, rectangle.left, rectangle.top, rectangle.right,
              rectangle.bottom, radius, radius);
    SelectObject(context, previousPen);
    SelectObject(context, previousBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void DrawUpdateText(const HDC context, const std::wstring_view text,
                    RECT rectangle, const HFONT font, const COLORREF color,
                    const UINT format) {
    const HGDIOBJ previousFont = SelectObject(context, font);
    SetBkMode(context, TRANSPARENT);
    SetTextColor(context, color);
    DrawTextW(context, text.data(), static_cast<int>(text.size()), &rectangle,
              format);
    SelectObject(context, previousFont);
}

struct UpdateDialogState final {
    HINSTANCE instance = nullptr;
    HWND owner = nullptr;
    HWND window = nullptr;
    HWND primary = nullptr;
    HWND secondary = nullptr;
    HFONT headingFont = nullptr;
    HFONT bodyFont = nullptr;
    HFONT detailFont = nullptr;
    std::wstring heading;
    std::wstring message;
    std::wstring detail;
    std::wstring primaryLabel;
    bool showSecondary = false;
    bool primaryHovered = false;
    bool secondaryHovered = false;
    bool accepted = false;
    bool complete = false;
};

void RecreateUpdateDialogFonts(UpdateDialogState& state) {
    for (HFONT* font : {&state.headingFont, &state.bodyFont, &state.detailFont}) {
        if (*font != nullptr) {
            DeleteObject(*font);
        }
    }
    const int dpi = static_cast<int>(GetDpiForWindow(state.window));
    state.headingFont = CreateFontW(
        -MulDiv(20, dpi, 96), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Display");
    state.bodyFont = CreateFontW(
        -MulDiv(14, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Text");
    state.detailFont = CreateFontW(
        -MulDiv(12, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Text");
    for (const HWND control : {state.primary, state.secondary}) {
        if (IsWindow(control)) {
            SendMessageW(control, WM_SETFONT,
                         reinterpret_cast<WPARAM>(state.bodyFont), TRUE);
        }
    }
}

void LayoutUpdateDialog(UpdateDialogState& state) {
    RECT client{};
    GetClientRect(state.window, &client);
    const int margin = UpdateUiScale(state.window, 24);
    const int buttonHeight = UpdateUiScale(state.window, 38);
    const int primaryWidth = UpdateUiScale(state.window, 126);
    const int secondaryWidth = UpdateUiScale(state.window, 92);
    const int gap = UpdateUiScale(state.window, 10);
    const int top = client.bottom - margin - buttonHeight;
    MoveWindow(state.primary, client.right - margin - primaryWidth, top,
               primaryWidth, buttonHeight, TRUE);
    if (state.showSecondary) {
        SetWindowPos(state.secondary, HWND_TOP,
                     client.right - margin - primaryWidth - gap - secondaryWidth,
                     top, secondaryWidth, buttonHeight, SWP_SHOWWINDOW);
    } else {
        ShowWindow(state.secondary, SW_HIDE);
    }
}

void PaintUpdateDialog(const UpdateDialogState& state, const HDC context) {
    RECT client{};
    GetClientRect(state.window, &client);
    FillUpdateRectangle(context, client, kUpdateBackground);

    const int margin = UpdateUiScale(state.window, 24);
    RECT heading{margin, UpdateUiScale(state.window, 22),
                 client.right - margin, UpdateUiScale(state.window, 54)};
    DrawUpdateText(context, state.heading, heading, state.headingFont,
                   kUpdateText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT message{margin, UpdateUiScale(state.window, 60), client.right - margin,
                 UpdateUiScale(state.window, 94)};
    DrawUpdateText(context, state.message, message, state.bodyFont,
                   kUpdateSecondaryText, DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                                             DT_END_ELLIPSIS);

    RECT detail{margin, UpdateUiScale(state.window, 104), client.right - margin,
                UpdateUiScale(state.window, 154)};
    FillUpdateRoundedRectangle(context, detail, kUpdatePanel, kUpdateBorder,
                               UpdateUiScale(state.window, 10));
    InflateRect(&detail, -UpdateUiScale(state.window, 14), 0);
    DrawUpdateText(context, state.detail, detail, state.detailFont, kUpdateText,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void DrawUpdateDialogButton(const UpdateDialogState& state,
                            const DRAWITEMSTRUCT& draw) {
    wchar_t label[64]{};
    GetWindowTextW(draw.hwndItem, label, static_cast<int>(std::size(label)));
    const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
    const bool focused = (draw.itemState & ODS_FOCUS) != 0;
    const bool primary = draw.CtlID == kUpdateDialogPrimaryCommand;
    const bool hovered = primary ? state.primaryHovered
                                 : state.secondaryHovered;
    FillUpdateRectangle(draw.hDC, draw.rcItem, kUpdateBackground);
    RECT button = draw.rcItem;
    InflateRect(&button, -1, -1);
    const COLORREF fill = primary
                              ? (pressed ? RGB(72, 99, 207)
                                         : (hovered ? kUpdateAccentHover
                                                    : kUpdateAccent))
                              : (pressed || hovered ? kUpdatePanelHover
                                                    : kUpdatePanel);
    const COLORREF outline = primary || focused ? kUpdateAccent : kUpdateBorder;
    FillUpdateRoundedRectangle(draw.hDC, button, fill, outline,
                               UpdateUiScale(state.window, 10));
    DrawUpdateText(draw.hDC, label, button, state.bodyFont, kUpdateText,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

LRESULT CALLBACK UpdateDialogButtonProcedure(
    const HWND window, const UINT message, const WPARAM wParam,
    const LPARAM lParam, const UINT_PTR subclassId,
    const DWORD_PTR referenceData) {
    auto* state = reinterpret_cast<UpdateDialogState*>(referenceData);
    if (state == nullptr) {
        return DefSubclassProc(window, message, wParam, lParam);
    }
    bool* hovered = window == state->primary ? &state->primaryHovered
                                             : &state->secondaryHovered;
    if (message == WM_MOUSEMOVE && !*hovered) {
        *hovered = true;
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
        InvalidateRect(window, nullptr, FALSE);
    } else if (message == WM_MOUSELEAVE && *hovered) {
        *hovered = false;
        InvalidateRect(window, nullptr, FALSE);
    } else if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, &UpdateDialogButtonProcedure, subclassId);
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

LRESULT CALLBACK UpdateDialogWindowProcedure(const HWND window,
                                             const UINT message,
                                             const WPARAM wParam,
                                             const LPARAM lParam) {
    auto* state = reinterpret_cast<UpdateDialogState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = static_cast<UpdateDialogState*>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    switch (message) {
        case WM_CREATE:
            state->primary = CreateWindowExW(
                0, L"BUTTON", state->primaryLabel.c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 1, 1,
                window, reinterpret_cast<HMENU>(
                            static_cast<INT_PTR>(kUpdateDialogPrimaryCommand)),
                state->instance, nullptr);
            state->secondary = CreateWindowExW(
                0, L"BUTTON", L"稍后",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 1, 1,
                window, reinterpret_cast<HMENU>(
                            static_cast<INT_PTR>(kUpdateDialogSecondaryCommand)),
                state->instance, nullptr);
            if (!IsWindow(state->primary) || !IsWindow(state->secondary)) {
                return -1;
            }
            SetWindowSubclass(state->primary, &UpdateDialogButtonProcedure, 1,
                              reinterpret_cast<DWORD_PTR>(state));
            SetWindowSubclass(state->secondary, &UpdateDialogButtonProcedure, 1,
                              reinterpret_cast<DWORD_PTR>(state));
            RecreateUpdateDialogFonts(*state);
            LayoutUpdateDialog(*state);
            return 0;
        case WM_SIZE:
            LayoutUpdateDialog(*state);
            return 0;
        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOACTIVATE | SWP_NOZORDER);
            RecreateUpdateDialogFonts(*state);
            LayoutUpdateDialog(*state);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_COMMAND:
            if (HIWORD(wParam) == BN_CLICKED &&
                LOWORD(wParam) == kUpdateDialogPrimaryCommand) {
                state->accepted = true;
                DestroyWindow(window);
                return 0;
            }
            if (HIWORD(wParam) == BN_CLICKED &&
                LOWORD(wParam) == kUpdateDialogSecondaryCommand) {
                DestroyWindow(window);
                return 0;
            }
            break;
        case WM_DRAWITEM:
            DrawUpdateDialogButton(
                *state, *reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const HDC context = BeginPaint(window, &paint);
            PaintUpdateDialog(*state, context);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_NCDESTROY:
            state->complete = true;
            state->window = nullptr;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            return DefWindowProcW(window, message, wParam, lParam);
        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool ShowUpdateDialog(const HWND owner, const HINSTANCE instance,
                      std::wstring heading, std::wstring message,
                      std::wstring detail, std::wstring primaryLabel,
                      const bool showSecondary) {
    UpdateDialogState state;
    state.instance = instance;
    state.owner = owner;
    state.heading = std::move(heading);
    state.message = std::move(message);
    state.detail = std::move(detail);
    state.primaryLabel = std::move(primaryLabel);
    state.showSecondary = showSecondary;

    const int clientWidth = UpdateUiScale(owner, 560);
    const int clientHeight = UpdateUiScale(owner, 218);
    RECT outer{0, 0, clientWidth, clientHeight};
    constexpr DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    constexpr DWORD extendedStyle = WS_EX_DLGMODALFRAME;
    AdjustWindowRectExForDpi(&outer, style, FALSE, extendedStyle,
                             GetDpiForWindow(owner));
    RECT ownerRectangle{};
    GetWindowRect(owner, &ownerRectangle);
    const int width = outer.right - outer.left;
    const int height = outer.bottom - outer.top;
    const int left = ownerRectangle.left +
                     (ownerRectangle.right - ownerRectangle.left - width) / 2;
    const int top = ownerRectangle.top +
                    (ownerRectangle.bottom - ownerRectangle.top - height) / 2;
    const HWND dialog = CreateWindowExW(
        extendedStyle, kUpdateDialogWindowClass, L"检查更新", style, left, top,
        width, height, owner, nullptr, instance, &state);
    if (!IsWindow(dialog)) {
        if (state.headingFont != nullptr) {
            DeleteObject(state.headingFont);
        }
        if (state.bodyFont != nullptr) {
            DeleteObject(state.bodyFont);
        }
        if (state.detailFont != nullptr) {
            DeleteObject(state.detailFont);
        }
        return false;
    }
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(dialog, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark,
                          sizeof(dark));
    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    SetForegroundWindow(dialog);

    bool receivedQuit = false;
    WPARAM quitCode = 0;
    MSG messageRecord{};
    while (!state.complete &&
           GetMessageW(&messageRecord, nullptr, 0, 0) > 0) {
        if (messageRecord.message == WM_KEYDOWN &&
            messageRecord.wParam == VK_ESCAPE) {
            PostMessageW(dialog, WM_CLOSE, 0, 0);
            continue;
        }
        if (!IsDialogMessageW(dialog, &messageRecord)) {
            TranslateMessage(&messageRecord);
            DispatchMessageW(&messageRecord);
        }
    }
    if (messageRecord.message == WM_QUIT) {
        receivedQuit = true;
        quitCode = messageRecord.wParam;
    }
    if (IsWindow(dialog)) {
        DestroyWindow(dialog);
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (state.headingFont != nullptr) {
        DeleteObject(state.headingFont);
    }
    if (state.bodyFont != nullptr) {
        DeleteObject(state.bodyFont);
    }
    if (state.detailFont != nullptr) {
        DeleteObject(state.detailFont);
    }
    if (receivedQuit) {
        PostQuitMessage(static_cast<int>(quitCode));
    }
    return state.accepted;
}

struct UpdateTitleButtonState final {
    HWND owner = nullptr;
    UINT clickMessage = 0;
    bool hovered = false;
    bool pressed = false;
};

struct TitleButtonCreateParameters final {
    HWND owner = nullptr;
    UINT clickMessage = 0;
};

void PaintUpdateTitleButton(const HWND window,
                            const UpdateTitleButtonState& state,
                            const HDC context) {
    constexpr COLORREF transparentKey = RGB(1, 2, 3);
    RECT client{};
    GetClientRect(window, &client);
    FillUpdateRectangle(context, client, transparentKey);
    RECT button = client;
    InflateRect(&button, -1, -1);
    wchar_t label[32]{};
    GetWindowTextW(window, label, static_cast<int>(std::size(label)));
    const bool checking = std::wstring_view(label).starts_with(L"检查中");
    const COLORREF fill = state.pressed
                              ? RGB(52, 65, 95)
                              : (state.hovered ? RGB(40, 49, 67)
                                               : RGB(27, 34, 45));
    const COLORREF outline = state.hovered || checking
                                 ? kUpdateAccent
                                 : RGB(61, 72, 91);
    FillUpdateRoundedRectangle(context, button, fill, outline,
                               UpdateUiScale(window, 7));
    const UINT dpi = GetDpiForWindow(window);
    const HFONT font = CreateFontW(
        -MulDiv(12, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Text");
    DrawUpdateText(context, label, button, font, kUpdateText,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DeleteObject(font);
}

LRESULT CALLBACK UpdateButtonWindowProcedure(const HWND window,
                                             const UINT message,
                                             const WPARAM wParam,
                                             const LPARAM lParam) {
    auto* state = reinterpret_cast<UpdateTitleButtonState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        const auto* parameters =
            static_cast<const TitleButtonCreateParameters*>(
                create->lpCreateParams);
        state = new UpdateTitleButtonState{
            parameters != nullptr ? parameters->owner : nullptr,
            parameters != nullptr ? parameters->clickMessage : 0};
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    switch (message) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        case WM_MOUSEMOVE:
            if (!state->hovered) {
                state->hovered = true;
                TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
                TrackMouseEvent(&tracking);
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSELEAVE:
            state->hovered = false;
            state->pressed = false;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            state->pressed = true;
            SetCapture(window);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            if (GetCapture() == window) {
                ReleaseCapture();
            }
            RECT client{};
            GetClientRect(window, &client);
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const bool clicked = state->pressed && PtInRect(&client, point);
            state->pressed = false;
            InvalidateRect(window, nullptr, FALSE);
            if (clicked && IsWindow(state->owner) && state->clickMessage != 0) {
                PostMessageW(state->owner, state->clickMessage, 0, 0);
            }
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const HDC context = BeginPaint(window, &paint);
            PaintUpdateTitleButton(window, *state, context);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_NCDESTROY:
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            delete state;
            return DefWindowProcW(window, message, wParam, lParam);
        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool RegisterApplicationClass(const HINSTANCE instance, const wchar_t* className,
                              const WNDPROC procedure, const HBRUSH background,
                              const HICON icon) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = procedure;
    windowClass.hInstance = instance;
    windowClass.hIcon = icon;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = background;
    windowClass.lpszClassName = className;
    windowClass.hIconSm = icon;

    if (RegisterClassExW(&windowClass) != 0 ||
        GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        return true;
    }
    core::LogError(L"RegisterClassEx failed.", HRESULT_FROM_WIN32(GetLastError()));
    return false;
}

DWORD RemainingTestMilliseconds(
    const std::chrono::seconds testDuration,
    const std::chrono::steady_clock::duration elapsed) {
    if (testDuration.count() <= 0) {
        return INFINITE;
    }
    const auto remaining =
        std::chrono::ceil<std::chrono::milliseconds>(testDuration - elapsed);
    return static_cast<DWORD>(
        std::clamp<std::int64_t>(remaining.count(), 1, INFINITE - 1LL));
}

core::WallpaperSelectionKind SettingsKind(const media::WallpaperKind kind) {
    switch (kind) {
        case media::WallpaperKind::StaticImage:
            return core::WallpaperSelectionKind::StaticImage;
        case media::WallpaperKind::AnimatedGif:
            return core::WallpaperSelectionKind::AnimatedGif;
        case media::WallpaperKind::Video:
            return core::WallpaperSelectionKind::Video;
    }
    return core::WallpaperSelectionKind::StaticImage;
}

media::WallpaperKind MediaKind(const core::WallpaperSelectionKind kind) {
    switch (kind) {
        case core::WallpaperSelectionKind::StaticImage:
            return media::WallpaperKind::StaticImage;
        case core::WallpaperSelectionKind::AnimatedGif:
            return media::WallpaperKind::AnimatedGif;
        case core::WallpaperSelectionKind::Video:
            return media::WallpaperKind::Video;
        case core::WallpaperSelectionKind::DynamicTest:
            break;
    }
    return media::WallpaperKind::StaticImage;
}

bool SamePath(const std::wstring_view left, const std::wstring_view right) {
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                right.data(), static_cast<int>(right.size()),
                                TRUE) == CSTR_EQUAL;
}

bool ContainsDisplayId(const std::vector<std::wstring>& identifiers,
    const std::wstring_view identifier) {
    return std::ranges::any_of(identifiers, [&](const std::wstring& existing) {
        return SamePath(existing, identifier);
    });
}

bool IsPackagePath(const std::wstring_view path) {
    return _wcsicmp(std::filesystem::path(path).extension().c_str(), L".lwewall") == 0;
}

bool IsArchivePath(const std::wstring_view path) {
    return _wcsicmp(std::filesystem::path(path).extension().c_str(), L".zip") == 0;
}

std::wstring JoinDisplayIds(const std::vector<std::wstring>& identifiers) {
    std::wstring joined;
    for (const std::wstring& identifier : identifiers) {
        if (!joined.empty()) {
            joined += L'|';
        }
        joined += identifier;
    }
    return joined;
}

std::vector<std::wstring> SplitDisplayIds(const std::wstring_view value) {
    std::vector<std::wstring> identifiers;
    std::size_t start = 0;
    while (start < value.size()) {
        const std::size_t delimiter = value.find(L'|', start);
        const std::size_t end = delimiter == std::wstring_view::npos
                                    ? value.size()
                                    : delimiter;
        if (end > start) {
            identifiers.emplace_back(value.substr(start, end - start));
        }
        if (delimiter == std::wstring_view::npos) {
            break;
        }
        start = delimiter + 1;
    }
    return identifiers;
}

bool AssignmentsUseUniqueDisplays(
    const std::vector<core::WallpaperAssignmentSetting>& assignments,
    const std::vector<shell::DisplayTarget>& displays) {
    std::vector<std::wstring> claimedDisplays;
    for (const core::WallpaperAssignmentSetting& assignment : assignments) {
        std::vector<std::wstring> identifiers;
        if (assignment.spanAcrossDisplays) {
            identifiers.reserve(displays.size());
            for (const shell::DisplayTarget& display : displays) {
                identifiers.push_back(display.deviceId);
            }
        } else {
            identifiers = SplitDisplayIds(assignment.displayTargets);
        }
        for (const std::wstring& identifier : identifiers) {
            if (ContainsDisplayId(claimedDisplays, identifier)) {
                return false;
            }
            claimedDisplays.push_back(identifier);
        }
    }
    return true;
}

std::wstring FormatResourceUsage(
    const platform::ProcessResourceUsage& usage) {
    constexpr double mebibyte = 1024.0 * 1024.0;
    std::wostringstream text;
    text << std::fixed << std::setprecision(1) << L"CPU " << usage.cpuPercent
         << L"%\tGPU ";
    if (usage.gpuAvailable) {
        text << usage.gpuPercent << L'%';
    } else {
        text << L"--";
    }
    text << L"\n内存 " << std::setprecision(0)
         << static_cast<double>(usage.workingSetBytes) / mebibyte
         << L" MB\n";
    if (usage.gpuMemoryAvailable) {
        text << L"专用 "
             << static_cast<double>(usage.dedicatedGpuMemoryBytes) / mebibyte
             << L" MB\t共享 "
             << static_cast<double>(usage.sharedGpuMemoryBytes) / mebibyte
             << L" MB";
    } else {
        text << L"专用 --\t共享 --";
    }
    return text.str();
}

}  // namespace

WallpaperApplication::WallpaperApplication(const HINSTANCE instance,
                                           const HANDLE activationEvent)
    : instance_(instance), activationEvent_(activationEvent) {}

WallpaperApplication::~WallpaperApplication() {
    Shutdown();
}

int WallpaperApplication::Run(const std::chrono::seconds testDuration,
                              const std::vector<std::wstring>& testWallpapers,
                              const updates::UpdateCheckMode updateCheckMode) {
    controlledTestMode_ = testDuration.count() > 0;
    updateCheckMode_ = updateCheckMode;
    // Tencent DeskGo and other desktop organizers use this established signal
    // to stop painting an opaque copy of the Windows wallpaper above live
    // wallpaper hosts. The handle exists only for our application lifetime.
    SetLastError(ERROR_SUCCESS);
    desktopCompatibilityMutex_ =
        CreateMutexW(nullptr, TRUE, kDesktopCompatibilityMutexName);
    if (desktopCompatibilityMutex_ == nullptr) {
        core::LogWarning(L"Desktop-organizer compatibility signal is unavailable.");
    } else {
        desktopCompatibilityMutexOwned_ = GetLastError() != ERROR_ALREADY_EXISTS;
        core::LogInfo(desktopCompatibilityMutexOwned_
                          ? L"Desktop-organizer live-wallpaper compatibility enabled."
                          : L"Desktop-organizer compatibility is already active.");
    }
    const HRESULT mediaFoundationResult = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    mediaFoundationStarted_ = SUCCEEDED(mediaFoundationResult);
    if (!mediaFoundationStarted_) {
        core::LogError(L"Media Foundation initialization failed; video is unavailable.",
                       mediaFoundationResult);
    }

    const HRESULT libraryResult = wallpaperLibrary_.Initialize();
    if (FAILED(libraryResult)) {
        core::LogError(L"The local wallpaper library could not be initialized.",
                       libraryResult);
    }
    const HRESULT groupsResult =
        SUCCEEDED(libraryResult)
            ? groupStore_.InitializeAt(wallpaperLibrary_.RootDirectory())
            : groupStore_.Initialize();
    if (FAILED(groupsResult)) {
        core::LogError(L"Wallpaper groups could not be initialized.", groupsResult);
    }

    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
    if (!RegisterWindowClasses() || !CreateControlWindow() ||
        !CreateWallpaperWindow()) {
        Shutdown();
        return 1;
    }
    InitializePlaybackPolicy();
    StartPlaybackRenderThread();
    resourceMonitor_.Initialize();
    SetTimer(controlWindow_, kResourceUsageTimer, 1000, nullptr);
    UpdateResourceUsage();

    bool wallpaperApplied = false;
    if (!testWallpapers.empty()) {
        wallpaperApplied = ApplyWallpaper(testWallpapers.front(), false, false);
        if (!wallpaperApplied) {
            Shutdown();
            return 1;
        }
    } else {
        wallpaperApplied = RestoreSavedWallpaperSelection();
    }
    if (!wallpaperApplied) {
        const std::scoped_lock playbackLock(playbackMutex_);
        playbackMode_ = controlledTestMode_ ? PlaybackMode::TechnicalTest
                                            : PlaybackMode::Stopped;
        if (!controlledTestMode_ && IsWindow(wallpaperWindow_)) {
            ShowWindow(wallpaperWindow_, SW_HIDE);
        }
        mainWindow_.SetStatus(
            L"尚未选择壁纸 · 点击“导入壁纸”开始建立你的本地壁纸库");
    }
    RefreshLibrary();
    if (!AddTrayIcon()) {
        core::LogWarning(
            L"The tray icon could not be created; the main window will stay open.");
    }

    const auto startedAt = std::chrono::steady_clock::now();
    auto nextControlledSwitchAt = startedAt + std::chrono::milliseconds(200);
    std::size_t nextControlledWallpaper = 1;
    int exitCode = 0;
    running_ = true;
    ShowControlWindow();

    MSG message{};
    while (running_) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                running_ = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!running_) {
            break;
        }

        if (activationEvent_ != nullptr &&
            WaitForSingleObject(activationEvent_, 0) == WAIT_OBJECT_0) {
            ShowControlWindow();
        }

        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = now - startedAt;
        if (testDuration.count() > 0 && elapsed >= testDuration) {
            core::LogInfo(L"Controlled test duration completed.");
            break;
        }
        if (testWallpapers.size() > 1 && now >= nextControlledSwitchAt) {
            const std::wstring& nextPath =
                testWallpapers[nextControlledWallpaper % testWallpapers.size()];
            if (!ApplyWallpaper(nextPath, false, false)) {
                core::LogError(L"Controlled wallpaper switching failed: " + nextPath);
                exitCode = 1;
                break;
            }
            ++nextControlledWallpaper;
            nextControlledSwitchAt = now + std::chrono::milliseconds(200);
            core::LogInfo(L"Controlled wallpaper switch completed; count=" +
                          std::to_wstring(nextControlledWallpaper - 1) + L'.');
        }
        if (playbackMode_ == PlaybackMode::TechnicalTest && renderer_.IsInitialized()) {
            if (!renderer_.Render(elapsed)) {
                exitCode = 1;
                break;
            }
            continue;
        }
        DWORD waitMilliseconds = RemainingTestMilliseconds(testDuration, elapsed);
        if (testWallpapers.size() > 1 && now < nextControlledSwitchAt) {
            waitMilliseconds = std::min(
                waitMilliseconds,
                static_cast<DWORD>(std::clamp<std::int64_t>(
                    std::chrono::ceil<std::chrono::milliseconds>(
                        nextControlledSwitchAt - now).count(),
                    1, INFINITE - 1LL)));
        }

        HANDLE handles[] = {activationEvent_};
        const DWORD handleCount = activationEvent_ == nullptr ? 0UL : 1UL;
        const DWORD waitResult = MsgWaitForMultipleObjectsEx(
            handleCount, handles, waitMilliseconds, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (handleCount == 1 && waitResult == WAIT_OBJECT_0) {
            ShowControlWindow();
        } else if (waitResult == WAIT_FAILED) {
            core::LogError(L"Idle message wait failed.",
                           HRESULT_FROM_WIN32(GetLastError()));
            exitCode = 1;
            break;
        }
    }

    Shutdown();
    return exitCode != 0 ? exitCode : runtimeExitCode_.load();
}

bool WallpaperApplication::RegisterWindowClasses() {
    const HICON icon = LoadIconW(instance_, MAKEINTRESOURCEW(kApplicationIconResource));
    return RegisterApplicationClass(instance_, kControlWindowClass,
                                    &WallpaperApplication::WindowProcedure, nullptr,
                                    icon) &&
           RegisterApplicationClass(instance_, kWallpaperWindowClass,
                                    &WallpaperApplication::WindowProcedure, nullptr,
                                    icon) &&
           RegisterApplicationClass(instance_, kUpdateDialogWindowClass,
                                    &UpdateDialogWindowProcedure, nullptr, icon) &&
           RegisterApplicationClass(instance_, kUpdateButtonWindowClass,
                                    &UpdateButtonWindowProcedure, nullptr, icon) &&
           RegisterApplicationClass(instance_, kSettingsButtonWindowClass,
                                    &UpdateButtonWindowProcedure, nullptr, icon);
}

bool WallpaperApplication::CreateControlWindow() {
    const UINT dpi = GetDpiForSystem();
    constexpr DWORD windowStyle =
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    RECT windowRectangle{0, 0, MulDiv(1040, dpi, 96), MulDiv(700, dpi, 96)};
    AdjustWindowRectExForDpi(&windowRectangle, windowStyle, FALSE,
                             WS_EX_APPWINDOW, dpi);
    controlWindow_ = CreateWindowExW(
        WS_EX_APPWINDOW, kControlWindowClass, kApplicationTitle, windowStyle,
        CW_USEDEFAULT, CW_USEDEFAULT, windowRectangle.right - windowRectangle.left,
        windowRectangle.bottom - windowRectangle.top, nullptr, nullptr, instance_, this);
    if (controlWindow_ == nullptr) {
        core::LogError(L"Unable to create the main window.",
                       HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    if (!mainWindow_.Create(controlWindow_, instance_)) {
        core::LogError(L"Unable to create the modern library controls.",
                       HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    if (!CreateUpdateButtonWindow()) {
        core::LogError(L"Unable to create the title-bar update button.",
                       HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    DragAcceptFiles(controlWindow_, TRUE);
    return true;
}

bool WallpaperApplication::CreateUpdateButtonWindow() {
    if (!IsWindow(controlWindow_)) {
        return false;
    }
    constexpr COLORREF transparentKey = RGB(1, 2, 3);
    const TitleButtonCreateParameters updateParameters{
        controlWindow_, kBeginUpdateCheckMessage};
    updateButtonWindow_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kUpdateButtonWindowClass, L"检查更新", WS_POPUP, 0, 0, 1, 1,
        controlWindow_, nullptr, instance_,
        const_cast<TitleButtonCreateParameters*>(&updateParameters));
    if (!IsWindow(updateButtonWindow_)) {
        return false;
    }
    SetLayeredWindowAttributes(updateButtonWindow_, transparentKey, 0,
                               LWA_COLORKEY);
    const TitleButtonCreateParameters settingsParameters{
        controlWindow_, kShowSettingsMessage};
    settingsButtonWindow_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kSettingsButtonWindowClass, L"设置", WS_POPUP, 0, 0, 1, 1,
        controlWindow_, nullptr, instance_,
        const_cast<TitleButtonCreateParameters*>(&settingsParameters));
    if (!IsWindow(settingsButtonWindow_)) {
        DestroyWindow(updateButtonWindow_);
        updateButtonWindow_ = nullptr;
        return false;
    }
    SetLayeredWindowAttributes(settingsButtonWindow_, transparentKey, 0,
                               LWA_COLORKEY);
    PositionUpdateButtonWindow();
    return true;
}

bool WallpaperApplication::CreateWallpaperWindow() {
    if (wallpaperWindow_ != nullptr && IsWindow(wallpaperWindow_)) {
        return true;
    }

    const std::optional target = shell::FindDesktopTarget();
    if (!target.has_value()) {
        return false;
    }
    desktopTarget_ = *target;

    constexpr DWORD extendedStyle =
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;
    constexpr DWORD style = WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    wallpaperWindow_ = CreateWindowExW(
        extendedStyle, kWallpaperWindowClass, L"", style, 0, 0, 1, 1, nullptr, nullptr,
        instance_, this);
    if (wallpaperWindow_ == nullptr) {
        core::LogError(L"CreateWindowEx failed for the wallpaper window.",
                       HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }

    if (!shell::AttachWallpaperWindow(wallpaperWindow_, desktopTarget_)) {
        return false;
    }
    RefreshDisplayTargets(true);
    if ((!assignments_.empty() && !ConfigureWallpaperWindowRegion()) ||
        !EnsureRenderer()) {
        return false;
    }
    ShowWindow(wallpaperWindow_, SW_SHOWNOACTIVATE);
    core::LogInfo(L"Wallpaper renderer window started.");
    return true;
}

bool WallpaperApplication::EnsureRenderer() {
    if (renderer_.IsInitialized()) {
        return true;
    }
    return wallpaperWindow_ != nullptr && renderer_.Initialize(wallpaperWindow_);
}

bool WallpaperApplication::ReattachToDesktop() {
    if (wallpaperWindow_ == nullptr || !IsWindow(wallpaperWindow_)) {
        return RecoverWallpaperWindow();
    }

    const std::optional target = shell::FindDesktopTarget();
    if (!target.has_value() ||
        !shell::AttachWallpaperWindow(wallpaperWindow_, *target)) {
        return false;
    }

    desktopTarget_ = *target;
    RefreshDisplayTargets(true);
    ConfigureWallpaperWindowRegion();
    ResizeRendererToWindow();
    core::LogInfo(L"Wallpaper window reattached after desktop topology change.");
    return true;
}

bool WallpaperApplication::RecoverWallpaperWindow() {
    if (!CreateWallpaperWindow()) {
        return false;
    }
    if (!assignments_.empty()) {
        return RebuildPlaybackSessions(false);
    }
    return true;
}

bool WallpaperApplication::AddTrayIcon() {
    if (controlWindow_ == nullptr) {
        return false;
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = controlWindow_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = kTrayCallbackMessage;
    data.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(kApplicationIconResource));
    wcscpy_s(data.szTip, kApplicationTitle);
    trayIconAdded_ = Shell_NotifyIconW(NIM_ADD, &data) == TRUE;
    if (trayIconAdded_) {
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
    }
    return trayIconAdded_;
}

void WallpaperApplication::RemoveTrayIcon() {
    if (!trayIconAdded_ || controlWindow_ == nullptr) {
        return;
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = controlWindow_;
    data.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
    trayIconAdded_ = false;
}

bool WallpaperApplication::RestoreSavedWallpaperSelection() {
    const std::optional settings = settingsStore_.Load();
    if (!settings.has_value()) {
        return false;
    }
    soundEnabled_ = settings->soundEnabled;
    releaseVideoResourcesOnPause_ =
        settings->releaseVideoResourcesOnPause;
    selectedDisplayIds_ = SplitDisplayIds(settings->displayTargets);
    spanAcrossDisplays_ = settings->spanAcrossDisplays;
    assignments_ = settings->assignments;
    RefreshDisplayTargets(true);
    const bool assignmentsRepaired = NormalizeAssignments();
    if (assignmentsRepaired && !controlledTestMode_ &&
        FAILED(SaveCurrentSelection())) {
        core::LogWarning(
            L"Overlapping wallpaper assignments were repaired in memory, but the "
            L"normalized settings could not be saved.");
    }
    mainWindow_.SetSoundEnabled(soundEnabled_);
    if (assignments_.empty()) {
        return false;
    }
    if (RebuildPlaybackSessions(false)) {
        core::LogInfo(L"Restored saved per-display wallpaper assignments.");
        return true;
    }
    mainWindow_.SetStatus(
        L"上次选择的壁纸无法恢复 · 文件可能已移动或当前系统缺少解码器");
    core::LogWarning(L"Unable to restore the saved wallpaper selection.");
    return false;
}

void WallpaperApplication::RefreshLibrary() {
    std::vector<core::WallpaperItem> items = wallpaperLibrary_.Scan();
    std::vector<std::wstring> validFileNames;
    validFileNames.reserve(items.size());
    for (const core::WallpaperItem& item : items) {
        validFileNames.push_back(item.path.filename().native());
    }
    const HRESULT pruneResult = groupStore_.Prune(validFileNames);
    if (FAILED(pruneResult)) {
        core::LogError(L"Stale wallpaper group entries could not be pruned.",
                       pruneResult);
    }
    for (const std::wstring& activePath : ActiveWallpaperPaths()) {
        const bool alreadyListed = std::ranges::any_of(items, [&](const auto& item) {
            return SamePath(item.path.native(), activePath);
        });
        std::error_code fileError;
        if (!alreadyListed && std::filesystem::is_regular_file(activePath, fileError) &&
            !fileError) {
            media::MediaInfo info;
            if (SUCCEEDED(media::ProbeMediaFile(activePath, info))) {
                core::WallpaperItem external;
                external.path = activePath;
                external.displayName =
                    std::filesystem::path(activePath).filename().native() +
                    L"（外部）";
                external.kind = info.kind;
                external.formatLabel = info.formatLabel;
                external.width = info.width;
                external.height = info.height;
                external.hasAudio = info.hasAudio;
                external.external = true;
                std::error_code error;
                external.fileSize =
                    std::filesystem::file_size(activePath, error);
                items.insert(items.begin(), std::move(external));
            }
        }
    }
    RefreshGroups();
    mainWindow_.SetItems(std::move(items));
    mainWindow_.SetActiveWallpapers(ActiveWallpapers());
    mainWindow_.SetSoundEnabled(soundEnabled_);
}

void WallpaperApplication::RefreshGroups(const std::wstring_view selectedGroupId) {
    mainWindow_.SetGroups(groupStore_.Groups(), groupStore_.Favorites(),
                          selectedGroupId);
}

void WallpaperApplication::CreateWallpaperGroup() {
    std::wstring base = L"新分组";
    std::wstring name = base;
    for (std::size_t suffix = 2; std::ranges::any_of(
             groupStore_.Groups(), [&](const core::WallpaperGroup& group) {
                 return _wcsicmp(group.name.c_str(), name.c_str()) == 0;
             }); ++suffix) {
        name = base + L" " + std::to_wstring(suffix);
    }
    std::wstring id;
    const HRESULT result = groupStore_.CreateGroup(name, id);
    if (FAILED(result)) {
        MessageBoxW(controlWindow_, L"无法新建壁纸分组。", kApplicationTitle,
                    MB_OK | MB_ICONERROR);
        return;
    }
    RefreshGroups(id);
    mainWindow_.BeginRenameSelectedGroup();
    mainWindow_.SetStatus(L"已新建分组 · 输入名称后按 Enter 保存");
}

void WallpaperApplication::RenameWallpaperGroup() {
    const auto rename = mainWindow_.FinishGroupRename();
    if (!rename.has_value()) {
        return;
    }
    const std::wstring name = TrimWhitespace(rename->second);
    const HRESULT result = groupStore_.RenameGroup(rename->first, name);
    if (FAILED(result)) {
        MessageBoxW(controlWindow_,
                    L"分组名称不能为空、不能重复，且最多 64 个字符。",
                    kApplicationTitle, MB_OK | MB_ICONWARNING);
        RefreshGroups(rename->first);
        return;
    }
    RefreshGroups(rename->first);
    mainWindow_.SetStatus(L"分组已重命名 · " + name);
}

void WallpaperApplication::DeleteWallpaperGroup() {
    const auto group = mainWindow_.SelectedCustomGroup();
    if (!group.has_value()) {
        return;
    }
    const std::wstring question = L"确定删除分组“" + group->name +
                                  L"”吗？\r\n\r\n壁纸文件不会被删除。";
    if (MessageBoxW(controlWindow_, question.c_str(), kApplicationTitle,
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    const HRESULT result = groupStore_.DeleteGroup(group->id);
    if (FAILED(result)) {
        MessageBoxW(controlWindow_, L"无法删除该分组。", kApplicationTitle,
                    MB_OK | MB_ICONERROR);
        return;
    }
    RefreshGroups(ModernMainWindow::AllGroupId);
    mainWindow_.SetStatus(L"已删除分组 · 壁纸文件保持不变");
}

void WallpaperApplication::CommitGroupOrder() {
    const auto order = mainWindow_.TakePendingGroupOrder();
    if (!order.has_value()) {
        return;
    }
    const std::wstring selected = mainWindow_.CurrentGroupId();
    const HRESULT result = groupStore_.ReorderGroups(*order);
    if (FAILED(result)) {
        MessageBoxW(controlWindow_, L"分组顺序保存失败。", kApplicationTitle,
                    MB_OK | MB_ICONERROR);
    }
    RefreshGroups(selected);
    if (SUCCEEDED(result)) {
        mainWindow_.SetStatus(L"分组顺序已保存");
    }
}

void WallpaperApplication::AddWallpapersToGroup(
    const std::span<const core::WallpaperItem> items,
    const std::wstring_view groupId) {
    std::vector<std::wstring> fileNames;
    for (const core::WallpaperItem& item : items) {
        if (!item.external) {
            fileNames.push_back(item.path.filename().native());
        }
    }
    if (fileNames.empty()) {
        return;
    }
    const HRESULT result = groupStore_.AddToGroup(groupId, fileNames);
    if (FAILED(result)) {
        MessageBoxW(controlWindow_, L"无法把所选壁纸添加到分组。",
                    kApplicationTitle, MB_OK | MB_ICONERROR);
        return;
    }
    RefreshGroups(mainWindow_.CurrentGroupId());
    mainWindow_.SetStatus(L"已将 " + std::to_wstring(fileNames.size()) +
                          L" 张壁纸添加到分组");
}

void WallpaperApplication::RemoveWallpapersFromCurrentGroup(
    const std::span<const core::WallpaperItem> items) {
    if (mainWindow_.CurrentGroupIsAll()) {
        return;
    }
    std::vector<std::wstring> fileNames;
    for (const auto& item : items) {
        if (!item.external) {
            fileNames.push_back(item.path.filename().native());
        }
    }
    HRESULT result = S_OK;
    if (mainWindow_.CurrentGroupIsFavorites()) {
        result = groupStore_.SetFavorites(fileNames, false);
    } else {
        result = groupStore_.RemoveFromGroup(mainWindow_.CurrentGroupId(),
                                             fileNames);
    }
    if (FAILED(result)) {
        MessageBoxW(controlWindow_, L"无法从当前分组移除所选壁纸。",
                    kApplicationTitle, MB_OK | MB_ICONERROR);
        return;
    }
    RefreshGroups(mainWindow_.CurrentGroupId());
    mainWindow_.EndExportSelection();
    mainWindow_.SetStatus(L"已从当前分组移除 " +
                          std::to_wstring(fileNames.size()) + L" 张壁纸");
}

void WallpaperApplication::SetWallpapersFavorite(
    const std::span<const core::WallpaperItem> items, const bool favorite) {
    std::vector<std::wstring> fileNames;
    for (const auto& item : items) {
        if (!item.external) {
            fileNames.push_back(item.path.filename().native());
        }
    }
    const HRESULT result = groupStore_.SetFavorites(fileNames, favorite);
    if (FAILED(result)) {
        MessageBoxW(controlWindow_, L"最爱壁纸保存失败。", kApplicationTitle,
                    MB_OK | MB_ICONERROR);
        return;
    }
    RefreshGroups(mainWindow_.CurrentGroupId());
    mainWindow_.SetStatus(
        std::wstring(favorite ? L"已添加到最爱 · " : L"已从最爱移除 · ") +
        std::to_wstring(fileNames.size()) + L" 张壁纸");
}

void WallpaperApplication::DeleteWallpapers(
    const std::span<const core::WallpaperItem> items) {
    std::vector<core::WallpaperItem> localItems;
    for (const auto& item : items) {
        if (!item.external) {
            localItems.push_back(item);
        }
    }
    if (localItems.empty()) {
        return;
    }
    const std::wstring question = L"确定从“全部壁纸”中删除选中的 " +
                                  std::to_wstring(localItems.size()) +
                                  L" 张壁纸吗？\r\n\r\n只删除软件本地库中的副本，源文件不受影响。";
    if (MessageBoxW(controlWindow_, question.c_str(), kApplicationTitle,
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    std::size_t removed = 0;
    for (const auto& item : localItems) {
        if (std::ranges::any_of(assignments_, [&](const auto& assignment) {
                return SamePath(assignment.wallpaperPath, item.path.native());
            })) {
            CancelWallpaper(item, true);
        }
        if (SUCCEEDED(wallpaperLibrary_.Remove(item))) {
            groupStore_.RemoveWallpaperKey(item.path.filename().native());
            ++removed;
        }
    }
    mainWindow_.EndExportSelection();
    RefreshLibrary();
    mainWindow_.SetStatus(L"已从本地壁纸库删除 " + std::to_wstring(removed) +
                          L" 张壁纸");
}

void WallpaperApplication::ChooseImport() {
    const std::optional request = mainWindow_.ChooseImportSource();
    if (!request.has_value()) {
        return;
    }

    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                      CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(result)) {
        MessageBoxW(controlWindow_, L"无法打开导入窗口。", kApplicationTitle,
                    MB_OK | MB_ICONERROR);
        return;
    }

    const COMDLG_FILTERSPEC mediaFilters[] = {
        {L"支持的图片和视频",
         L"*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.mp4;*.m4v;*.mov;*.wmv;*.avi"},
        {L"图片和 GIF", L"*.jpg;*.jpeg;*.png;*.bmp;*.gif"},
        {L"视频", L"*.mp4;*.m4v;*.mov;*.wmv;*.avi"},
    };
    const COMDLG_FILTERSPEC shareFilters[] = {
        {L"壁纸分享包 (*.zip;*.lwewall)", L"*.zip;*.lwewall"},
        {L"ZIP 壁纸分享包 (*.zip)", L"*.zip"},
        {L"单个壁纸文件 (*.lwewall)", L"*.lwewall"},
    };
    const bool importingMedia =
        request->choice == ModernMainWindow::ImportChoice::MediaFiles;
    result = importingMedia
                 ? dialog->SetFileTypes(static_cast<UINT>(std::size(mediaFilters)),
                                        mediaFilters)
                 : dialog->SetFileTypes(static_cast<UINT>(std::size(shareFilters)),
                                        shareFilters);
    FILEOPENDIALOGOPTIONS options{};
    if (SUCCEEDED(result)) {
        result = dialog->GetOptions(&options);
    }
    if (SUCCEEDED(result)) {
        result = dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST |
                                    FOS_PATHMUSTEXIST | FOS_ALLOWMULTISELECT |
                                    FOS_NOCHANGEDIR);
    }
    if (SUCCEEDED(result)) {
        result = dialog->SetTitle(importingMedia ? L"导入图片 / 视频"
                                                 : L"导入壁纸分享包");
    }
    if (SUCCEEDED(result)) {
        result = dialog->Show(controlWindow_);
    }
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return;
    }
    if (FAILED(result)) {
        MessageBoxW(controlWindow_, L"导入窗口打开失败。", kApplicationTitle,
                    MB_OK | MB_ICONERROR);
        return;
    }

    Microsoft::WRL::ComPtr<IShellItemArray> selected;
    result = dialog->GetResults(&selected);
    DWORD count = 0;
    if (SUCCEEDED(result)) {
        result = selected->GetCount(&count);
    }
    std::vector<std::wstring> paths;
    for (DWORD index = 0; SUCCEEDED(result) && index < count; ++index) {
        Microsoft::WRL::ComPtr<IShellItem> item;
        result = selected->GetItemAt(index, &item);
        PWSTR path = nullptr;
        if (SUCCEEDED(result)) {
            result = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
        }
        if (SUCCEEDED(result) && path != nullptr) {
            paths.emplace_back(path);
        }
        CoTaskMemFree(path);
    }
    if (SUCCEEDED(result)) {
        ImportPaths(paths, request->compressToDisplay);
    }
}

void WallpaperApplication::ImportPaths(const std::vector<std::wstring>& paths,
                                       const bool compressToDisplay) {
    std::vector<core::WallpaperItem> importedItems;
    std::size_t failedCount = 0;
    bool importedArchive = false;
    for (const std::wstring& path : paths) {
        HRESULT result = S_OK;
        if (IsArchivePath(path)) {
            std::vector<core::WallpaperItem> archiveItems;
            result = wallpaperLibrary_.ImportArchive(path, archiveItems);
            if (SUCCEEDED(result)) {
                importedArchive = true;
                importedItems.insert(importedItems.end(),
                                     std::make_move_iterator(archiveItems.begin()),
                                     std::make_move_iterator(archiveItems.end()));
            }
        } else {
            core::WallpaperItem item;
            result = IsPackagePath(path)
                         ? wallpaperLibrary_.ImportPackage(path, item)
                         : wallpaperLibrary_.ImportFile(path, item);
            if (SUCCEEDED(result)) {
                importedItems.push_back(std::move(item));
            }
        }
        if (FAILED(result)) {
            ++failedCount;
            core::LogError(L"Wallpaper import failed: " + path, result);
        }
    }

    if (!importedItems.empty() && !importedArchive) {
        ApplyWallpaperWithTargetPrompt(importedItems.front().path.native(), true,
                                       true);
    }
    RefreshLibrary();

    std::wstring status = L"已导入 " + std::to_wstring(importedItems.size()) + L" 项";
    if (failedCount > 0) {
        status += L" · " + std::to_wstring(failedCount) + L" 项不受支持或校验失败";
        MessageBoxW(controlWindow_, status.c_str(), kApplicationTitle,
                    MB_OK | MB_ICONWARNING);
    }
    mainWindow_.SetStatus(std::move(status));
    if (compressToDisplay) {
        QueueVideoOptimizations(importedItems);
    }
}

void WallpaperApplication::QueueVideoOptimizations(
    const std::span<const core::WallpaperItem> importedItems) {
    UINT maximumWidth = 0;
    UINT maximumHeight = 0;
    for (const shell::DisplayTarget& display : displayTargets_) {
        maximumWidth = std::max(
            maximumWidth,
            static_cast<UINT>(std::max(
                0L, display.clientBounds.right - display.clientBounds.left)));
        maximumHeight = std::max(
            maximumHeight,
            static_cast<UINT>(std::max(
                0L, display.clientBounds.bottom - display.clientBounds.top)));
    }
    if (spanAcrossDisplays_ && displayTargets_.size() > 1) {
        RECT desktopBounds = displayTargets_.front().clientBounds;
        for (const shell::DisplayTarget& display : displayTargets_) {
            desktopBounds.left = std::min(desktopBounds.left,
                                          display.clientBounds.left);
            desktopBounds.top = std::min(desktopBounds.top,
                                         display.clientBounds.top);
            desktopBounds.right = std::max(desktopBounds.right,
                                           display.clientBounds.right);
            desktopBounds.bottom = std::max(desktopBounds.bottom,
                                            display.clientBounds.bottom);
        }
        maximumWidth = static_cast<UINT>(
            std::max(0L, desktopBounds.right - desktopBounds.left));
        maximumHeight = static_cast<UINT>(
            std::max(0L, desktopBounds.bottom - desktopBounds.top));
    }
    if (maximumWidth == 0 || maximumHeight == 0) {
        maximumWidth = 1920;
        maximumHeight = 1080;
    }

    std::size_t queued = 0;
    {
        const std::scoped_lock lock(videoOptimizationMutex_);
        if (videoOptimizationQueue_.empty() && !videoOptimizationJobActive_ &&
            pendingVideoOptimizationResults_.empty()) {
            videoOptimizationBatchOptimized_ = 0;
            videoOptimizationBatchSkipped_ = 0;
            videoOptimizationBatchFailed_ = 0;
        }
        for (const core::WallpaperItem& item : importedItems) {
            if (item.kind != media::WallpaperKind::Video || item.external) {
                continue;
            }
            const bool alreadyQueued = std::ranges::any_of(
                videoOptimizationQueue_, [&](const VideoOptimizationJob& job) {
                    return SamePath(job.originalPath, item.path.native());
                });
            if (alreadyQueued) {
                continue;
            }
            videoOptimizationQueue_.push_back(VideoOptimizationJob{
                item.path.native(), maximumWidth, maximumHeight});
            ++queued;
        }
    }
    if (queued == 0) {
        const wchar_t* message =
            L"导入的壁纸分辨率小于或等于屏幕分辨率，没有执行压缩。";
        mainWindow_.SetStatus(message);
        MessageBoxW(controlWindow_, message, kApplicationTitle,
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    StartVideoOptimizationThread();
    videoOptimizationWake_.notify_all();
    mainWindow_.SetStatus(
        L"正在后台压缩 " + std::to_wstring(queued) +
        L" 个视频 · 保留原始帧率和原文件");
}

void WallpaperApplication::StartVideoOptimizationThread() {
    if (videoOptimizationThread_.joinable()) {
        return;
    }
    const HWND notificationWindow = controlWindow_;
    videoOptimizationThread_ = std::jthread(
        [this, notificationWindow](const std::stop_token stopToken) {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            const HRESULT comResult =
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            while (!stopToken.stop_requested()) {
                VideoOptimizationJob job;
                {
                    std::unique_lock lock(videoOptimizationMutex_);
                    if (!videoOptimizationWake_.wait(
                            lock, stopToken, [&] {
                                return !videoOptimizationQueue_.empty();
                            })) {
                        break;
                    }
                    job = std::move(videoOptimizationQueue_.front());
                    videoOptimizationQueue_.pop_front();
                    videoOptimizationJobActive_ = true;
                }

                VideoOptimizationResult completed;
                completed.originalPath = job.originalPath;
                media::video::VideoOptimizationPlan plan;
                completed.status = media::video::PlanVideoOptimization(
                    job.originalPath, job.displayWidth, job.displayHeight,
                    plan);
                if (SUCCEEDED(completed.status) && !plan.needed) {
                    completed.skipped = true;
                } else if (SUCCEEDED(completed.status)) {
                    std::filesystem::path optimizedPath;
                    completed.status = wallpaperLibrary_.PrepareOptimizedVideoPath(
                        job.originalPath, optimizedPath);
                    const std::filesystem::path temporaryPath =
                        optimizedPath.native() + L".optimizing.mp4";
                    media::MediaInfo cachedInfo;
                    std::error_code cachedError;
                    const bool cacheReady =
                        SUCCEEDED(completed.status) &&
                        std::filesystem::is_regular_file(optimizedPath,
                                                         cachedError) &&
                        !cachedError &&
                        SUCCEEDED(media::ProbeMediaFile(optimizedPath.native(),
                                                        cachedInfo)) &&
                        cachedInfo.kind == media::WallpaperKind::Video &&
                        cachedInfo.width == plan.outputWidth &&
                        cachedInfo.height == plan.outputHeight &&
                        cachedInfo.frameRateDenominator != 0 &&
                        static_cast<std::uint64_t>(
                            cachedInfo.frameRateNumerator) *
                                plan.frameRateDenominator ==
                            static_cast<std::uint64_t>(
                                plan.frameRateNumerator) *
                                cachedInfo.frameRateDenominator &&
                        (!plan.hasAudio || cachedInfo.hasAudio);
                    if (cacheReady) {
                        completed.skipped = true;
                    }
                    if (SUCCEEDED(completed.status) && !cacheReady) {
                        DeleteFileW(temporaryPath.c_str());
                        completed.status = media::video::OptimizeVideo(
                            job.originalPath, temporaryPath.native(), plan,
                            stopToken);
                    }
                    media::MediaInfo optimizedInfo;
                    if (SUCCEEDED(completed.status) && !cacheReady) {
                        completed.status = media::ProbeMediaFile(
                            temporaryPath.native(), optimizedInfo);
                    }
                    if (SUCCEEDED(completed.status) && !cacheReady &&
                        (optimizedInfo.kind != media::WallpaperKind::Video ||
                         optimizedInfo.width != plan.outputWidth ||
                         optimizedInfo.height != plan.outputHeight ||
                         optimizedInfo.frameRateDenominator == 0 ||
                         static_cast<std::uint64_t>(
                             optimizedInfo.frameRateNumerator) *
                                 plan.frameRateDenominator !=
                             static_cast<std::uint64_t>(
                                 plan.frameRateNumerator) *
                                 optimizedInfo.frameRateDenominator ||
                         (plan.hasAudio && !optimizedInfo.hasAudio))) {
                        completed.status = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                    }
                    if (SUCCEEDED(completed.status) && !cacheReady &&
                        !MoveFileExW(temporaryPath.c_str(), optimizedPath.c_str(),
                                     MOVEFILE_REPLACE_EXISTING |
                                         MOVEFILE_WRITE_THROUGH)) {
                        completed.status = HRESULT_FROM_WIN32(GetLastError());
                    }
                    if (FAILED(completed.status)) {
                        DeleteFileW(temporaryPath.c_str());
                    } else if (!cacheReady) {
                        completed.optimized = true;
                    }
                }

                {
                    const std::scoped_lock lock(videoOptimizationMutex_);
                    videoOptimizationJobActive_ = false;
                    pendingVideoOptimizationResults_.push_back(
                        std::move(completed));
                }
                if (!stopToken.stop_requested() &&
                    IsWindow(notificationWindow)) {
                    PostMessageW(notificationWindow,
                                 kVideoOptimizationResultMessage, 0, 0);
                }
            }
            if (SUCCEEDED(comResult)) {
                CoUninitialize();
            }
        });
}

void WallpaperApplication::StopVideoOptimizationThread() {
    if (videoOptimizationThread_.joinable()) {
        videoOptimizationThread_.request_stop();
        videoOptimizationWake_.notify_all();
        videoOptimizationThread_.join();
    }
    const std::scoped_lock lock(videoOptimizationMutex_);
    videoOptimizationQueue_.clear();
    pendingVideoOptimizationResults_.clear();
    videoOptimizationJobActive_ = false;
    videoOptimizationBatchOptimized_ = 0;
    videoOptimizationBatchSkipped_ = 0;
    videoOptimizationBatchFailed_ = 0;
}

void WallpaperApplication::CompleteVideoOptimizations() {
    std::deque<VideoOptimizationResult> completed;
    bool batchComplete = false;
    {
        const std::scoped_lock lock(videoOptimizationMutex_);
        completed.swap(pendingVideoOptimizationResults_);
        batchComplete = videoOptimizationQueue_.empty() &&
                        !videoOptimizationJobActive_;
    }
    if (completed.empty()) {
        return;
    }

    std::size_t optimizedCount = 0;
    std::size_t skippedCount = 0;
    std::size_t failedCount = 0;
    for (const VideoOptimizationResult& result : completed) {
        if (result.optimized) {
            ++optimizedCount;
        } else if (result.skipped) {
            ++skippedCount;
        } else {
            ++failedCount;
            core::LogError(L"Wallpaper video optimization failed; the original "
                           L"file remains available: " +
                               result.originalPath,
                               result.status);
        }
    }
    videoOptimizationBatchOptimized_ += optimizedCount;
    videoOptimizationBatchSkipped_ += skippedCount;
    videoOptimizationBatchFailed_ += failedCount;
    if (!batchComplete) {
        return;
    }

    std::wstring status;
    if (videoOptimizationBatchOptimized_ > 0) {
        status = L"视频压缩完成 " +
                 std::to_wstring(videoOptimizationBatchOptimized_) +
                 L" 项 · 保留原始帧率";
        if (videoOptimizationBatchSkipped_ > 0) {
            status += L" · " +
                      std::to_wstring(videoOptimizationBatchSkipped_) +
                      L" 项无需压缩";
        }
    } else if (videoOptimizationBatchSkipped_ > 0 &&
               videoOptimizationBatchFailed_ == 0) {
        const wchar_t* message =
            L"导入的壁纸分辨率小于或等于屏幕分辨率，没有执行压缩。";
        status = message;
        MessageBoxW(controlWindow_, message, kApplicationTitle,
                    MB_OK | MB_ICONINFORMATION);
    }
    if (videoOptimizationBatchFailed_ > 0) {
        if (!status.empty()) {
            status += L" · ";
        }
        status += std::to_wstring(videoOptimizationBatchFailed_) +
                  L" 项压缩失败，继续使用原始文件";
    }
    if (!status.empty()) {
        mainWindow_.SetStatus(std::move(status));
    }
    videoOptimizationBatchOptimized_ = 0;
    videoOptimizationBatchSkipped_ = 0;
    videoOptimizationBatchFailed_ = 0;
}

void WallpaperApplication::ChooseExport() {
    if (wallpaperLibrary_.Scan().empty()) {
        MessageBoxW(controlWindow_, L"“全部壁纸”中还没有可导出的壁纸。",
                     kApplicationTitle, MB_OK | MB_ICONINFORMATION);
        return;
    }
    mainWindow_.BeginExportSelection();
    mainWindow_.SetStatus(L"已进入多选 · 可批量分组、收藏、导出或删除");
}

void WallpaperApplication::ExportWallpapers(
    const std::vector<core::WallpaperItem>& items) {
    if (items.empty()) {
        MessageBoxW(controlWindow_, L"请至少选择一张壁纸。", kApplicationTitle,
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    Microsoft::WRL::ComPtr<IFileSaveDialog> dialog;
    HRESULT result = CoCreateInstance(CLSID_FileSaveDialog, nullptr,
                                      CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(result)) {
        return;
    }
    const COMDLG_FILTERSPEC filter{L"ZIP 壁纸分享包 (*.zip)", L"*.zip"};
    result = dialog->SetFileTypes(1, &filter);
    if (SUCCEEDED(result)) {
        result = dialog->SetDefaultExtension(L"zip");
    }
    const std::wstring suggestedName =
        items.size() == 1
            ? items.front().path.stem().native() + L"-分享包.zip"
            : L"LiveWallpaper-壁纸分享包.zip";
    if (SUCCEEDED(result)) {
        result = dialog->SetFileName(suggestedName.c_str());
    }
    if (SUCCEEDED(result)) {
        result = dialog->SetTitle(L"导出可分享壁纸包");
    }
    if (SUCCEEDED(result)) {
        result = dialog->Show(controlWindow_);
    }
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return;
    }

    Microsoft::WRL::ComPtr<IShellItem> destinationItem;
    PWSTR destination = nullptr;
    if (SUCCEEDED(result)) {
        result = dialog->GetResult(&destinationItem);
    }
    if (SUCCEEDED(result)) {
        result = destinationItem->GetDisplayName(SIGDN_FILESYSPATH, &destination);
    }
    if (SUCCEEDED(result) && destination != nullptr) {
        result = wallpaperLibrary_.ExportArchive(items, destination);
    }
    CoTaskMemFree(destination);

    if (FAILED(result)) {
        std::wstring message = L"ZIP 壁纸分享包导出失败。\r\n\r\n";
        message += core::HResultMessage(result);
        MessageBoxW(controlWindow_, message.c_str(), kApplicationTitle,
                    MB_OK | MB_ICONERROR);
        return;
    }
    mainWindow_.EndExportSelection();
    mainWindow_.SetStatus(L"已导出 " + std::to_wstring(items.size()) +
                          L" 张壁纸 · ZIP 分享包可以直接发送给其他用户");
}

void WallpaperApplication::ApplySelectedWallpaper() {
    const std::optional selected = mainWindow_.SelectedItem();
    if (!selected.has_value()) {
        MessageBoxW(controlWindow_, L"请先选择一项壁纸。", kApplicationTitle,
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    ApplyWallpaperWithTargetPrompt(selected->path.native());
}

bool WallpaperApplication::ApplyWallpaperWithTargetPrompt(
    const std::wstring_view path, const bool persistSelection,
    const bool showErrors) {
    if (!ChooseApplicationTargets()) {
        return false;
    }
    return ApplyWallpaper(path, persistSelection, showErrors);
}

void WallpaperApplication::PreviewSelectedWallpaper() {
    const std::optional selected = mainWindow_.SelectedItem();
    if (!selected.has_value()) {
        return;
    }
    PreviewWallpaper(*selected);
}

void WallpaperApplication::PreviewWallpaper(const core::WallpaperItem& item) {
    // Use the registered Windows preview/player so image, GIF and video
    // previews remain codec-aware without starting a second decoder inside the
    // lightweight wallpaper process.
    const HINSTANCE opened = ShellExecuteW(controlWindow_, L"open",
                                           item.path.c_str(), nullptr,
                                           item.path.parent_path().c_str(),
                                           SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(opened) <= 32) {
        MessageBoxW(controlWindow_, L"Windows 无法打开该壁纸的预览程序。",
                    kApplicationTitle, MB_OK | MB_ICONERROR);
    }
}

void WallpaperApplication::OpenWallpaperLocation(
    const core::WallpaperItem& item) {
    const std::wstring parameters = L"/select,\"" + item.path.native() + L"\"";
    const HINSTANCE opened = ShellExecuteW(
        controlWindow_, L"open", L"explorer.exe", parameters.c_str(), nullptr,
        SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(opened) <= 32) {
        MessageBoxW(controlWindow_, L"无法在文件资源管理器中定位该壁纸。",
                    kApplicationTitle, MB_OK | MB_ICONERROR);
    }
}

void WallpaperApplication::RemoveWallpaperFromLibrary(
    const core::WallpaperItem& item) {
    if (item.external) {
        return;
    }
    const bool active = std::ranges::any_of(
        assignments_, [&](const core::WallpaperAssignmentSetting& assignment) {
            return SamePath(assignment.wallpaperPath, item.path.native());
        });
    std::wstring question = L"确定从“全部壁纸”中删除“" + item.displayName +
                            L"”吗？\r\n\r\n";
    if (active) {
        question += L"该壁纸正在使用，删除前会先取消应用。\r\n";
    }
    question += L"只删除软件本地壁纸库中的副本，不影响最初导入的源文件。";
    if (MessageBoxW(controlWindow_, question.c_str(), kApplicationTitle,
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    if (active) {
        CancelWallpaper(item, true);
    }
    const HRESULT result = wallpaperLibrary_.Remove(item);
    if (FAILED(result)) {
        std::wstring message = L"无法从“全部壁纸”中删除该壁纸。\r\n\r\n";
        message += core::HResultMessage(result);
        MessageBoxW(controlWindow_, message.c_str(), kApplicationTitle,
                    MB_OK | MB_ICONERROR);
        RefreshLibrary();
        return;
    }
    const HRESULT groupResult =
        groupStore_.RemoveWallpaperKey(item.path.filename().native());
    if (FAILED(groupResult)) {
        core::LogError(L"Removed wallpaper group metadata could not be updated.",
                       groupResult);
    }
    RefreshLibrary();
    mainWindow_.SetStatus(L"已从“全部壁纸”中删除 · " + item.displayName);
}

void WallpaperApplication::CommitLibraryOrder() {
    const auto order = mainWindow_.TakePendingLibraryOrder();
    if (!order.has_value()) {
        return;
    }
    const HRESULT result = wallpaperLibrary_.Reorder(*order);
    if (FAILED(result)) {
        std::wstring message = L"壁纸顺序保存失败。\r\n\r\n";
        message += core::HResultMessage(result);
        MessageBoxW(controlWindow_, message.c_str(), kApplicationTitle,
                    MB_OK | MB_ICONERROR);
    }
    RefreshLibrary();
    if (SUCCEEDED(result)) {
        mainWindow_.SetStatus(L"壁纸顺序已保存");
    }
}

void WallpaperApplication::CommitWallpaperRename() {
    const auto rename = mainWindow_.FinishRename();
    if (!rename.has_value()) {
        return;
    }
    const core::WallpaperItem source = rename->first;
    const bool wasActive = std::ranges::any_of(
        assignments_, [&](const core::WallpaperAssignmentSetting& assignment) {
            return SamePath(assignment.wallpaperPath, source.path.native());
        });
    if (wasActive) {
        StopAllPlayback();
    }
    core::WallpaperItem renamed;
    const HRESULT result = wallpaperLibrary_.Rename(source, rename->second, renamed);
    if (FAILED(result)) {
        if (wasActive) {
            RebuildPlaybackSessions(false);
        }
        std::wstring message = L"壁纸重命名失败。\r\n\r\n";
        message += core::HResultMessage(result);
        MessageBoxW(controlWindow_, message.c_str(), kApplicationTitle,
                    MB_OK | MB_ICONERROR);
        return;
    }
    const HRESULT groupResult = groupStore_.ReplaceWallpaperKey(
        source.path.filename().native(), renamed.path.filename().native());
    if (FAILED(groupResult)) {
        core::LogError(L"Renamed wallpaper group metadata could not be updated.",
                       groupResult);
    }
    if (wasActive) {
        for (auto& assignment : assignments_) {
            if (SamePath(assignment.wallpaperPath, source.path.native())) {
                assignment.wallpaperPath = renamed.path.native();
            }
        }
        if (!RebuildPlaybackSessions(true) || FAILED(SaveCurrentSelection())) {
            MessageBoxW(controlWindow_,
                        L"壁纸已重命名，但播放状态或本地设置恢复失败。",
                        kApplicationTitle, MB_OK | MB_ICONWARNING);
        }
    }
    RefreshLibrary();
    mainWindow_.SetStatus(L"壁纸已重命名 · " + renamed.displayName);
}

void WallpaperApplication::ShowLibraryContextMenu(POINT screenPoint) {
    if (screenPoint.x == -1 && screenPoint.y == -1) {
        const LRESULT selection =
            SendMessageW(mainWindow_.LibraryControl(), LB_GETCURSEL, 0, 0);
        RECT item{};
        if (selection < 0 ||
            SendMessageW(mainWindow_.LibraryControl(), LB_GETITEMRECT, selection,
                         reinterpret_cast<LPARAM>(&item)) == LB_ERR) {
            return;
        }
        screenPoint = POINT{item.left + 24, item.bottom};
        ClientToScreen(mainWindow_.LibraryControl(), &screenPoint);
    } else if (!mainWindow_.SelectItemAtScreenPoint(screenPoint)) {
        return;
    }

    const std::optional selected = mainWindow_.SelectedItem();
    if (!selected.has_value()) {
        return;
    }
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    AppendMenuW(menu, MF_STRING, kLibraryPreviewCommand, L"预览壁纸");
    AppendMenuW(menu, MF_STRING | (selected->external ? MF_GRAYED : 0),
                kLibraryRenameCommand, L"重命名");
    AppendMenuW(menu, MF_STRING, kLibraryOpenLocationCommand,
                L"打开文件所在位置");
    AppendMenuW(menu, MF_STRING, kLibraryExportCommand, L"导出分享包");
    AppendMenuW(menu, MF_STRING, kLibraryMultiSelectCommand, L"多选");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kLibraryApplyCommand, L"应用到所选屏幕");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    HMENU addToGroup = CreatePopupMenu();
    if (addToGroup != nullptr) {
        if (groupStore_.Groups().empty()) {
            AppendMenuW(addToGroup, MF_STRING | MF_GRAYED, 0, L"暂无自定义分组");
        } else {
            for (std::size_t index = 0; index < groupStore_.Groups().size(); ++index) {
                const bool alreadyMember = !selected->external &&
                    groupStore_.IsInGroup(
                        groupStore_.Groups()[index].id,
                        selected->path.filename().native());
                AppendMenuW(addToGroup,
                            MF_STRING | (alreadyMember ? MF_CHECKED : 0),
                            kAddToGroupCommandBase + static_cast<UINT>(index),
                            groupStore_.Groups()[index].name.c_str());
            }
        }
        AppendMenuW(menu, MF_POPUP | (selected->external ? MF_GRAYED : 0),
                    reinterpret_cast<UINT_PTR>(addToGroup), L"添加到分组");
    }
    const bool favorite = !selected->external && groupStore_.IsFavorite(
        selected->path.filename().native());
    if (!mainWindow_.CurrentGroupIsFavorites()) {
        AppendMenuW(menu, MF_STRING | (selected->external ? MF_GRAYED : 0),
                    favorite ? kLibraryRemoveFavoriteCommand
                             : kLibraryAddFavoriteCommand,
                    favorite ? L"从最爱壁纸中移除" : L"添加到最爱");
    }
    if (!mainWindow_.CurrentGroupIsAll()) {
        AppendMenuW(menu, MF_STRING | (selected->external ? MF_GRAYED : 0),
                    kLibraryRemoveFromGroupCommand,
                    mainWindow_.CurrentGroupIsFavorites()
                        ? L"从最爱壁纸中移除"
                        : L"从该分组中移除");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (selected->external ? MF_GRAYED : 0),
                kLibraryRemoveCommand, L"从全部壁纸中删除");
    const UINT command = TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, screenPoint.x,
        screenPoint.y, 0, controlWindow_, nullptr);
    DestroyMenu(menu);
    if (command == kLibraryPreviewCommand) {
        PreviewSelectedWallpaper();
    } else if (command == kLibraryRenameCommand) {
        mainWindow_.BeginRenameSelected();
    } else if (command == kLibraryOpenLocationCommand) {
        OpenWallpaperLocation(*selected);
    } else if (command == kLibraryExportCommand) {
        ExportWallpapers({*selected});
    } else if (command == kLibraryMultiSelectCommand) {
        mainWindow_.BeginExportSelection(selected->path.native());
        mainWindow_.SetStatus(L"已进入多选 · 可批量分组、收藏、导出或删除");
    } else if (command == kLibraryApplyCommand) {
        ApplySelectedWallpaper();
    } else if (command == kLibraryAddFavoriteCommand) {
        SetWallpapersFavorite(std::span<const core::WallpaperItem>(&*selected, 1), true);
    } else if (command == kLibraryRemoveFavoriteCommand) {
        SetWallpapersFavorite(std::span<const core::WallpaperItem>(&*selected, 1), false);
    } else if (command == kLibraryRemoveFromGroupCommand) {
        RemoveWallpapersFromCurrentGroup(
            std::span<const core::WallpaperItem>(&*selected, 1));
    } else if (command == kLibraryRemoveCommand) {
        RemoveWallpaperFromLibrary(*selected);
    } else if (command >= kAddToGroupCommandBase &&
               command < kAddToGroupCommandBase + groupStore_.Groups().size()) {
        const auto& group =
            groupStore_.Groups()[command - kAddToGroupCommandBase];
        AddWallpapersToGroup(std::span<const core::WallpaperItem>(&*selected, 1),
                             group.id);
    }
}

void WallpaperApplication::ShowGroupContextMenu(POINT screenPoint) {
    if (screenPoint.x == -1 && screenPoint.y == -1) {
        const auto group = mainWindow_.SelectedCustomGroup();
        if (!group.has_value()) {
            return;
        }
        const LRESULT selection = SendMessageW(mainWindow_.GroupListControl(),
                                               LB_GETCURSEL, 0, 0);
        RECT item{};
        if (selection < 0 ||
            SendMessageW(mainWindow_.GroupListControl(), LB_GETITEMRECT, selection,
                         reinterpret_cast<LPARAM>(&item)) == LB_ERR) {
            return;
        }
        screenPoint = POINT{item.left + 20, item.bottom};
        ClientToScreen(mainWindow_.GroupListControl(), &screenPoint);
    } else if (!mainWindow_.SelectCustomGroupAtScreenPoint(screenPoint)) {
        return;
    }
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    AppendMenuW(menu, MF_STRING, kGroupRenameCommand, L"重命名分组");
    AppendMenuW(menu, MF_STRING, kGroupDeleteCommand, L"删除分组");
    const UINT command = TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, screenPoint.x,
        screenPoint.y, 0, controlWindow_, nullptr);
    DestroyMenu(menu);
    if (command == kGroupRenameCommand) {
        mainWindow_.BeginRenameSelectedGroup();
    } else if (command == kGroupDeleteCommand) {
        DeleteWallpaperGroup();
    }
}

void WallpaperApplication::ShowBatchActionsMenu() {
    const std::vector<core::WallpaperItem> selected =
        mainWindow_.SelectedExportItems();
    if (selected.empty()) {
        return;
    }
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    HMENU addToGroup = CreatePopupMenu();
    if (addToGroup != nullptr) {
        if (groupStore_.Groups().empty()) {
            AppendMenuW(addToGroup, MF_STRING | MF_GRAYED, 0, L"暂无自定义分组");
        } else {
            for (std::size_t index = 0; index < groupStore_.Groups().size(); ++index) {
                AppendMenuW(addToGroup, MF_STRING,
                            kAddToGroupCommandBase + static_cast<UINT>(index),
                            groupStore_.Groups()[index].name.c_str());
            }
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(addToGroup),
                    L"批量添加到分组");
    }
    if (!mainWindow_.CurrentGroupIsFavorites()) {
        AppendMenuW(menu, MF_STRING, kBatchAddFavoriteCommand, L"添加到最爱");
        AppendMenuW(menu, MF_STRING, kBatchRemoveFavoriteCommand,
                    L"从最爱中移除");
    }
    if (!mainWindow_.CurrentGroupIsAll()) {
        AppendMenuW(menu, MF_STRING, kBatchRemoveFromGroupCommand,
                    mainWindow_.CurrentGroupIsFavorites()
                        ? L"从最爱壁纸中移除"
                        : L"从当前分组移除");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kBatchExportCommand, L"导出分享包");
    if (mainWindow_.CurrentGroupIsAll()) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kBatchDeleteCommand, L"从本地库删除");
    }

    RECT anchor{};
    GetWindowRect(mainWindow_.BatchActionsControl(), &anchor);
    const UINT command = TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_RIGHTALIGN,
        anchor.right, anchor.bottom + 4, 0, controlWindow_, nullptr);
    DestroyMenu(menu);
    if (command == kBatchExportCommand) {
        ExportWallpapers(selected);
    } else if (command == kBatchAddFavoriteCommand) {
        SetWallpapersFavorite(selected, true);
        mainWindow_.EndExportSelection();
    } else if (command == kBatchRemoveFavoriteCommand) {
        SetWallpapersFavorite(selected, false);
        mainWindow_.EndExportSelection();
    } else if (command == kBatchRemoveFromGroupCommand) {
        RemoveWallpapersFromCurrentGroup(selected);
    } else if (command == kBatchDeleteCommand) {
        DeleteWallpapers(selected);
    } else if (command >= kAddToGroupCommandBase &&
               command < kAddToGroupCommandBase + groupStore_.Groups().size()) {
        AddWallpapersToGroup(selected,
                             groupStore_.Groups()[command -
                                                  kAddToGroupCommandBase]
                                 .id);
        mainWindow_.EndExportSelection();
    }
}

void WallpaperApplication::ShowActiveWallpaperContextMenu(POINT screenPoint) {
    if (screenPoint.x == -1 && screenPoint.y == -1) {
        const LRESULT selection = SendMessageW(
            mainWindow_.ActiveLibraryControl(), LB_GETCURSEL, 0, 0);
        RECT item{};
        if (selection < 0 ||
            SendMessageW(mainWindow_.ActiveLibraryControl(), LB_GETITEMRECT,
                         selection, reinterpret_cast<LPARAM>(&item)) == LB_ERR) {
            return;
        }
        screenPoint = POINT{item.left + 24, item.bottom};
        ClientToScreen(mainWindow_.ActiveLibraryControl(), &screenPoint);
    } else if (!mainWindow_.SelectActiveItemAtScreenPoint(screenPoint)) {
        return;
    }

    const std::optional selected = mainWindow_.SelectedActiveItem();
    if (!selected.has_value()) {
        return;
    }
    constexpr int kActivePreviewCommand = 2210;
    constexpr int kActiveRenameCommand = 2211;
    constexpr int kActiveCancelCommand = 2212;
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    AppendMenuW(menu, MF_STRING, kActivePreviewCommand, L"预览壁纸");
    AppendMenuW(menu, MF_STRING | (selected->external ? MF_GRAYED : 0),
                kActiveRenameCommand, L"重命名");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kActiveCancelCommand, L"取消应用此壁纸");
    const UINT command = TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, screenPoint.x,
        screenPoint.y, 0, controlWindow_, nullptr);
    DestroyMenu(menu);
    if (command == kActivePreviewCommand) {
        PreviewWallpaper(*selected);
    } else if (command == kActiveRenameCommand) {
        mainWindow_.BeginRenameActiveSelected();
    } else if (command == kActiveCancelCommand) {
        CancelWallpaper(*selected);
    }
}

void WallpaperApplication::CancelActiveWallpaper(const bool persistSelection) {
    const std::scoped_lock playbackLock(playbackMutex_);
    StopAllPlayback();
    assignments_.clear();
    playbackMode_ = PlaybackMode::Stopped;
    manualPlaybackPaused_ = false;
    if (IsWindow(wallpaperWindow_)) {
        ShowWindow(wallpaperWindow_, SW_HIDE);
    }
    mainWindow_.SetActiveWallpapers({});
    mainWindow_.SetStatus(L"当前未应用壁纸 · Windows 原壁纸已恢复显示");
    RefreshLibrary();
    if (persistSelection && FAILED(SaveCurrentSelection())) {
        MessageBoxW(controlWindow_, L"壁纸已取消，但本地设置保存失败。",
                    kApplicationTitle, MB_OK | MB_ICONWARNING);
    }
}

void WallpaperApplication::CancelWallpaper(
    const core::WallpaperItem& item, const bool persistSelection) {
    const bool isActive = std::ranges::any_of(
        assignments_, [&](const core::WallpaperAssignmentSetting& assignment) {
            return SamePath(assignment.wallpaperPath, item.path.native());
        });
    if (!isActive) {
        return;
    }
    const std::scoped_lock playbackLock(playbackMutex_);

    // Removing one wallpaper must not rebuild unrelated video decoders. Their
    // clocks and transfer surfaces remain alive, so canceling an image on one
    // display cannot introduce a visible hitch on another display's video.
    std::erase_if(playbackSessions_, [&](const auto& session) {
        const bool matches =
            SamePath(session->assignment.wallpaperPath, item.path.native());
        if (matches && session->gifPlayer) {
            session->gifPlayer->Reset();
        }
        if (matches && session->videoPlayer) {
            session->videoPlayer->Shutdown();
        }
        return matches;
    });
    std::erase_if(assignments_, [&](const auto& assignment) {
        return SamePath(assignment.wallpaperPath, item.path.native());
    });

    if (assignments_.empty()) {
        playbackMode_ = PlaybackMode::Stopped;
        pendingWallpaperReveal_ = false;
        dynamicPlaybackPaused_ = false;
        manualPlaybackPaused_ = false;
        if (IsWindow(wallpaperWindow_)) {
            ShowWindow(wallpaperWindow_, SW_HIDE);
        }
    } else {
        playbackMode_ = PlaybackMode::Active;
        ConfigureWallpaperWindowRegion();
    }
    mainWindow_.SetActiveWallpapers(ActiveWallpapers());
    mainWindow_.SetStatus(ActivePlaybackStatus());
    RefreshLibrary();
    WakePlaybackRenderThread();
    if (persistSelection && FAILED(SaveCurrentSelection())) {
        MessageBoxW(controlWindow_, L"壁纸已取消，但本地设置保存失败。",
                    kApplicationTitle, MB_OK | MB_ICONWARNING);
    }
}

bool WallpaperApplication::ApplyWallpaper(const std::wstring_view path,
                                          const bool persistSelection,
                                          const bool showErrors) {
    media::MediaInfo info;
    HRESULT result = media::ProbeMediaFile(path, info);
    if (FAILED(result)) {
        if (showErrors) {
            MessageBoxW(controlWindow_,
                        L"无法识别该文件的实际图片容器或视频编码。",
                        kApplicationTitle, MB_OK | MB_ICONERROR);
        }
        return false;
    }
    if ((wallpaperWindow_ == nullptr || !IsWindow(wallpaperWindow_)) &&
        !CreateWallpaperWindow()) {
        return false;
    }
    const auto previousAssignments = assignments_;
    core::WallpaperAssignmentSetting newAssignment{
        SettingsKind(info.kind), std::wstring(path),
        JoinDisplayIds(selectedDisplayIds_), spanAcrossDisplays_};

    if (spanAcrossDisplays_) {
        assignments_.clear();
        assignments_.push_back(std::move(newAssignment));
    } else {
        // Replacing selected screens must leave every other screen untouched.
        // A former span assignment is first expanded to concrete display IDs,
        // then the selected IDs are removed from it.
        for (auto& assignment : assignments_) {
            std::vector<std::wstring> identifiers =
                assignment.spanAcrossDisplays
                    ? std::vector<std::wstring>{}
                    : SplitDisplayIds(assignment.displayTargets);
            if (assignment.spanAcrossDisplays) {
                identifiers.reserve(displayTargets_.size());
                for (const auto& display : displayTargets_) {
                    identifiers.push_back(display.deviceId);
                }
            }
            std::erase_if(identifiers, [&](const std::wstring& identifier) {
                return ContainsDisplayId(selectedDisplayIds_, identifier);
            });
            assignment.displayTargets = JoinDisplayIds(identifiers);
            assignment.spanAcrossDisplays = false;
        }
        std::erase_if(assignments_, [](const auto& assignment) {
            return !assignment.spanAcrossDisplays &&
                   assignment.displayTargets.empty();
        });

        auto matching = std::ranges::find_if(
            assignments_, [&](const auto& assignment) {
                return !assignment.spanAcrossDisplays &&
                       assignment.wallpaperKind == newAssignment.wallpaperKind &&
                       SamePath(assignment.wallpaperPath,
                                newAssignment.wallpaperPath);
            });
        if (matching == assignments_.end()) {
            assignments_.push_back(std::move(newAssignment));
        } else {
            std::vector<std::wstring> identifiers =
                SplitDisplayIds(matching->displayTargets);
            for (const std::wstring& selectedId : selectedDisplayIds_) {
                if (!ContainsDisplayId(identifiers, selectedId)) {
                    identifiers.push_back(selectedId);
                }
            }
            matching->displayTargets = JoinDisplayIds(identifiers);
        }
    }

    NormalizeAssignments();
    if (!AssignmentsUseUniqueDisplays(assignments_, displayTargets_)) {
        assignments_ = previousAssignments;
        core::LogError(L"Wallpaper assignment replacement left a display claimed twice.",
                       E_UNEXPECTED);
        if (showErrors) {
            MessageBoxW(controlWindow_,
                        L"无法完成屏幕壁纸替换，原来的屏幕分配保持不变。",
                        kApplicationTitle, MB_OK | MB_ICONERROR);
        }
        return false;
    }

    if (!RebuildPlaybackSessions(showErrors)) {
        assignments_ = previousAssignments;
        RebuildPlaybackSessions(false);
        if (showErrors) {
            MessageBoxW(controlWindow_, L"壁纸无法加载或播放。",
                        kApplicationTitle,
                        MB_OK | MB_ICONERROR);
        }
        return false;
    }

    HRESULT saveResult = S_OK;
    if (persistSelection) {
        saveResult = SaveCurrentSelection();
    }
    mainWindow_.SetActiveWallpapers(ActiveWallpapers());
    mainWindow_.SetStatus(ActivePlaybackStatus());
    mainWindow_.SetSoundEnabled(soundEnabled_);
    RefreshLibrary();

    if (persistSelection && FAILED(saveResult) && showErrors) {
        MessageBoxW(controlWindow_, L"壁纸已应用，但本地设置保存失败。",
                    kApplicationTitle, MB_OK | MB_ICONWARNING);
    }
    return true;
}

bool WallpaperApplication::NormalizeAssignments() {
    if (assignments_.empty() || displayTargets_.empty()) {
        return false;
    }

    const std::vector<core::WallpaperAssignmentSetting> previous = assignments_;
    std::vector<core::WallpaperAssignmentSetting> newestFirst;
    std::vector<std::wstring> claimedDisplays;
    newestFirst.reserve(assignments_.size());

    for (auto assignment = assignments_.rbegin(); assignment != assignments_.rend();
         ++assignment) {
        std::vector<std::wstring> requestedDisplays;
        if (assignment->spanAcrossDisplays) {
            requestedDisplays.reserve(displayTargets_.size());
            for (const shell::DisplayTarget& display : displayTargets_) {
                requestedDisplays.push_back(display.deviceId);
            }
        } else {
            requestedDisplays = SplitDisplayIds(assignment->displayTargets);
        }

        std::vector<std::wstring> availableDisplays;
        for (const shell::DisplayTarget& display : displayTargets_) {
            if (ContainsDisplayId(requestedDisplays, display.deviceId) &&
                !ContainsDisplayId(claimedDisplays, display.deviceId)) {
                availableDisplays.push_back(display.deviceId);
            }
        }
        if (availableDisplays.empty()) {
            continue;
        }

        core::WallpaperAssignmentSetting normalized = *assignment;
        if (assignment->spanAcrossDisplays && claimedDisplays.empty() &&
            availableDisplays.size() == displayTargets_.size()) {
            normalized.displayTargets.clear();
            normalized.spanAcrossDisplays = true;
            newestFirst.push_back(std::move(normalized));
            break;
        }
        normalized.displayTargets = JoinDisplayIds(availableDisplays);
        normalized.spanAcrossDisplays = false;
        newestFirst.push_back(std::move(normalized));
        claimedDisplays.insert(claimedDisplays.end(), availableDisplays.begin(),
                               availableDisplays.end());
    }

    std::ranges::reverse(newestFirst);
    std::vector<core::WallpaperAssignmentSetting> merged;
    for (core::WallpaperAssignmentSetting& assignment : newestFirst) {
        if (assignment.spanAcrossDisplays) {
            merged.clear();
            merged.push_back(std::move(assignment));
            break;
        }
        auto existing = std::ranges::find_if(
            merged, [&](const core::WallpaperAssignmentSetting& candidate) {
                return !candidate.spanAcrossDisplays &&
                       candidate.wallpaperKind == assignment.wallpaperKind &&
                       SamePath(candidate.wallpaperPath, assignment.wallpaperPath);
            });
        if (existing == merged.end()) {
            merged.push_back(std::move(assignment));
            continue;
        }
        std::vector<std::wstring> identifiers =
            SplitDisplayIds(existing->displayTargets);
        for (const std::wstring& identifier :
             SplitDisplayIds(assignment.displayTargets)) {
            if (!ContainsDisplayId(identifiers, identifier)) {
                identifiers.push_back(identifier);
            }
        }
        existing->displayTargets = JoinDisplayIds(identifiers);
    }
    assignments_ = std::move(merged);

    const auto sameAssignment = [](const core::WallpaperAssignmentSetting& left,
                                   const core::WallpaperAssignmentSetting& right) {
        return left.wallpaperKind == right.wallpaperKind &&
               SamePath(left.wallpaperPath, right.wallpaperPath) &&
               SamePath(left.displayTargets, right.displayTargets) &&
               left.spanAcrossDisplays == right.spanAcrossDisplays;
    };
    const bool changed = previous.size() != assignments_.size() ||
                         !std::ranges::equal(previous, assignments_, sameAssignment);
    if (changed) {
        core::LogInfo(
            L"Normalized wallpaper assignments so each display has one wallpaper.");
    }
    return changed;
}

bool WallpaperApplication::RebuildPlaybackSessions(const bool showErrors) {
    const std::scoped_lock playbackLock(playbackMutex_);
    StopAllPlayback();
    if (assignments_.empty()) {
        playbackMode_ = PlaybackMode::Stopped;
        if (IsWindow(wallpaperWindow_)) {
            ShowWindow(wallpaperWindow_, SW_HIDE);
        }
        return true;
    }
    if (!EnsureRenderer()) {
        return false;
    }
    if (!ConfigureWallpaperWindowRegion()) {
        return false;
    }

    bool presentedImmediately = false;
    for (const auto& assignment : assignments_) {
        auto session = std::make_unique<WallpaperSession>();
        session->token = nextSessionToken_++;
        if (nextSessionToken_ == 0) {
            nextSessionToken_ = 1;
        }
        session->assignment = assignment;
        session->kind = MediaKind(assignment.wallpaperKind);
        session->destinations = DestinationsForAssignment(assignment);
        if (session->destinations.empty()) {
            continue;
        }
        const HRESULT result = StartWallpaperSession(*session);
        if (FAILED(result)) {
            StopAllPlayback();
            if (showErrors) {
                std::wstring message = L"壁纸会话启动失败。\r\n\r\n";
                message += core::HResultMessage(result);
                MessageBoxW(controlWindow_, message.c_str(), kApplicationTitle,
                            MB_OK | MB_ICONERROR);
            }
            return false;
        }
        std::wstring sessionMessage =
            L"Wallpaper session " + std::to_wstring(session->token) + L" started: ";
        sessionMessage += media::WallpaperKindLabel(session->kind);
        sessionMessage += L", targets=";
        sessionMessage += assignment.spanAcrossDisplays
                              ? std::wstring(L"span_all")
                              : assignment.displayTargets;
        core::LogInfo(sessionMessage);
        presentedImmediately = presentedImmediately ||
                               session->kind != media::WallpaperKind::Video;
        playbackSessions_.push_back(std::move(session));
    }
    if (playbackSessions_.empty()) {
        return false;
    }
    playbackMode_ = PlaybackMode::Active;
    pendingWallpaperReveal_ =
        !presentedImmediately && !IsWindowVisible(wallpaperWindow_);
    if (presentedImmediately) {
        ShowWindow(wallpaperWindow_, SW_SHOWNOACTIVATE);
    }
    RefreshPlaybackPolicy();
    WakePlaybackRenderThread();
    return true;
}

HRESULT WallpaperApplication::StartWallpaperSession(WallpaperSession& session) {
    if (session.destinations.empty()) {
        return E_INVALIDARG;
    }
    if (session.kind == media::WallpaperKind::StaticImage) {
        const HRESULT result = RenderStaticImage(
            session.assignment.wallpaperPath, session.destinations);
        if (SUCCEEDED(result)) {
            core::LogInfo(L"Static wallpaper session presented one frame.");
        }
        return result;
    }
    if (session.kind == media::WallpaperKind::AnimatedGif) {
        RECT client{};
        if (!GetClientRect(wallpaperWindow_, &client)) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        session.gifPlayer = std::make_unique<media::image::GifPlayer>();
        HRESULT result = session.gifPlayer->Load(
            session.assignment.wallpaperPath, static_cast<UINT>(client.right),
            static_cast<UINT>(client.bottom));
        if (SUCCEEDED(result)) {
            session.gifPlayer->SetTargetRects(session.destinations);
        }
        if (SUCCEEDED(result) && !session.gifPlayer->PresentDue(
                                     renderer_, std::chrono::steady_clock::now())) {
            result = E_FAIL;
        }
        if (SUCCEEDED(result)) {
            core::LogInfo(L"Animated GIF overlay mode activated.");
        }
        return result;
    }
    if (!mediaFoundationStarted_) {
        return MF_E_PLATFORM_NOT_INITIALIZED;
    }
    session.videoPlayer = std::make_unique<media::video::MediaEnginePlayer>();
    UINT requiredWidth = 0;
    UINT requiredHeight = 0;
    for (const RECT& destination : session.destinations) {
        requiredWidth = std::max(
            requiredWidth,
            static_cast<UINT>(std::max(0L, destination.right - destination.left)));
        requiredHeight = std::max(
            requiredHeight,
            static_cast<UINT>(std::max(0L, destination.bottom - destination.top)));
    }
    const std::filesystem::path playbackPath =
        wallpaperLibrary_.ResolveVideoPlaybackPath(
            session.assignment.wallpaperPath, requiredWidth, requiredHeight);
    if (!SamePath(playbackPath.native(), session.assignment.wallpaperPath)) {
        core::LogInfo(L"Using a local optimized video copy: " +
                      playbackPath.native());
    }
    const HRESULT result = session.videoPlayer->Open(
        renderer_.Device(), controlWindow_, kMediaEngineEventMessage,
        playbackPath.native(), soundEnabled_, session.token);
    session.videoResourcesReleased = false;
    return result;
}

HRESULT WallpaperApplication::RenderStaticImage(
    const std::wstring_view path, const std::span<const RECT> destinations) {
    if (!renderer_.IsInitialized() || wallpaperWindow_ == nullptr) {
        return E_UNEXPECTED;
    }
    std::vector<media::image::DecodedImage> images;
    images.reserve(destinations.size());
    HRESULT result = S_OK;
    for (const RECT& destination : destinations) {
        const LONG width = destination.right - destination.left;
        const LONG height = destination.bottom - destination.top;
        if (width <= 0 || height <= 0) {
            return E_INVALIDARG;
        }
        media::image::DecodedImage image;
        result = imageLoader_.LoadFill(path, static_cast<UINT>(width),
                                       static_cast<UINT>(height), image);
        if (FAILED(result)) {
            return result;
        }
        images.push_back(std::move(image));
    }
    std::vector<render::ImageRegion> regions;
    regions.reserve(images.size());
    for (std::size_t index = 0; index < images.size(); ++index) {
        const auto& image = images[index];
        const RECT& destination = destinations[index];
        regions.push_back(render::ImageRegion{
            image.pixels, image.width, image.height, image.stride,
            destination.left, destination.top});
    }
    if (regions.empty() || !renderer_.PresentImageRegions(regions)) {
        result = E_FAIL;
    }
    return result;
}

void WallpaperApplication::StopAllPlayback() {
    const std::scoped_lock playbackLock(playbackMutex_);
    for (const auto& session : playbackSessions_) {
        if (session->gifPlayer) {
            session->gifPlayer->Reset();
        }
        if (session->videoPlayer) {
            session->videoPlayer->Shutdown();
        }
    }
    playbackSessions_.clear();
    dynamicPlaybackPaused_ = false;
    dynamicPauseStartedAt_.reset();
    pendingWallpaperReveal_ = false;
}

bool WallpaperApplication::RemoveFailedPlaybackSessions() {
    const std::scoped_lock playbackLock(playbackMutex_);
    std::vector<core::WallpaperAssignmentSetting> failedAssignments;
    for (const auto& session : playbackSessions_) {
        if (session->videoPlayer && session->videoPlayer->HasFailed()) {
            failedAssignments.push_back(session->assignment);
        }
    }
    if (failedAssignments.empty()) {
        return false;
    }

    // A decoder failure belongs to one wallpaper assignment. Keeping the
    // control window and unrelated display sessions alive prevents a bad or
    // transient video event from looking like an application crash.
    std::erase_if(playbackSessions_, [&](const auto& session) {
        const bool failed = session->videoPlayer && session->videoPlayer->HasFailed();
        if (failed) {
            session->videoPlayer->Shutdown();
        }
        return failed;
    });
    const auto assignmentFailed = [&](const auto& assignment) {
        return std::ranges::any_of(
            failedAssignments, [&](const auto& failed) {
                return assignment.wallpaperKind == failed.wallpaperKind &&
                       assignment.spanAcrossDisplays == failed.spanAcrossDisplays &&
                       SamePath(assignment.wallpaperPath, failed.wallpaperPath) &&
                       SamePath(assignment.displayTargets, failed.displayTargets);
            });
    };
    std::erase_if(assignments_, assignmentFailed);

    if (assignments_.empty()) {
        playbackMode_ = PlaybackMode::Stopped;
        pendingWallpaperReveal_ = false;
        if (IsWindow(wallpaperWindow_)) {
            ShowWindow(wallpaperWindow_, SW_HIDE);
        }
    } else {
        playbackMode_ = PlaybackMode::Active;
        ConfigureWallpaperWindowRegion();
    }
    mainWindow_.SetActiveWallpapers(ActiveWallpapers());
    mainWindow_.SetStatus(
        assignments_.empty()
            ? L"视频播放失败，已停止该壁纸 · 程序仍在运行"
            : L"一个视频播放失败并已停止 · 其他屏幕继续运行");
    RefreshLibrary();
    if (!controlledTestMode_ && FAILED(SaveCurrentSelection())) {
        core::LogWarning(
            L"Failed video assignment was removed but settings could not be saved.");
    }
    core::LogWarning(
        L"Contained a video playback failure without exiting the application.");
    WakePlaybackRenderThread();
    return true;
}

void WallpaperApplication::ToggleSound() {
    const std::scoped_lock playbackLock(playbackMutex_);
    soundEnabled_ = !soundEnabled_;
    bool failed = false;
    for (const auto& session : playbackSessions_) {
        if (session->videoPlayer &&
            FAILED(session->videoPlayer->SetSoundEnabled(soundEnabled_))) {
            failed = true;
        }
    }
    if (failed) {
        soundEnabled_ = !soundEnabled_;
        for (const auto& session : playbackSessions_) {
            if (session->videoPlayer) {
                session->videoPlayer->SetSoundEnabled(soundEnabled_);
            }
        }
        MessageBoxW(controlWindow_, L"无法更改当前视频的声音状态。",
                    kApplicationTitle, MB_OK | MB_ICONERROR);
        return;
    }
    mainWindow_.SetSoundEnabled(soundEnabled_);
    if (!controlledTestMode_ && !assignments_.empty()) {
        SaveCurrentSelection();
        mainWindow_.SetStatus(ActivePlaybackStatus());
    } else {
        mainWindow_.SetStatus(soundEnabled_ ? L"声音已开启 · 将应用到后续视频壁纸"
                                           : L"声音已关闭 · 视频默认静音");
    }
}

void WallpaperApplication::SetReleaseVideoResourcesOnPause(
    const bool enabled) {
    const std::scoped_lock playbackLock(playbackMutex_);
    if (releaseVideoResourcesOnPause_ == enabled) {
        return;
    }
    releaseVideoResourcesOnPause_ = enabled;
    if (!releaseVideoResourcesOnPause_ && dynamicPlaybackPaused_) {
        RestoreReleasedVideoResources(true);
    } else if (releaseVideoResourcesOnPause_ && dynamicPlaybackPaused_ &&
               CanDeepReleaseForCurrentPause() && systemSuspended_) {
        ReleasePausedVideoResources();
    }
    if (!controlledTestMode_) {
        SaveCurrentSelection();
    }
    core::LogInfo(releaseVideoResourcesOnPause_
                      ? L"Deep-pause resource release setting enabled."
                      : L"Deep-pause resource release setting disabled.");
    mainWindow_.SetStatus(
        releaseVideoResourcesOnPause_
            ? L"锁屏/熄屏释放视频资源已开启 · 恢复时可能短暂卡顿"
            : L"锁屏/熄屏释放视频资源已关闭 · 暂停时保留解码资源");
}

void WallpaperApplication::ShowSettings() {
    ShowWindow(updateButtonWindow_, SW_HIDE);
    ShowWindow(settingsButtonWindow_, SW_HIDE);
    const std::optional result = mainWindow_.ChoosePerformanceSettings(
        releaseVideoResourcesOnPause_);
    PositionUpdateButtonWindow();
    if (result.has_value()) {
        SetReleaseVideoResourcesOnPause(*result);
    }
}

void WallpaperApplication::ToggleManualPlaybackPause() {
    const std::scoped_lock playbackLock(playbackMutex_);
    if (!HasDynamicPlayback()) {
        return;
    }
    manualPlaybackPaused_ = !manualPlaybackPaused_;
    RefreshPlaybackPolicy();
}

HRESULT WallpaperApplication::SaveCurrentSelection() const {
    core::AppSettings settings;
    settings.assignments = assignments_;
    settings.soundEnabled = soundEnabled_;
    settings.releaseVideoResourcesOnPause = releaseVideoResourcesOnPause_;
    settings.displayTargets = JoinDisplayIds(selectedDisplayIds_);
    settings.spanAcrossDisplays = spanAcrossDisplays_;
    return settingsStore_.Save(settings);
}

void WallpaperApplication::RefreshDisplayTargets(const bool preserveSelection) {
    if (!IsWindow(desktopTarget_.parent)) {
        return;
    }
    displayTargets_ = shell::EnumerateDisplayTargets(desktopTarget_.parent);
    if (displayTargets_.empty()) {
        return;
    }

    if (!preserveSelection) {
        selectedDisplayIds_.clear();
    }
    std::erase_if(selectedDisplayIds_, [&](const std::wstring& identifier) {
        return std::ranges::none_of(displayTargets_, [&](const auto& display) {
            return _wcsicmp(display.deviceId.c_str(), identifier.c_str()) == 0;
        });
    });
    if (selectedDisplayIds_.empty()) {
        const auto primary = std::ranges::find_if(
            displayTargets_, [](const shell::DisplayTarget& display) {
                return display.primary;
            });
        selectedDisplayIds_.push_back(
            (primary != displayTargets_.end() ? *primary : displayTargets_.front())
                .deviceId);
    }

    std::vector<ModernMainWindow::DisplayOption> options;
    options.reserve(displayTargets_.size());
    for (std::size_t index = 0; index < displayTargets_.size(); ++index) {
        const auto& display = displayTargets_[index];
        const LONG width = display.clientBounds.right - display.clientBounds.left;
        const LONG height = display.clientBounds.bottom - display.clientBounds.top;
        ModernMainWindow::DisplayOption option;
        option.id = display.deviceId;
        option.label = L"屏幕 " + std::to_wstring(index + 1U) +
                       (display.primary ? L"（主屏）" : L"") + L" · " +
                       std::to_wstring(width) + L"×" + std::to_wstring(height);
        for (auto assignment = assignments_.rbegin();
             assignment != assignments_.rend(); ++assignment) {
            const bool targetsDisplay =
                assignment->spanAcrossDisplays ||
                ContainsDisplayId(SplitDisplayIds(assignment->displayTargets),
                                  display.deviceId);
            if (!targetsDisplay) {
                continue;
            }
            const std::filesystem::path wallpaperPath(assignment->wallpaperPath);
            option.activeWallpaperName = wallpaperPath.filename().native();
            if (option.activeWallpaperName.empty()) {
                option.activeWallpaperName = assignment->wallpaperPath;
            }
            break;
        }
        option.selected = std::ranges::any_of(
            selectedDisplayIds_, [&](const std::wstring& identifier) {
                return _wcsicmp(identifier.c_str(), display.deviceId.c_str()) == 0;
            });
        options.push_back(std::move(option));
    }
    mainWindow_.SetDisplayOptions(std::move(options), spanAcrossDisplays_);
}

void WallpaperApplication::ApplyDisplayModeFromUi() {
    spanAcrossDisplays_ = mainWindow_.SpanAcrossDisplays();
    if (FAILED(SaveCurrentSelection())) {
        mainWindow_.SetStatus(L"显示方式已选择，但本地设置保存失败");
        return;
    }
    mainWindow_.SetStatus(
        spanAcrossDisplays_
            ? L"显示方式：跨屏扩展 · 选择壁纸后应用到整个桌面"
            : L"显示方式：分屏显示 · 应用壁纸时选择一个或多个屏幕");
}

bool WallpaperApplication::ChooseApplicationTargets() {
    RefreshDisplayTargets(true);
    spanAcrossDisplays_ = mainWindow_.SpanAcrossDisplays();
    if (spanAcrossDisplays_) {
        return true;
    }
    if (displayTargets_.empty()) {
        MessageBoxW(controlWindow_, L"当前没有可用的显示器。", kApplicationTitle,
                    MB_OK | MB_ICONERROR);
        return false;
    }
    if (displayTargets_.size() == 1) {
        selectedDisplayIds_ = {displayTargets_.front().deviceId};
        return true;
    }
    const auto selected = mainWindow_.ChooseDisplayTargets();
    if (!selected.has_value()) {
        mainWindow_.SetStatus(L"已取消应用 · 当前壁纸保持不变");
        return false;
    }
    selectedDisplayIds_ = *selected;
    return !selectedDisplayIds_.empty();
}

std::vector<RECT> WallpaperApplication::DestinationsForAssignment(
    const core::WallpaperAssignmentSetting& assignment) const {
    std::vector<RECT> destinations;
    if (!IsWindow(wallpaperWindow_)) {
        return destinations;
    }
    RECT client{};
    if (!GetClientRect(wallpaperWindow_, &client)) {
        return destinations;
    }
    if (assignment.spanAcrossDisplays) {
        destinations.push_back(client);
        return destinations;
    }

    const std::vector<std::wstring> identifiers =
        SplitDisplayIds(assignment.displayTargets);
    for (const shell::DisplayTarget& display : displayTargets_) {
        const bool selected = ContainsDisplayId(identifiers, display.deviceId);
        RECT clipped{};
        if (selected && IntersectRect(&clipped, &client, &display.clientBounds) &&
            !IsRectEmpty(&clipped)) {
            destinations.push_back(clipped);
        }
    }
    return destinations;
}

bool WallpaperApplication::ConfigureWallpaperWindowRegion() {
    if (!IsWindow(wallpaperWindow_)) {
        return false;
    }
    std::vector<RECT> activeDestinations;
    for (const auto& assignment : assignments_) {
        std::vector<RECT> destinations = DestinationsForAssignment(assignment);
        activeDestinations.insert(activeDestinations.end(), destinations.begin(),
                                  destinations.end());
    }
    if (activeDestinations.empty()) {
        return false;
    }

    HRGN combined = CreateRectRgn(0, 0, 0, 0);
    if (combined == nullptr) {
        return false;
    }
    for (const RECT& destination : activeDestinations) {
        HRGN part = CreateRectRgn(destination.left, destination.top,
                                  destination.right, destination.bottom);
        if (part == nullptr || CombineRgn(combined, combined, part, RGN_OR) == ERROR) {
            if (part != nullptr) {
                DeleteObject(part);
            }
            DeleteObject(combined);
            return false;
        }
        DeleteObject(part);
    }
    // SetWindowRgn takes ownership only on success. The union contains every
    // assigned screen while unassigned displays reveal the Windows wallpaper.
    if (SetWindowRgn(wallpaperWindow_, combined, TRUE) == 0) {
        DeleteObject(combined);
        core::LogError(L"Unable to apply the selected display region.",
                       HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    return true;
}

std::vector<std::wstring> WallpaperApplication::ActiveWallpaperPaths() const {
    std::vector<std::wstring> paths;
    paths.reserve(assignments_.size());
    for (const auto& assignment : assignments_) {
        if (!std::ranges::any_of(paths, [&](const std::wstring& existing) {
                return SamePath(existing, assignment.wallpaperPath);
            })) {
            paths.push_back(assignment.wallpaperPath);
        }
    }
    return paths;
}

std::vector<ModernMainWindow::ActiveWallpaperInfo>
WallpaperApplication::ActiveWallpapers() const {
    std::vector<ModernMainWindow::ActiveWallpaperInfo> wallpapers;
    for (const std::wstring& path : ActiveWallpaperPaths()) {
        bool spansAllDisplays = false;
        std::vector<std::wstring> identifiers;
        for (const core::WallpaperAssignmentSetting& assignment : assignments_) {
            if (!SamePath(assignment.wallpaperPath, path)) {
                continue;
            }
            if (assignment.spanAcrossDisplays) {
                spansAllDisplays = true;
                break;
            }
            for (const std::wstring& identifier :
                 SplitDisplayIds(assignment.displayTargets)) {
                if (!ContainsDisplayId(identifiers, identifier)) {
                    identifiers.push_back(identifier);
                }
            }
        }

        ModernMainWindow::ActiveWallpaperInfo wallpaper;
        wallpaper.path = path;
        if (spansAllDisplays) {
            wallpaper.displayLabel = L"跨屏扩展（全部屏幕）";
            for (std::size_t index = 0; index < displayTargets_.size(); ++index) {
                const shell::DisplayTarget& display = displayTargets_[index];
                wallpaper.displayBadges.push_back(
                    {L"屏幕" + std::to_wstring(index + 1U), display.primary});
            }
        } else {
            for (std::size_t index = 0; index < displayTargets_.size(); ++index) {
                const shell::DisplayTarget& display = displayTargets_[index];
                if (!ContainsDisplayId(identifiers, display.deviceId)) {
                    continue;
                }
                if (!wallpaper.displayLabel.empty()) {
                    wallpaper.displayLabel += L"、";
                }
                wallpaper.displayLabel += L"屏幕 " + std::to_wstring(index + 1U);
                if (display.primary) {
                    wallpaper.displayLabel += L"（主屏）";
                }
                wallpaper.displayBadges.push_back(
                    {L"屏幕" + std::to_wstring(index + 1U), display.primary});
            }
            if (wallpaper.displayLabel.empty()) {
                wallpaper.displayLabel = L"未连接屏幕";
            }
        }
        wallpapers.push_back(std::move(wallpaper));
    }
    return wallpapers;
}

std::wstring WallpaperApplication::ActivePlaybackStatus() const {
    if (assignments_.empty()) {
        return L"当前未应用壁纸 · Windows 原壁纸已恢复显示";
    }
    std::vector<std::wstring> assignedDisplays;
    for (const auto& assignment : assignments_) {
        if (assignment.spanAcrossDisplays) {
            assignedDisplays.clear();
            for (const auto& display : displayTargets_) {
                assignedDisplays.push_back(display.deviceId);
            }
            break;
        }
        for (const std::wstring& identifier :
             SplitDisplayIds(assignment.displayTargets)) {
            if (!ContainsDisplayId(assignedDisplays, identifier)) {
                assignedDisplays.push_back(identifier);
            }
        }
    }
    if (assignments_.size() == 1) {
        const auto& assignment = assignments_.front();
        std::wstring status = L"正在使用 · ";
        status += media::WallpaperKindLabel(MediaKind(assignment.wallpaperKind));
        if (assignment.wallpaperKind == core::WallpaperSelectionKind::Video) {
            status += soundEnabled_ ? L" · 声音已开启" : L" · 默认静音";
        }
        status += L" · ";
        status += std::filesystem::path(assignment.wallpaperPath).filename().native();
        status += L" · ";
        status += assignment.spanAcrossDisplays
                      ? L"跨屏扩展"
                      : std::to_wstring(assignedDisplays.size()) + L" 个屏幕";
        return status;
    }
    return L"正在使用 · " + std::to_wstring(ActiveWallpaperPaths().size()) +
           L" 张壁纸 · " + std::to_wstring(assignedDisplays.size()) +
           L" 个屏幕";
}

bool WallpaperApplication::HasDynamicPlayback() const {
    const std::scoped_lock playbackLock(playbackMutex_);
    return std::ranges::any_of(playbackSessions_, [](const auto& session) {
        return session->gifPlayer || session->videoPlayer;
    });
}

std::uint64_t WallpaperApplication::VideoTransferredFrameCount() const {
    const std::scoped_lock playbackLock(playbackMutex_);
    std::uint64_t count = 0;
    for (const auto& session : playbackSessions_) {
        if (session->videoPlayer) {
            count += session->videoPlayer->TransferredFrameCount();
        }
    }
    return count;
}

bool WallpaperApplication::RenderPlaybackFrame() {
    const std::scoped_lock playbackLock(playbackMutex_);
    if (dynamicPlaybackPaused_ || playbackMode_ != PlaybackMode::Active) {
        return true;
    }
    if (std::ranges::any_of(playbackSessions_, [](const auto& session) {
            return session->videoPlayer && session->videoPlayer->HasFailed();
        })) {
        if (!playbackFailurePending_.exchange(true)) {
            PostMessageW(controlWindow_, kPlaybackFailureMessage, 0, 0);
        }
        return true;
    }

    const auto now = std::chrono::steady_clock::now();
    bool videoFrameComposed = false;
    for (const auto& session : playbackSessions_) {
        if (session->gifPlayer &&
            !session->gifPlayer->PresentDue(renderer_, now)) {
            if (runtimeExitCode_.exchange(1) == 0) {
                PostMessageW(controlWindow_, kPlaybackFatalMessage, 0, 0);
            }
            return false;
        }
        if (!session->videoPlayer || !session->videoPlayer->IsPlaying()) {
            continue;
        }
        const HRESULT frameResult = session->videoPlayer->PresentFrame(
            renderer_, session->destinations, false);
        if (FAILED(frameResult)) {
            if (!playbackFailurePending_.exchange(true)) {
                PostMessageW(controlWindow_, kPlaybackFailureMessage, 0, 0);
            }
            return true;
        }
        videoFrameComposed = videoFrameComposed || frameResult == S_OK;
    }

    if (videoFrameComposed && !renderer_.PresentCurrentFrame(true)) {
        if (runtimeExitCode_.exchange(1) == 0) {
            PostMessageW(controlWindow_, kPlaybackFatalMessage, 0, 0);
        }
        return false;
    }
    if (videoFrameComposed && pendingWallpaperReveal_) {
        pendingWallpaperReveal_ = false;
        PostMessageW(controlWindow_, kRevealWallpaperMessage, 0, 0);
    }
    return true;
}

void WallpaperApplication::StartPlaybackRenderThread() {
    if (playbackRenderThread_.joinable()) {
        return;
    }
    playbackRenderThread_ = std::jthread([this](const std::stop_token stopToken) {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        core::LogInfo(L"Playback render thread started; thread_id=" +
                      std::to_wstring(GetCurrentThreadId()) + L'.');
        while (!stopToken.stop_requested()) {
            DWORD waitMilliseconds = 250;
            {
                const std::scoped_lock playbackLock(playbackMutex_);
                if (playbackMode_ == PlaybackMode::Active &&
                    !dynamicPlaybackPaused_) {
                    RenderPlaybackFrame();
                    const auto now = std::chrono::steady_clock::now();
                    waitMilliseconds = INFINITE;
                    for (const auto& session : playbackSessions_) {
                        if (session->gifPlayer) {
                            waitMilliseconds = std::min(
                                waitMilliseconds,
                                session->gifPlayer->WaitMilliseconds(now));
                        }
                        if (session->videoPlayer) {
                            waitMilliseconds = std::min(waitMilliseconds, 8UL);
                        }
                    }
                    if (waitMilliseconds == INFINITE) {
                        waitMilliseconds = 250;
                    }
                }
            }

            std::unique_lock waitLock(playbackMutex_);
            playbackWake_.wait_for(
                waitLock,
                std::chrono::milliseconds(std::max(1UL, waitMilliseconds)));
        }
        core::LogInfo(L"Playback render thread stopped.");
        if (SUCCEEDED(comResult)) {
            CoUninitialize();
        }
    });
}

void WallpaperApplication::StopPlaybackRenderThread() {
    if (!playbackRenderThread_.joinable()) {
        return;
    }
    playbackRenderThread_.request_stop();
    playbackWake_.notify_all();
    playbackRenderThread_.join();
}

void WallpaperApplication::WakePlaybackRenderThread() {
    playbackWake_.notify_all();
}

WallpaperApplication::WallpaperSession* WallpaperApplication::FindSession(
    const std::uint32_t token) {
    const auto found = std::ranges::find_if(
        playbackSessions_, [&](const auto& session) {
            return session->token == token;
        });
    return found == playbackSessions_.end() ? nullptr : found->get();
}

void WallpaperApplication::UpdateResourceUsage() {
    const platform::ProcessResourceUsage usage = resourceMonitor_.Sample();
    mainWindow_.SetResourceUsage(FormatResourceUsage(usage));
}

void WallpaperApplication::InitializePlaybackPolicy() {
    if (!IsWindow(controlWindow_)) {
        return;
    }
    sessionNotificationsRegistered_ =
        WTSRegisterSessionNotification(controlWindow_, NOTIFY_FOR_THIS_SESSION) == TRUE;
    if (!sessionNotificationsRegistered_) {
        core::LogWarning(L"Session lock notifications are unavailable.");
    }
    displayPowerNotification_ = RegisterPowerSettingNotification(
        controlWindow_, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (displayPowerNotification_ == nullptr) {
        core::LogWarning(L"Display power notifications are unavailable.");
    }
    SetTimer(controlWindow_, kPlaybackPolicyTimer, 2000, nullptr);
    RefreshPlaybackPolicy();
}

std::wstring WallpaperApplication::PlaybackPauseReason() const {
    if (manualPlaybackPaused_) {
        return L"已从托盘手动暂停";
    }
    if (systemSuspended_) {
        return L"系统正在睡眠";
    }
    if (sessionLocked_) {
        return L"会话已锁定";
    }
    if (displayOff_) {
        return L"显示器已关闭";
    }
    if (fullScreenActive_) {
        return L"检测到全屏应用";
    }
    return {};
}

void WallpaperApplication::RefreshPlaybackPolicy() {
    QUERY_USER_NOTIFICATION_STATE state = QUNS_ACCEPTS_NOTIFICATIONS;
    const HRESULT notificationResult = SHQueryUserNotificationState(&state);
    fullScreenActive_ = SUCCEEDED(notificationResult) &&
                        (state == QUNS_RUNNING_D3D_FULL_SCREEN ||
                         state == QUNS_PRESENTATION_MODE);

    const std::scoped_lock playbackLock(playbackMutex_);

    const bool dynamic = HasDynamicPlayback();
    const std::wstring reason = PlaybackPauseReason();
    const bool shouldPause = dynamic && !reason.empty();
    if (shouldPause == dynamicPlaybackPaused_) {
        if (shouldPause && releaseVideoResourcesOnPause_ &&
            CanDeepReleaseForCurrentPause() &&
            dynamicPauseStartedAt_.has_value() &&
            (systemSuspended_ || std::chrono::steady_clock::now() -
                                     *dynamicPauseStartedAt_ >=
                                     kDeepPauseReleaseDelay)) {
            ReleasePausedVideoResources();
        }
        return;
    }

    if (!shouldPause && !RestoreReleasedVideoResources(false)) {
        mainWindow_.SetStatus(
            L"动态壁纸恢复失败 · 将自动重试视频解码器");
        return;
    }

    bool pauseFailed = false;
    for (const auto& session : playbackSessions_) {
        if (session->videoPlayer &&
            FAILED(session->videoPlayer->SetPaused(shouldPause))) {
            pauseFailed = true;
        }
    }
    if (pauseFailed) {
        core::LogWarning(L"Unable to change video playback for the pause policy.");
        return;
    }
    dynamicPlaybackPaused_ = shouldPause;
    if (shouldPause) {
        dynamicPauseStartedAt_ = std::chrono::steady_clock::now();
        mainWindow_.SetStatus(L"动态壁纸已暂停 · " + reason);
        core::LogInfo(L"Dynamic wallpaper paused: " + reason);
        if (releaseVideoResourcesOnPause_ && systemSuspended_) {
            ReleasePausedVideoResources();
        }
    } else {
        dynamicPauseStartedAt_.reset();
        mainWindow_.SetStatus(ActivePlaybackStatus());
        core::LogInfo(L"Dynamic wallpaper resumed after pause policy cleared.");
    }
    WakePlaybackRenderThread();
}

bool WallpaperApplication::CanDeepReleaseForCurrentPause() const noexcept {
    return systemSuspended_ || sessionLocked_ || displayOff_;
}

void WallpaperApplication::ReleasePausedVideoResources() {
    bool releasedAny = false;
    for (const auto& session : playbackSessions_) {
        if (!session->videoPlayer || session->videoResourcesReleased) {
            continue;
        }
        session->videoPlayer->Shutdown();
        session->videoPlayer.reset();
        session->videoResourcesReleased = true;
        releasedAny = true;
    }
    if (releasedAny) {
        core::LogInfo(
            L"Released paused video decoders and transfer surfaces to reduce "
            L"memory usage.");
        mainWindow_.SetStatus(
            L"动态壁纸深度暂停 · 视频解码资源已释放");
    }
}

bool WallpaperApplication::RestoreReleasedVideoResources(
    const bool remainPaused) {
    bool restoredAll = true;
    bool restoredAny = false;
    for (const auto& session : playbackSessions_) {
        if (!session->videoResourcesReleased) {
            continue;
        }
        const HRESULT result = StartWallpaperSession(*session);
        if (FAILED(result)) {
            session->videoPlayer.reset();
            session->videoResourcesReleased = true;
            restoredAll = false;
            core::LogError(L"Unable to recreate a released video decoder.",
                           result);
            continue;
        }
        if (remainPaused && session->videoPlayer) {
            session->videoPlayer->SetPaused(true);
        }
        restoredAny = true;
    }
    if (restoredAll && restoredAny) {
        core::LogInfo(L"Restored video decoders after deep pause.");
        WakePlaybackRenderThread();
    }
    return restoredAll;
}

void WallpaperApplication::ShowControlWindow() {
    if (controlWindow_ == nullptr || !IsWindow(controlWindow_)) {
        return;
    }
    ShowWindow(controlWindow_, IsIconic(controlWindow_) ? SW_RESTORE : SW_SHOW);
    PositionUpdateButtonWindow();
    SetForegroundWindow(controlWindow_);
    FLASHWINFO flash{sizeof(flash), controlWindow_, FLASHW_TRAY, 2, 0};
    FlashWindowEx(&flash);
}

void WallpaperApplication::ShowTrayMenu() {
    POINT cursor{};
    if (!GetCursorPos(&cursor)) {
        return;
    }
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    AppendMenuW(menu, MF_STRING | MF_DEFAULT, kTrayShowCommand, L"打开全部壁纸");
    AppendMenuW(menu, MF_STRING, kTrayImportCommand, L"导入壁纸...");
    AppendMenuW(menu, MF_STRING | (soundEnabled_ ? MF_CHECKED : 0),
                kTraySoundCommand, L"视频声音");
    AppendMenuW(menu,
                MF_STRING | (HasDynamicPlayback() ? 0 : MF_GRAYED) |
                    (manualPlaybackPaused_ ? MF_CHECKED : 0),
                kTrayPauseCommand,
                manualPlaybackPaused_ ? L"继续动态壁纸" : L"暂停动态壁纸");
    AppendMenuW(menu, MF_STRING | (assignments_.empty() ? MF_GRAYED : 0),
                kTrayCancelCommand, L"取消应用当前壁纸");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayExitCommand, L"退出");

    if (controlledTestMode_) {
        DestroyMenu(menu);
        core::LogInfo(
            L"CONTROLLED_TRAY_MENU=show,import,sound,pause,cancel,exit");
        return;
    }

    const bool controlWasVisible = IsWindowVisible(controlWindow_) != FALSE;
    SetForegroundWindow(controlWindow_);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY |
                                                 TPM_RIGHTBUTTON,
                                        cursor.x, cursor.y, 0, controlWindow_, nullptr);
    DestroyMenu(menu);
    PostMessageW(controlWindow_, WM_NULL, 0, 0);
    if (!controlWasVisible) {
        ShowWindow(controlWindow_, SW_HIDE);
    }

    if (command == kTrayShowCommand) {
        ShowControlWindow();
    } else if (command == kTrayImportCommand) {
        ShowControlWindow();
        ChooseImport();
    } else if (command == kTraySoundCommand) {
        ToggleSound();
    } else if (command == kTrayPauseCommand) {
        ToggleManualPlaybackPause();
    } else if (command == kTrayCancelCommand) {
        CancelActiveWallpaper();
    } else if (command == kTrayExitCommand) {
        RequestExit();
    }
}

RECT WallpaperApplication::UpdateButtonRectangle() const {
    RECT empty{};
    if (!IsWindow(controlWindow_) || IsIconic(controlWindow_)) {
        return empty;
    }

    RECT windowRectangle{};
    if (!GetWindowRect(controlWindow_, &windowRectangle)) {
        return empty;
    }
    POINT clientOrigin{};
    if (!ClientToScreen(controlWindow_, &clientOrigin)) {
        return empty;
    }
    const UINT dpi = GetDpiForWindow(controlWindow_);
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    HFONT captionFont = nullptr;
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                                   &metrics, 0, dpi)) {
        captionFont = CreateFontIndirectW(&metrics.lfCaptionFont);
    }

    int titleWidth = MulDiv(116, dpi, 96);
    const HDC context = GetWindowDC(controlWindow_);
    if (context != nullptr && captionFont != nullptr) {
        const HGDIOBJ previous = SelectObject(context, captionFont);
        SIZE titleSize{};
        if (GetTextExtentPoint32W(
                context, kApplicationTitle,
                static_cast<int>(std::size(kApplicationTitle)) - 1, &titleSize)) {
            titleWidth = titleSize.cx;
        }
        SelectObject(context, previous);
    }
    if (context != nullptr) {
        ReleaseDC(controlWindow_, context);
    }
    if (captionFont != nullptr) {
        DeleteObject(captionFont);
    }

    const int clientLeft = clientOrigin.x - windowRectangle.left;
    const int clientTop = clientOrigin.y - windowRectangle.top;
    const int iconWidth = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    const int left = clientLeft + MulDiv(7, dpi, 96) + iconWidth +
                     MulDiv(7, dpi, 96) + titleWidth + MulDiv(10, dpi, 96);
    const int top = std::max(MulDiv(3, dpi, 96), clientLeft / 2);
    const int bottom = std::max(top + 1, clientTop - MulDiv(4, dpi, 96));
    return RECT{left, top, left + MulDiv(84, dpi, 96), bottom};
}

RECT WallpaperApplication::SettingsButtonRectangle() const {
    const RECT update = UpdateButtonRectangle();
    if (IsRectEmpty(&update)) {
        return {};
    }
    const UINT dpi = GetDpiForWindow(controlWindow_);
    const int left = update.right + MulDiv(6, dpi, 96);
    return RECT{left, update.top, left + MulDiv(58, dpi, 96), update.bottom};
}

void WallpaperApplication::PositionUpdateButtonWindow() const {
    if (!IsWindow(controlWindow_) || !IsWindow(updateButtonWindow_) ||
        !IsWindow(settingsButtonWindow_)) {
        return;
    }
    if (!IsWindowVisible(controlWindow_) || IsIconic(controlWindow_)) {
        ShowWindow(updateButtonWindow_, SW_HIDE);
        ShowWindow(settingsButtonWindow_, SW_HIDE);
        return;
    }
    RECT windowRectangle{};
    if (!GetWindowRect(controlWindow_, &windowRectangle)) {
        return;
    }
    const RECT button = UpdateButtonRectangle();
    const RECT settingsButton = SettingsButtonRectangle();
    if (IsRectEmpty(&button) || IsRectEmpty(&settingsButton)) {
        return;
    }
    SetWindowPos(updateButtonWindow_, HWND_TOP,
                 windowRectangle.left + button.left,
                 windowRectangle.top + button.top, button.right - button.left,
                 button.bottom - button.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetWindowPos(settingsButtonWindow_, HWND_TOP,
                 windowRectangle.left + settingsButton.left,
                 windowRectangle.top + settingsButton.top,
                 settingsButton.right - settingsButton.left,
                 settingsButton.bottom - settingsButton.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void WallpaperApplication::RedrawUpdateButton() const {
    if (IsWindow(updateButtonWindow_)) {
        InvalidateRect(updateButtonWindow_, nullptr, FALSE);
        UpdateWindow(updateButtonWindow_);
    }
}

void WallpaperApplication::BeginUpdateCheck() {
    if (updateCheckInProgress_ || shuttingDown_ || !IsWindow(controlWindow_)) {
        return;
    }
    if (updateCheckThread_.joinable()) {
        updateCheckThread_.join();
    }
    {
        const std::scoped_lock lock(updateCheckMutex_);
        pendingUpdateResult_.reset();
    }
    updateCheckInProgress_ = true;
    SetWindowTextW(updateButtonWindow_, L"检查中…");
    RedrawUpdateButton();
    core::LogInfo(L"Manual update check started.");

    const HWND resultWindow = controlWindow_;
    updateCheckThread_ = std::jthread(
        [this, resultWindow](const std::stop_token stopToken) {
            updates::UpdateCheckResult result =
                updates::CheckForLatestRelease(stopToken, updateCheckMode_);
            if (stopToken.stop_requested()) {
                return;
            }
            {
                const std::scoped_lock lock(updateCheckMutex_);
                pendingUpdateResult_ = std::move(result);
            }
            PostMessageW(resultWindow, kUpdateCheckResultMessage, 0, 0);
        });
}

void WallpaperApplication::CompleteUpdateCheck() {
    std::optional<updates::UpdateCheckResult> result;
    {
        const std::scoped_lock lock(updateCheckMutex_);
        result = std::move(pendingUpdateResult_);
        pendingUpdateResult_.reset();
    }
    if (updateCheckThread_.joinable()) {
        updateCheckThread_.join();
    }
    updateCheckInProgress_ = false;
    SetWindowTextW(updateButtonWindow_, L"检查更新");
    RedrawUpdateButton();
    if (!result.has_value() || shuttingDown_) {
        return;
    }

    core::LogInfo(L"Manual update check completed: current=" +
                  result->currentTag + L", latest=" + result->latestTag + L'.');
    const auto showResult = [&](std::wstring heading, std::wstring message,
                                std::wstring detail,
                                std::wstring primaryLabel,
                                const bool showSecondary) {
        EnableWindow(updateButtonWindow_, FALSE);
        const bool accepted = ShowUpdateDialog(
            controlWindow_, instance_, std::move(heading), std::move(message),
            std::move(detail), std::move(primaryLabel), showSecondary);
        EnableWindow(updateButtonWindow_, TRUE);
        return accepted;
    };
    const auto openReleasePage = [&](const std::wstring& releaseUrl) {
        const HINSTANCE opened = ShellExecuteW(
            controlWindow_, L"open", releaseUrl.c_str(), nullptr, nullptr,
            SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(opened) <= 32) {
            showResult(L"无法打开浏览器", L"系统没有成功打开 GitHub Release 页面。",
                       releaseUrl, L"知道了", false);
        }
    };
    if (result->status == updates::UpdateStatus::UpdateAvailable) {
        const bool openRelease = showResult(
            L"发现新版本",
            L"当前版本 " + result->currentTag + L"，最新版本 " +
                result->latestTag + L"。",
            L"点击后将在浏览器中打开 GitHub Release 下载页。", L"前往下载",
            true);
        if (openRelease) {
            openReleasePage(result->releaseUrl);
        }
        return;
    }
    if (result->status == updates::UpdateStatus::UpToDate) {
        showResult(L"已是最新版本", L"当前没有需要安装的新版本。",
                   L"当前版本 " + result->currentTag, L"知道了", false);
        return;
    }
    core::LogWarning(L"Manual update check failed: " + result->errorSummary +
                     L" " + result->errorMessage);
    const bool openReleaseList = showResult(
        L"检查更新失败",
        result->errorSummary.empty() ? L"暂时无法获取最新版本信息。"
                                     : result->errorSummary,
        result->errorMessage, L"查看 Release", true);
    if (openReleaseList && !result->releaseUrl.empty()) {
        openReleasePage(result->releaseUrl);
    }
}

void WallpaperApplication::StopUpdateCheck() {
    if (updateCheckThread_.joinable()) {
        updateCheckThread_.request_stop();
        updateCheckThread_.join();
    }
    const std::scoped_lock lock(updateCheckMutex_);
    pendingUpdateResult_.reset();
    updateCheckInProgress_ = false;
    if (IsWindow(updateButtonWindow_)) {
        SetWindowTextW(updateButtonWindow_, L"检查更新");
    }
}

void WallpaperApplication::ResizeRendererToWindow() {
    const std::scoped_lock playbackLock(playbackMutex_);
    if (wallpaperWindow_ == nullptr) {
        return;
    }
    RECT client{};
    if (!GetClientRect(wallpaperWindow_, &client)) {
        return;
    }
    const UINT width = static_cast<UINT>(client.right);
    const UINT height = static_cast<UINT>(client.bottom);
    if (renderer_.IsInitialized() && !renderer_.Resize(width, height)) {
        return;
    }
    if (!assignments_.empty()) {
        ConfigureWallpaperWindowRegion();
    }
    for (const auto& session : playbackSessions_) {
        session->destinations = DestinationsForAssignment(session->assignment);
        if (session->kind == media::WallpaperKind::StaticImage) {
            RenderStaticImage(session->assignment.wallpaperPath,
                              session->destinations);
        } else if (session->gifPlayer) {
            session->gifPlayer->Resize(width, height);
            session->gifPlayer->SetTargetRects(session->destinations);
        }
    }
}

void WallpaperApplication::RequestExit() {
    running_ = false;
    if (controlWindow_ != nullptr) {
        PostMessageW(controlWindow_, WM_NULL, 0, 0);
    }
}

void WallpaperApplication::Shutdown() {
    if (shuttingDown_) {
        return;
    }
    shuttingDown_ = true;
    running_ = false;
    StopUpdateCheck();
    StopVideoOptimizationThread();
    if (controlWindow_ != nullptr) {
        KillTimer(controlWindow_, kPlaybackPolicyTimer);
        KillTimer(controlWindow_, kExplorerRecoveryTimer);
        KillTimer(controlWindow_, kResourceUsageTimer);
    }
    if (displayPowerNotification_ != nullptr) {
        UnregisterPowerSettingNotification(displayPowerNotification_);
        displayPowerNotification_ = nullptr;
    }
    if (sessionNotificationsRegistered_ && controlWindow_ != nullptr) {
        WTSUnRegisterSessionNotification(controlWindow_);
        sessionNotificationsRegistered_ = false;
    }
    RemoveTrayIcon();
    StopPlaybackRenderThread();
    StopAllPlayback();
    renderer_.Shutdown();

    if (wallpaperWindow_ != nullptr && IsWindow(wallpaperWindow_)) {
        ShowWindow(wallpaperWindow_, SW_HIDE);
        DestroyWindow(wallpaperWindow_);
    }
    wallpaperWindow_ = nullptr;
    if (desktopCompatibilityMutex_ != nullptr) {
        // The mutex must remain owned, not merely present. DeskGo waits on it
        // to detect shutdown; an unowned mutex is immediately signalled and
        // makes its static background repeatedly hide/show, causing flashing.
        if (desktopCompatibilityMutexOwned_) {
            ReleaseMutex(desktopCompatibilityMutex_);
        }
        CloseHandle(desktopCompatibilityMutex_);
        desktopCompatibilityMutex_ = nullptr;
        desktopCompatibilityMutexOwned_ = false;
    }
    if (controlWindow_ != nullptr && IsWindow(controlWindow_)) {
        DragAcceptFiles(controlWindow_, FALSE);
        if (IsWindow(updateButtonWindow_)) {
            DestroyWindow(updateButtonWindow_);
        }
        updateButtonWindow_ = nullptr;
        if (IsWindow(settingsButtonWindow_)) {
            DestroyWindow(settingsButtonWindow_);
        }
        settingsButtonWindow_ = nullptr;
        DestroyWindow(controlWindow_);
    }
    controlWindow_ = nullptr;

    if (mediaFoundationStarted_) {
        MFShutdown();
        mediaFoundationStarted_ = false;
    }
}

LRESULT CALLBACK WallpaperApplication::WindowProcedure(const HWND window,
                                                        const UINT message,
                                                        const WPARAM wParam,
                                                        const LPARAM lParam) {
    WallpaperApplication* application = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        application = static_cast<WallpaperApplication*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(application));
    } else {
        application = reinterpret_cast<WallpaperApplication*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    return application != nullptr
               ? application->HandleWindowMessage(window, message, wParam, lParam)
               : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT WallpaperApplication::HandleWindowMessage(const HWND window,
                                                   const UINT message,
                                                   const WPARAM wParam,
                                                   const LPARAM lParam) {
    if (window == controlWindow_ && taskbarCreatedMessage_ != 0 &&
        message == taskbarCreatedMessage_) {
        trayIconAdded_ = false;
        AddTrayIcon();
        SetTimer(controlWindow_, kExplorerRecoveryTimer, 1000, nullptr);
        return 0;
    }

    if (window == controlWindow_) {
        switch (message) {
            case WM_COMMAND: {
                const WORD identifier = LOWORD(wParam);
                const WORD notification = HIWORD(wParam);
                if (identifier == kTrayPauseCommand) {
                    ToggleManualPlaybackPause();
                    return 0;
                }
                if (controlledTestMode_ &&
                    identifier == kControlledTestExitCommand) {
                    RequestExit();
                    return 0;
                }
                if (controlledTestMode_ &&
                    identifier == kControlledDeepPauseToggleCommand) {
                    SetReleaseVideoResourcesOnPause(
                        !releaseVideoResourcesOnPause_);
                    return 0;
                }
                if (controlledTestMode_ &&
                    identifier == kControlledDeepPauseReleaseCommand) {
                    sessionLocked_ = true;
                    RefreshPlaybackPolicy();
                    dynamicPauseStartedAt_ = std::chrono::steady_clock::now() -
                                             kDeepPauseReleaseDelay;
                    RefreshPlaybackPolicy();
                    const std::scoped_lock playbackLock(playbackMutex_);
                    const bool released = std::ranges::any_of(
                        playbackSessions_, [](const auto& session) {
                            return session->videoResourcesReleased;
                        });
                    core::LogInfo(
                        std::wstring(L"CONTROLLED_DEEP_PAUSE_RELEASE=") +
                        (released ? L"True" : L"False"));
                    return 0;
                }
                if (controlledTestMode_ &&
                    identifier == kControlledLibraryDrawCountCommand) {
                    core::LogInfo(
                        L"CONTROLLED_LIBRARY_DRAW_COUNT=" +
                        std::to_wstring(mainWindow_.LibraryDrawCount()));
                    return 0;
                }
                if (controlledTestMode_ &&
                    identifier == kControlledFrameCountCommand) {
                    core::LogInfo(
                        L"CONTROLLED_VIDEO_TRANSFER_COUNT=" +
                        std::to_wstring(VideoTransferredFrameCount()));
                    return 0;
                }
                if (controlledTestMode_ &&
                    identifier == kControlledTestSaveCommand) {
                    const HRESULT result = SaveCurrentSelection();
                    if (SUCCEEDED(result)) {
                        core::LogInfo(L"CONTROLLED_SETTINGS_SAVE=True");
                    } else {
                        core::LogError(L"Controlled settings save failed.", result);
                    }
                    return 0;
                }
                if (mainWindow_.HandleFilterCommand(identifier, notification)) {
                    return 0;
                }
                if (identifier == ModernMainWindow::DisplayModeChanged &&
                    notification == BN_CLICKED) {
                    ApplyDisplayModeFromUi();
                    return 0;
                }
                if (identifier == ModernMainWindow::CancelSelectedWallpaper &&
                    notification == BN_CLICKED) {
                    const HWND source = reinterpret_cast<HWND>(lParam);
                    const std::optional selected =
                        source == mainWindow_.ActiveLibraryControl()
                            ? mainWindow_.SelectedActiveItem()
                            : mainWindow_.SelectedItem();
                    if (selected.has_value()) {
                        CancelWallpaper(*selected);
                    }
                    return 0;
                }
                if (identifier == ModernMainWindow::ExportSelectAll &&
                    notification == BN_CLICKED) {
                    mainWindow_.SelectAllVisibleForExport();
                    return 0;
                }
                if (identifier == ModernMainWindow::ExportClearAll &&
                    notification == BN_CLICKED) {
                    mainWindow_.ClearExportSelection();
                    return 0;
                }
                if (identifier == ModernMainWindow::ExportConfirm &&
                    notification == BN_CLICKED) {
                    ShowBatchActionsMenu();
                    return 0;
                }
                if (identifier == ModernMainWindow::ExportCancel &&
                    notification == BN_CLICKED) {
                    mainWindow_.EndExportSelection();
                    mainWindow_.SetStatus(L"已退出多选");
                    return 0;
                }
                if (identifier == ModernMainWindow::LibraryReordered &&
                    notification == BN_CLICKED) {
                    CommitLibraryOrder();
                    return 0;
                }
                if (identifier == ModernMainWindow::GroupReordered &&
                    notification == BN_CLICKED) {
                    CommitGroupOrder();
                    return 0;
                }
                if (identifier == ModernMainWindow::GroupCreate &&
                    notification == BN_CLICKED) {
                    CreateWallpaperGroup();
                    return 0;
                }
                if (identifier == ModernMainWindow::GroupRenameCommit &&
                    (notification == BN_CLICKED || notification == EN_KILLFOCUS)) {
                    RenameWallpaperGroup();
                    return 0;
                }
                mainWindow_.CloseTransientUi();
                if (identifier == ModernMainWindow::Import &&
                    notification == BN_CLICKED) {
                    ChooseImport();
                } else if (identifier == ModernMainWindow::Export &&
                           notification == BN_CLICKED) {
                    ChooseExport();
                } else if (identifier == ModernMainWindow::Library &&
                           notification == LBN_DBLCLK) {
                    if (!mainWindow_.ExportSelectionActive()) {
                        ApplySelectedWallpaper();
                    }
                } else if (identifier == ModernMainWindow::Sound &&
                           notification == BN_CLICKED) {
                    ToggleSound();
                } else if (identifier == ModernMainWindow::RenameCommit &&
                           notification == BN_CLICKED) {
                    CommitWallpaperRename();
                }
                return 0;
            }

            case WM_CONTEXTMENU:
                if (reinterpret_cast<HWND>(wParam) ==
                    mainWindow_.LibraryControl()) {
                    ShowLibraryContextMenu(
                        POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
                    return 0;
                }
                if (reinterpret_cast<HWND>(wParam) ==
                    mainWindow_.ActiveLibraryControl()) {
                    ShowActiveWallpaperContextMenu(
                        POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
                    return 0;
                }
                if (reinterpret_cast<HWND>(wParam) ==
                    mainWindow_.GroupListControl()) {
                    ShowGroupContextMenu(
                        POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
                    return 0;
                }
                break;

            case WM_DRAWITEM:
                if (mainWindow_.DrawItem(
                        *reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) {
                    return TRUE;
                }
                break;

            case WM_CTLCOLOREDIT:
            case WM_CTLCOLORLISTBOX:
            case WM_CTLCOLORSTATIC:
                return reinterpret_cast<LRESULT>(mainWindow_.ColorControl(
                    reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam)));

            case WM_PAINT: {
                PAINTSTRUCT paint{};
                const HDC context = BeginPaint(controlWindow_, &paint);
                RECT client{};
                GetClientRect(controlWindow_, &client);
                const int width = client.right - client.left;
                const int height = client.bottom - client.top;
                const HDC bufferContext = CreateCompatibleDC(context);
                const HBITMAP buffer =
                    width > 0 && height > 0
                        ? CreateCompatibleBitmap(context, width, height)
                        : nullptr;
                if (bufferContext != nullptr && buffer != nullptr) {
                    const HGDIOBJ previous = SelectObject(bufferContext, buffer);
                    mainWindow_.Paint(bufferContext, paint.rcPaint);
                    BitBlt(context, paint.rcPaint.left, paint.rcPaint.top,
                           paint.rcPaint.right - paint.rcPaint.left,
                           paint.rcPaint.bottom - paint.rcPaint.top, bufferContext,
                           paint.rcPaint.left, paint.rcPaint.top, SRCCOPY);
                    SelectObject(bufferContext, previous);
                } else {
                    mainWindow_.Paint(context, paint.rcPaint);
                }
                if (buffer != nullptr) {
                    DeleteObject(buffer);
                }
                if (bufferContext != nullptr) {
                    DeleteDC(bufferContext);
                }
                EndPaint(controlWindow_, &paint);
                return 0;
            }

            case WM_ERASEBKGND:
                return 1;

            case WM_CLOSE:
                if (trayIconAdded_) {
                    ShowWindow(controlWindow_, SW_HIDE);
                } else {
                    RequestExit();
                }
                return 0;

            case WM_SIZE:
                mainWindow_.Layout();
                PositionUpdateButtonWindow();
                return 0;

            case WM_MOVE:
            case WM_WINDOWPOSCHANGED:
                PositionUpdateButtonWindow();
                break;

            case WM_GETMINMAXINFO: {
                auto* minimum = reinterpret_cast<MINMAXINFO*>(lParam);
                const UINT dpi = GetDpiForWindow(controlWindow_);
                minimum->ptMinTrackSize.x = MulDiv(860, dpi, 96);
                minimum->ptMinTrackSize.y = MulDiv(590, dpi, 96);
                return 0;
            }

            case WM_DPICHANGED: {
                const auto* suggested = reinterpret_cast<const RECT*>(lParam);
                SetWindowPos(controlWindow_, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOACTIVATE | SWP_NOZORDER);
                mainWindow_.DpiChanged();
                PositionUpdateButtonWindow();
                return 0;
            }

            case WM_DROPFILES: {
                const HDROP drop = reinterpret_cast<HDROP>(wParam);
                const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                std::vector<std::wstring> paths;
                for (UINT index = 0; index < count; ++index) {
                    const UINT length = DragQueryFileW(drop, index, nullptr, 0);
                    std::wstring path(length + 1U, L'\0');
                    DragQueryFileW(drop, index, path.data(), length + 1U);
                    path.resize(length);
                    paths.push_back(std::move(path));
                }
                DragFinish(drop);
                ImportPaths(paths);
                return 0;
            }

            case WM_TIMER:
                if (wParam == kExplorerRecoveryTimer) {
                    KillTimer(controlWindow_, kExplorerRecoveryTimer);
                    ReattachToDesktop();
                    return 0;
                }
                if (wParam == kPlaybackPolicyTimer) {
                    RefreshPlaybackPolicy();
                    return 0;
                }
                if (wParam == kResourceUsageTimer) {
                    UpdateResourceUsage();
                    return 0;
                }
                if (wParam == ModernMainWindow::AnimationTimerId) {
                    mainWindow_.HandleAnimationTimer();
                    return 0;
                }
                break;

            case WM_WTSSESSION_CHANGE:
                if (wParam == WTS_SESSION_LOCK || wParam == WTS_SESSION_UNLOCK) {
                    sessionLocked_ = wParam == WTS_SESSION_LOCK;
                    RefreshPlaybackPolicy();
                }
                return 0;

            case WM_POWERBROADCAST:
                if (wParam == PBT_APMSUSPEND) {
                    systemSuspended_ = true;
                    RefreshPlaybackPolicy();
                } else if (wParam == PBT_APMRESUMEAUTOMATIC ||
                           wParam == PBT_APMRESUMESUSPEND) {
                    systemSuspended_ = false;
                    RefreshPlaybackPolicy();
                } else if (wParam == PBT_POWERSETTINGCHANGE && lParam != 0) {
                    const auto* setting =
                        reinterpret_cast<const POWERBROADCAST_SETTING*>(lParam);
                    if (IsEqualGUID(setting->PowerSetting,
                                    GUID_CONSOLE_DISPLAY_STATE) &&
                        setting->DataLength >= sizeof(DWORD)) {
                        const DWORD displayState =
                            *reinterpret_cast<const DWORD*>(setting->Data);
                        displayOff_ = displayState == 0;
                        RefreshPlaybackPolicy();
                    }
                }
                return TRUE;

            case WM_DISPLAYCHANGE:
                ReattachToDesktop();
                return 0;

            case kMediaEngineEventMessage:
                {
                    const std::scoped_lock playbackLock(playbackMutex_);
                    if (WallpaperSession* session = FindSession(
                            static_cast<std::uint32_t>(
                                static_cast<std::uint64_t>(wParam) >> 32U));
                        session != nullptr && session->videoPlayer) {
                        session->videoPlayer->HandleEvent(
                            static_cast<DWORD>(static_cast<std::uint64_t>(wParam) &
                                               0xffffffffULL),
                            static_cast<std::uint32_t>(lParam));
                        if (session->videoPlayer->IsPlaying()) {
                            mainWindow_.SetStatus(ActivePlaybackStatus());
                            WakePlaybackRenderThread();
                        }
                    }
                }
                return 0;

            case kPlaybackFailureMessage:
                playbackFailurePending_ = false;
                RemoveFailedPlaybackSessions();
                WakePlaybackRenderThread();
                return 0;

            case kPlaybackFatalMessage:
                runtimeExitCode_ = 1;
                RequestExit();
                return 0;

            case kRevealWallpaperMessage:
                if (IsWindow(wallpaperWindow_)) {
                    ShowWindow(wallpaperWindow_, SW_SHOWNOACTIVATE);
                }
                return 0;

            case kUpdateCheckResultMessage:
                CompleteUpdateCheck();
                return 0;

            case kVideoOptimizationResultMessage:
                CompleteVideoOptimizations();
                return 0;

            case kBeginUpdateCheckMessage:
                BeginUpdateCheck();
                return 0;

            case kShowSettingsMessage:
                ShowSettings();
                return 0;

            case kInstallerShutdownMessage:
                core::LogInfo(L"Installer requested application shutdown.");
                RequestExit();
                return 0;

            case kTrayCallbackMessage:
                if (LOWORD(lParam) == WM_LBUTTONUP) {
                    ShowControlWindow();
                } else if (LOWORD(lParam) == WM_RBUTTONUP ||
                           LOWORD(lParam) == WM_CONTEXTMENU) {
                    ShowTrayMenu();
                }
                return 0;

            case WM_DESTROY:
                controlWindow_ = nullptr;
                if (!shuttingDown_) {
                    running_ = false;
                }
                PostQuitMessage(0);
                return 0;

            default:
                break;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    if (window == wallpaperWindow_) {
        switch (message) {
            case WM_SIZE:
                if (wParam != SIZE_MINIMIZED) {
                    ResizeRendererToWindow();
                }
                return 0;
            case WM_NCHITTEST:
                return HTTRANSPARENT;
            case WM_MOUSEACTIVATE:
                return MA_NOACTIVATE;
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT: {
                PAINTSTRUCT paint{};
                BeginPaint(window, &paint);
                EndPaint(window, &paint);
                return 0;
            }
            case WM_DESTROY:
                wallpaperWindow_ = nullptr;
                StopAllPlayback();
                renderer_.Shutdown();
                if (!shuttingDown_ && controlWindow_ != nullptr) {
                    core::LogWarning(
                        L"Wallpaper window was destroyed; scheduling one recovery.");
                    SetTimer(controlWindow_, kExplorerRecoveryTimer, 1000, nullptr);
                }
                return 0;
            default:
                break;
        }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace lwe::app
