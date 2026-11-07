## 2026.10.29
本文件创建。

## 2026.10.29
- 新增：根据源码 SHA-256 的 CRC32 生成 8 位十六进制构建 ID，并新增 `version [self|DLL_NAME]` 命令。
- 新增：插件可通过 `sct_plugin_dll_ver` 上报 `dll_ver`，未导出时回退到 DLL 的 `FileVersion`。
- 撤销：移除提权 helper 会话，改为关闭并重启 SCT 为目标身份。
- 修复：提权后的窗口默认最小化或隐藏。
- 调整：启动横幅根据版本号和内容宽度自动调整。

## 2026.10.31
- 调整：统一终端字号，`version` 输出的版本号不再高亮显示。
- 修复：禁用选中终端输出后的拖动，避免文本被移动导致显示错乱。
- 新增：支持 `Ctrl+鼠标滚轮` 调整终端字号（6-32pt），仅对当前会话生效。
- 修复：缩放后不再重置已有文字颜色和加粗样式。
- 文档：更新 `ADD_COMMAND.md` 中的 GUI 终端维护说明。
- 新增：`echo` 支持前景色、背景色和 ANSI 样式。
- 新增：`cert` 支持生成自签名证书、由父 PFX 签发子证书、导出 PFX/DER，以及使用 PFX 进行 Authenticode 文件签名。
- 调整：`help <command>` 读取命令注册时提供的帮助文本。
- 调整：构建产物改名为 `SCT64.exe` 和 `SCT32.exe`，ZIP 文件名保持不变。
- 新增：为 `SCT64.exe` 和 `SCT32.exe` 嵌入对应架构的程序图标，并同步窗口图标。
- 调整：GUI 标题栏改为自绘 Windows Basic 风格，隐藏系统默认的最小化、最大化、关闭按钮并显示程序图标。
- 修复：`sudo ad` / `sudo sys` / `sudo su` / `sudo exit` 等待新窗口成功显示后再退出当前进程，避免后续窗口未打开。
- 修复：自绘标题栏改用窗口相对坐标并裁剪到标题栏区域，修复按钮点击错位、最大化/缩放时标题栏偏移和随机遮挡窗口的问题。
- 修复：自绘最小化/最大化/还原/关闭按钮改为直接处理鼠标点击，并在窗口状态变化后立即重绘按钮图标。
- 调整：GUI 右侧滚动条禁用视觉主题，改为 Windows Basic/经典样式。
- 修复：关闭按钮、`Alt+F4`、`WM_CLOSE` 和 `WM_SYSCOMMAND SC_CLOSE` 统一走程序关闭流程，正在运行的命令会被取消后再退出。
- 调整：`taskkill` 不加 `-f` 时优先发送 `WM_CLOSE`，无窗口时发送 `WM_QUIT`；`-f` 使用 `TerminateProcess`，不按 SCT 映像名或窗口类特殊处理。
- 调整：原 `ppl` 命令移动到 `protect ppl on/off`。
- 新增：`protect keep [on|1|off|0]` 为当前进程注册拒绝终止的 DACL；开启后关闭按钮和 Alt+F4 被阻止，任务管理器等外部关闭请求会重启受保护实例，`taskkill -f` 被拒绝，使用 `exit` 退出。
- 改进：`protect ppl on` 失败时显示驱动安装错误、易受攻击驱动黑名单状态，并在 32 位 SCT 运行于 64 位 Windows 时直接提示不支持。
- 新增：`start <program|script|document> [arguments...]`；程序和脚本优先用 `CreateProcessW` 直接启动，文档/URL 回退 `ShellExecuteExW`，保留原始参数并异步独立运行。
- 新增：README.md
- 新增：`build-release.ps1 -RandomTitle` 随机标题发布变体，每次启动生成 16 位字母数字标题，输出到项目根目录 `release/random/`，普通发布输出到 `release/normal/`，文件名仍为 `SCT64.exe` / `SCT32.exe`。
- Add: the random build copies itself to %LOCALAPPDATA%\SCT\runtime\<16-random>.exe (no SCT prefix), launches the child, and exits the parent; old copies are cleaned on the next launch.
- Fix: the random build loads plugins, autoexec.sct, NSudo, and PPL DLLs from the original release folder.
- Improve: with protect keep enabled, the caption close button and Alt+F4 are blocked; external close requests restart a protected instance instead of hanging, and taskkill -f is denied by the process DACL.
- Fix: the protected-close message no longer becomes console input; it refreshes and prints a new prompt.
- Improve: internal WM_TERMINAL_* messages require a per-process random token; external messages are ignored.

## 2026.11.01
- 文档：将 `ADD_COMMAND.md` 和 `CHANGELOG.md` 移动到项目根目录。
- 文档：新增 `DLL.md`，说明 DLL 插件的 ABI、导出函数、命令描述符、Host API、构建、启用和签名规范。

## 2026.11.06
- 新增：`protect front [on|1|off|0|1ms|2..2000]`，默认每 200ms 重新断言窗口 topmost，并在前台窗口变化时加速恢复；与 `protect keep` 独立。
- 调整：外部关闭触发的受保护实例重启不再向新实例注入 `protect keep`；keep 和 front 均不跨重启恢复，新实例只加载 `autoexec.sct`。
- 新增：进程退出前向插件顶层窗口广播关闭通知，定义 `SCT_PLUGIN_SHUTDOWN_MESSAGE_NAME`、`SCT_PLUGIN_SHUTDOWN_EXIT`、`SCT_PLUGIN_SHUTDOWN_RESTART`、`SCT_PLUGIN_SHUTDOWN_SYSTEM`。
- 文档：更新 `ADD_COMMAND.md`、`DLL.md`，补充 front、重启行为和插件关闭广播说明。
