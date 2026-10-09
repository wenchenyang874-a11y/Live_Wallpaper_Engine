#pragma once
#include <functional>
#include <stop_token>
#include <string>
#include <windows.h>

namespace lwe::app {
// Work runs off the UI thread. The nested UI loop keeps timers and wallpaper
// notifications alive; the owner is transactionally input-disabled, not frozen.
bool RunTaskWindow(HWND owner, const std::wstring& title,
                   const std::function<std::wstring(std::stop_token)>& work,
                   bool closeWhenDone = false);
void ShowDetailsWindow(HWND owner, const std::wstring& title,
                       const std::wstring& details);
}
