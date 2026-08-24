# Live Wallpaper Engine

面向 Windows 10/11 x64 的轻量级、本地动态壁纸软件。

> 当前为 `0.x` 预览阶段。程序完全本地运行，不需要账户、服务器或管理员权限。

## 当前验证内容

- C++20、Win32 和 Direct3D 11 原生实现。
- 识别 Windows 11 raised desktop 与传统 WorkerW 桌面结构。
- 壁纸窗口不进入 Alt+Tab、不获取焦点，并将鼠标输入交还桌面。
- 单实例运行；再次双击程序会唤醒已有的状态窗口，不会重复创建渲染器。
- 提供现代化“我的壁纸”界面，可搜索并按静态图片、GIF、视频筛选。
- 壁纸条目显示系统生成的图片/视频缩略图；右键可预览、重命名或应用。
- 支持按钮、文件选择器或拖放导入本地 JPG/JPEG、PNG、BMP、GIF 和受系统解码器支持的视频。
- GIF 由 WIC 按帧解码和合成；视频通过 Media Foundation/Media Engine 帧服务器解码到同一 D3D11 交换链并循环播放，优先使用系统硬件解码能力。
- 视频默认静音，可在主界面或托盘开启声音；选择会保存在当前用户的本地设置中。
- 动态内容在会话锁定、系统睡眠、显示器关闭和检测到符合规则的全屏应用时暂停。
- 本地壁纸可以导出为带 SHA-256 完整性校验的 `.lwewall` 分享包；其他用户可直接导入或拖入本软件。
- 静态图片经居中裁剪后只呈现一帧到桌面覆盖层，空闲时使用事件等待而非定时轮询。
- 显示位置使用下拉菜单，可跨屏扩展，也可勾选任意一个或多个显示器；同一壁纸会在每个所选显示器内独立居中填充。
- 当前壁纸可从状态栏或托盘菜单取消应用；覆盖窗口随即隐藏并显露原 Windows 壁纸。
- 状态栏实时显示本进程 CPU、GPU、内存和显存；GPU 与任务管理器进程页一样采用最繁忙 GPU 引擎，而不是把不同引擎相加。
- 不修改 Windows 原壁纸；退出程序会销毁覆盖窗口，原壁纸随即自然显露。
- 壁纸库位于 `%LOCALAPPDATA%\LiveWallpaperEngine\library`，设置保存在同一应用数据目录，重启后自动恢复。
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

安装包输出到 `dist\`，默认安装到当前用户的 `%LOCALAPPDATA%\Programs\Live Wallpaper Engine`，不请求管理员权限。`v0.x.y` 会发布为 GitHub 预发布版，`v1.0.0` 起才视为稳定正式版。

## 受控运行

以下命令呈现十秒测试画面后自动退出：

```powershell
.\out\x64\Release\LiveWallpaperEngine.exe --test-seconds=10
```

不传 `--test-seconds` 时程序持续运行。关闭状态窗口会隐藏到系统托盘；双击托盘图标可以重新打开，右键托盘图标可以退出。

在“我的壁纸”中点击“导入壁纸”，或把文件直接拖入窗口，即可加入本地库；选中后可应用或导出分享包。显示位置菜单支持跨屏扩展，以及在任意一个或多个显示器上分别填充同一壁纸。退出程序不会改动或重设 Windows 原壁纸。不同显示器同时使用不同壁纸和更多缩放模式仍待实现。

静态覆盖层回归测试可在编译后执行：

```powershell
.\tools\test-static-overlay.ps1 -Configuration Release
```

GIF/视频回归需要传入一个 GIF 和一个系统可解码的视频：

```powershell
.\tools\test-media.ps1 -Configuration Release -GifPath .\sample.gif -VideoPath .\sample.mp4
```

## 常见问题（FAQ）

### 使用腾讯桌面整理后，壁纸不显示或视频一直闪烁怎么办？

腾讯桌面整理默认可能会绘制一层系统静态壁纸，覆盖本软件的图片、GIF 或视频壁纸；视频持续呈现时，两层画面交替显示还可能表现为闪烁。

请打开腾讯桌面整理的“设置中心 → 桌面整理”，勾选“兼容第三方桌面壁纸”并应用，然后重新应用本软件中的壁纸。本软件不会自动修改第三方软件的设置。

### 为什么软件显示的 GPU 与任务管理器“性能”页不同？

软件底部显示的是本进程占用，适合与任务管理器“进程”页中 `Live Wallpaper Engine` 这一行比较；任务管理器“性能”页显示的是整块 GPU 的系统总占用，两者不是同一统计对象。不同采样时刻仍可能出现少量波动。

## 许可

本项目采用 [Apache License 2.0](LICENSE) 开源。

开发中的变更记录见 [CHANGELOG.md](CHANGELOG.md)。
