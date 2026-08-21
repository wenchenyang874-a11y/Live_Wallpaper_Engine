# Changelog

本文件记录 Live Wallpaper Engine 的重要变更。

项目采用 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 的结构，并计划遵循[语义化版本](https://semver.org/lang/zh-CN/)。在首次正式发布前，开发内容统一记录在 `Unreleased`。

## [Unreleased]

### Added

- 建立 C++20、Win32 和 Direct3D 11 原生项目骨架。
- 增加 Windows 11 raised-desktop、经典 WorkerW 和 Progman 回退路径的桌面宿主探测。
- 增加不抢焦点、不进入 Alt+Tab 且不拦截桌面输入的壁纸窗口。
- 增加 D3D11 测试渲染器，以及硬件设备失败后的 WARP 回退。
- 增加 `--test-seconds=N` 受控运行参数。
- 增加 `%LOCALAPPDATA%\LiveWallpaperEngine\logs` 本地运行日志。
- 增加 Visual Studio 2022 x64 Debug/Release 构建配置。
- 增加无需管理员权限的 Inno Setup x64 安装包配置。
- 增加三段式 tag 校验、安装包构建、SHA-256 生成和 GitHub Release 上传工作流。
- 增加由发布版本号生成的 Windows 可执行文件版本资源。
- 采用 Apache License 2.0 开源许可。

### Changed

- Release 和 Debug 均静态链接 MSVC CRT，降低目标机器的运行库依赖。

### Fixed

- 修复目标机器加载旧版 `MSVCP140.dll` 时，标准库文件系统调用可能触发启动崩溃的问题。
- 修复 `CreateWindowExW` 创建阶段尚未返回窗口句柄时，早期窗口消息可能使用空句柄的问题。

### Known limitations

- 当前只提供桌面嵌入和 D3D11 测试画面，尚未接入图片、GIF、视频及设置界面。
- Windows 10 经典桌面路径、Explorer 重启恢复和完整桌面图标交互仍待验证。
