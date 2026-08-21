# Live Wallpaper Engine

面向 Windows 10/11 x64 的轻量级、本地动态壁纸软件。

> 当前处于桌面嵌入技术验证阶段。现有程序只在桌面图标后呈现一个缓慢变化的 Direct3D 测试画面，尚未接入图片、GIF、视频或正式设置界面。

## 当前验证内容

- C++20、Win32 和 Direct3D 11 原生实现。
- 识别 Windows 11 raised desktop 与传统 WorkerW 桌面结构。
- 壁纸窗口不进入 Alt+Tab、不获取焦点，并将鼠标输入交还桌面。
- Windows 11 layered desktop 使用兼容的 D3D11 BitBlt 交换链。
- 支持 `--test-seconds=N` 受控运行，便于自动退出和验证。
- 日志写入 `%LOCALAPPDATA%\LiveWallpaperEngine\logs\LiveWallpaperEngine.log`。

## 构建

要求：

- Visual Studio 2022，包含“使用 C++ 的桌面开发”。
- Windows 10/11 SDK 10.0.26100.0 或兼容版本。

在 Developer PowerShell 中执行：

```powershell
msbuild .\LiveWallpaperEngine.sln /m /p:Configuration=Release /p:Platform=x64
```

输出位于 `out\x64\Release\LiveWallpaperEngine.exe`。

## 安装包与发布

正式版本使用 `vMAJOR.MINOR.PATCH` 三段式 Git tag，例如 `v0.1.0`。推送合规 tag 后，GitHub Actions 会构建 x64 安装包、生成 SHA-256 校验文件，并创建同名 GitHub Release 上传资产。

本地构建安装包需要 Inno Setup 6：

```powershell
.\tools\build-release.ps1 -Version 0.1.0
```

安装包输出到 `dist\`，默认安装到当前用户的 `%LOCALAPPDATA%\Programs\Live Wallpaper Engine`，不请求管理员权限。未发布开发状态只写入 Changelog，不创建 tag。

## 受控运行

以下命令呈现十秒测试画面后自动退出：

```powershell
.\out\x64\Release\LiveWallpaperEngine.exe --test-seconds=10
```

不传 `--test-seconds` 时程序持续运行，可通过任务管理器结束。正式托盘退出功能将在后续阶段实现。

## 许可

本项目采用 [Apache License 2.0](LICENSE) 开源。

开发中的变更记录见 [CHANGELOG.md](CHANGELOG.md)。
