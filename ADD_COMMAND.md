# 命令维护说明

本文说明 SuperCommandTerminal 的命令结构、新增命令方式、参数规则和维护约定。

## 1. 目录结构

```text
SuperCommandTerminal/
├── command.h             命令参数、上下文、注册表接口
├── command.cpp           参数解析、注册表、命令执行
├── cancel.h              Ctrl+C 与命令取消接口
├── cancel.cpp            控制台取消事件和命令运行状态
├── autoexec.h            启动脚本接口
├── autoexec.cpp          autoexec.sct 读取与执行
├── ppl.h                 PPL 动态加载接口
├── ppl.cpp               PPL DLL 加载与保护命令封装
├── ppl/
│   ├── PLL.MD            PPLcontrol 使用说明
│   ├── ppl64.dll         64 位 PPL 控制 DLL
│   └── ppl32.dll         32 位 PPL 控制 DLL
├── commands/             每个命令一个独立 .cpp
│   ├── help.cpp
│   ├── cd.cpp
│   ├── cls.cpp
│   ├── dir.cpp
│   ├── exit.cpp
│   ├── ppl.cpp
│   ├── ren.cpp
│   ├── rm.cpp
│   ├── shell.cpp
│   ├── taskkill.cpp
│   ├── whoami.cpp
│   └── sudo.cpp
├── global.h              集中包含系统头文件和项目头文件
├── shell_host.h          Shell 主循环接口
├── shell_host.cpp        提示符、输入处理和提升会话调度
├── tools/
│   ├── build-common.ps1  工具链解析、源文件收集和路径映射
│   ├── build-release.ps1 自动编译根目录和 commands/ 下的 .cpp
│   └── build-debug.ps1   生成 build/debug/main.exe 供 VS Code 调试
└── ADD_COMMAND.md        本说明
```

新增命令不需要修改 `command.cpp`、`app.cpp` 或任何中央注册函数。

## 2. 新增命令步骤

### 2.1 创建命令文件

在 `commands/` 下创建 `<命令名>.cpp`，例如 `commands/echo.cpp`。

### 2.2 编写处理函数

处理函数签名固定为：

```cpp
static int handle_echo(CommandContext& context);
```

返回值使用 `error_code.h` 中的错误码，成功返回 `ERR_NONE`。

```cpp
#include "global.h"

static int handle_echo(CommandContext& context) {
    bool no_newline = context.args.has("n");
    std::string text = context.args.positional(0).value_or("");

    context.out << text;
    if (!no_newline) context.out << '\n';
    return ERR_NONE;
}
```

### 2.3 定义参数规则

所有参数信息集中写在 `CommandSchema` 中：

```cpp
static CommandSchema make_echo_schema() {
    CommandSchema schema;
    schema.options.push_back(CommandOption{"n", OptionValueMode::Flag});
    schema.min_positionals = 0;
    schema.max_positionals = 1;
    return schema;
}
```

### 2.4 注册命令

在同一个文件末尾定义一个静态注册对象：

```cpp
static const CommandRegistration register_echo_command(
    "echo", "Print text.", make_echo_schema(), handle_echo, 0);
```

程序启动时，`CommandRegistration` 会自动把命令加入全局注册表。命令会出现在 `help` 列表中，不需要修改中央代码。

第五个参数 `requireElevation` 可选，省略时为 `0`。

### 2.5 命令权限要求

`CommandRegistration` 的第五个参数是 `requireElevation`，取值：

| 值 | 含义 |
| --- | --- |
| 0 | 无要求，默认值 |
| 1 | User |
| 2 | Administrator |
| 3 | System |
| 4 | TrustedInstaller |

权限顺序为 `User < Administrator < System < TrustedInstaller`。

```cpp
static const CommandRegistration register_admin_command(
    "admincmd", "Administrator-only command.", make_admin_schema(),
    handle_admin_command, 2);
```

`execute_command_line` 会在执行命令前检查当前进程权限。权限不足时输出所需权限和当前权限，返回 `ERR_PERMISSION_DENIED`，并提示使用对应的 `sudo` 模式。提升会话中的检查在 helper 进程中执行，因此以会话目标权限为准。

### 2.6 构建和测试

```powershell
.\tools\build-release.ps1 -Arch x64 -Zip
```

快速测试：

```powershell
@('echo hello', 'echo -n hello', 'echo --n hello', 'exit') |
    & .\release\main.exe
```

## 3. 参数规则

### 3.1 选项写法

以下写法等价：

```text
-n
--n
/n
```

选项名不区分大小写，注册时建议使用小写，且不要带 `-`、`--` 或 `/`。

支持内联值：

```text
-o=value
-o value
--output=value
/output value
```

### 3.2 选项值模式

`CommandOption` 的 `value_mode` 有三种：

```cpp
CommandOption{"n", OptionValueMode::Flag};
CommandOption{"o", OptionValueMode::Required};
CommandOption{"o", OptionValueMode::Optional};
```

| 模式 | 含义 | 示例 |
| --- | --- | --- |
| `Flag` | 只能作为开关，不能带值 | `-n` |
| `Required` | 必须带值 | `-o file` 或 `-o=file` |
| `Optional` | 可以不带值，也可以带值 | `-o` 或 `-o file` |

`Flag` 如果写成 `-n=value`，会返回“参数过多”错误。

`Required` 如果缺少值，会返回“参数过少”错误。

### 3.3 位置参数

不属于任何选项的值称为位置参数：

```text
echo hello
```

其中 `hello` 是位置参数 0。

```cpp
std::optional<std::string> first = context.args.positional(0);
const std::vector<std::string>& all = context.args.positionals();
size_t count = context.args.positional_count();
```

`min_positionals` 和 `max_positionals` 控制位置参数数量。两者默认都是 `0`，因此命令默认不接受任何位置参数。

```cpp
schema.min_positionals = 1;
schema.max_positionals = 2;
```

### 3.4 选项终止符

`--` 表示选项解析结束，后面的内容全部作为位置参数：

```text
command -- -n
```

这里的 `-n` 是位置参数，不再当作选项。

### 3.5 负数和特殊值

当 `Required` 或 `Optional` 选项需要读取下一个参数时，会消费下一个 token，除非它是：

- `--`
- 当前 schema 中已声明的选项

因此负数可以作为选项值：

```text
command -o -1
```

这里的 `-1` 会作为 `-o` 的值，而不是未知选项。

### 3.6 重复选项

同一个选项出现多次会返回 `ERR_DUPLICATE_OPTION`：

```text
command -o a -o b
```

### 3.7 未知选项

未在 `CommandSchema.options` 中声明的选项会返回 `ERR_INVALID_OPTION`。

## 4. 读取参数

处理函数通过 `context.args` 读取参数：

```cpp
bool has_n = context.args.has("n");
std::optional<std::string> output = context.args.value("o");
const std::vector<ParsedOption>& options = context.args.options();
```

- `has("n")`：选项是否出现
- `value("o")`：选项值，可能为空
- `options()`：所有已解析选项
- `positional(index)`：指定位置参数
- `positionals()`：全部位置参数
- `positional_count()`：位置参数数量

`Flag` 和未带值的 `Optional` 选项，其 `value()` 返回空 `optional`。

## 5. 上下文

`CommandContext` 提供：

```cpp
std::ostream& out;              // 输出流，不要直接使用 std::cout
bool exit_requested = false;    // 是否请求退出主循环
std::string raw_line;           // 当前完整命令行
CommandArguments args;          // 解析后的参数
```

输出统一使用：

```cpp
print(context.out, "text\n", CLR_White);
```

需要退出 shell 时设置：

```cpp
context.exit_requested = true;
```

## 6. raw_tail 模式

`raw_tail = true` 适用于需要把后续内容原样传递给其他命令的命令，例如 `sudo`。

```cpp
static CommandSchema make_sudo_schema() {
    CommandSchema schema;
    schema.max_positionals = 1;
    schema.raw_tail = true;
    return schema;
}
```

行为：

- 只把第一个 token 作为位置参数
- 后续 token 不参与选项解析，也不做位置参数数量检查
- 后续内容可通过 `context.raw_line` 获取

`sudo` 使用：

```cpp
std::string command_line = rest_after_first_token(context.raw_line);
```

## 7. 现有命令

| 命令 | 文件 | 参数规则 | requireElevation |
| --- | --- | --- | --- |
| `help` | `commands/help.cpp` | 无参数 | 0 |
| `cd` | `commands/cd.cpp` | `-d` 开关，最多 1 个位置参数 | 0 |
| `cls` | `commands/cls.cpp` | 无参数，清除当前显示 | 0 |
| `dir` / `ls` | `commands/dir.cpp` | `-a`、`-d`、`-t`，可接收多个路径 | 0 |
| `exit` | `commands/exit.cpp` | 无参数 | 0 |
| `ren` | `commands/ren.cpp` | 2 个位置参数 | 0 |
| `rm` | `commands/rm.cpp` | `-r`、`-f`、`-rf`、`--no-preserve-root`，可接收多个路径 | 0 |
| `ppl` | `commands/ppl.cpp` | `on [TYPE]` / `off` | 2 |
| `shell` | `commands/shell.cpp` | `-hide`/`-h`、`-c`/`-p`，后接原始命令 | 0 |
| `taskkill` | `commands/taskkill.cpp` | `-f`、`-t`、`-im`、`-pid` | 0 |
| `whoami` | `commands/whoami.cpp` | 无参数 | 0 |
| `sudo` | `commands/sudo.cpp` | `raw_tail`，最多 1 个位置参数 | 0 |

`sudo` 支持：

```text
sudo ad
sudo sys
sudo su
sudo exit
sudo <command>
```

`cls` 清除当前控制台显示，可在本地和提升会话中使用。

### `ren`

```text
ren <原始文件名> <重命名后文件名>
```

- 只接受两个位置参数。
- 使用 `MoveFileExW` 重命名文件或目录。
- 目标已存在时不会覆盖。

### `rm`

```text
rm [-r] [-f] [-rf] [--no-preserve-root] <路径...>
```

| 选项 | 含义 |
| --- | --- |
| `-r` / `--recursive` | 递归删除目录及其内容 |
| `-f` / `--force` | 忽略不存在的目标，并清除只读属性后删除 |
| `-rf` / `-fr` | 同时启用递归和强制模式 |
| `--no-preserve-root` | 允许删除磁盘根目录或卷根目录 |

行为说明：

- 默认只删除文件；目录需要 `-r`。
- 支持一次删除多个路径。
- 每成功删除一个文件或目录，输出 `removed '<路径>'`。
- 删除目录时不会跟随符号链接或重解析点。
- 删除根目录（例如 `C:\`、`D:\`）默认会被拒绝，并输出：

```text
Dangerous operation. Make sure you know what you are doing. To continue, enter: rm -rf "C:\" --no-preserve-root
```

- 只有显式提供 `--no-preserve-root` 才会继续删除根目录。该操作极其危险。
- 根目录本身不会被删除，只会删除其中的内容，避免 Windows 拒绝删除卷根目录而产生误报。

### `shell`

```text
shell [-hide|-h] [-c|-p] <command>
```

| 选项 | 含义 |
| --- | --- |
| `-hide` / `-h` | 隐藏子进程窗口 |
| `-c` | 使用 `cmd.exe` 执行 |
| `-p` | 使用 PowerShell 执行，默认值 |

行为说明：

- `-c` / `-p` / `-h` 必须位于 Command 之前。
- Command 及其参数保持原样传递。
- 如果 Command 本身以 `-` 或 `/` 开头，可先写 `--` 结束选项解析。
- 子进程继承 SCT 当前进程的令牌，因此不会触发 UAC；若 SCT 处于提升会话，则提升 helper 中执行。
- stdout 和 stderr 会实时回显到 SCT。
- PowerShell 输出使用 UTF-8；cmd 输出会从系统 OEM 代码页转换为 UTF-8。
- 未指定 `-c` / `-p` 时默认使用 PowerShell。

示例：

```text
shell -c echo hello
shell -p Write-Output "hello"
shell -h -p Get-ChildItem C:\Windows | Select-Object -First 5
shell -- -CommandThatStartsWithDash
```

### `ppl`

```text
ppl on [TYPE]
ppl off
```

- `TYPE` 可选值为 `Authenticode`、`CodeGen`、`Antimalware`、`Lsa`、`Windows`、`WinTcb`、`WinSystem`。
- 未指定 `TYPE` 时使用 `SCT_PPL_TYPE`，默认 `WinTcb`。
- `ppl on` 会显式加载并启动 PPL 驱动，然后设置进程保护；保护失败时会尝试卸载驱动。
- `ppl off` 会先移除进程保护，成功后再停止并卸载 PPL 驱动，避免驱动长期驻留被利用。
- 如果 `off` 的 `unprotect` 失败，会保留驱动以便重试，不会进入无法恢复的保护状态。
- `ppl` 命令要求 Administrator 权限，PPL 不会在启动时自动启用。
- `ppl on/off` 必须在已提升的主 SCT 进程中执行，不能通过 `sudo` 会话执行；请直接以管理员或更高权限启动 SCT。

### `taskkill`

```text
taskkill [-f] [-t] (-im <imagename> | -pid <processid>)
```

| 选项 | 含义 |
| --- | --- |
| `-f` / `--force` | 强制终止进程 |
| `-t` / `--tree` | 同时终止目标进程的子进程 |
| `-im <imagename>` | 按映像名称终止，支持 `*` 和 `?` 通配符 |
| `-pid <processid>` | 按 PID 终止 |

行为说明：

- `-im` 和 `-pid` 不能同时使用。
- 不加 `-f` 时发送终止信号：向目标窗口发送 `WM_CLOSE`，并向目标线程发送 `WM_QUIT`。
- 加 `-f` 时使用 `TerminateProcess` 强制终止。
- 使用 SCT 当前进程令牌执行，不提升权限，也不会弹出 UAC。
- 不调用 `cmd.exe` 或其他 shell，直接使用 Win32 进程和线程 API。
- 使用当前用户权限无法访问的进程会返回权限错误。
- 控制台程序可能忽略终止信号，此时需要使用 `-f`。

示例：

```text
taskkill -pid 1234
taskkill -f -pid 1234
taskkill -t -im notepad.exe
taskkill -f -t -im note*.exe
```

未实现 `/S`、`/U`、`/P`、`/FI`。

### `dir` / `ls`

```text
dir [选项] [路径或通配符...]
ls  [选项] [路径或通配符...]
```

| 选项 | 含义 |
| --- | --- |
| `-a` / `--all` | 显示隐藏文件和系统文件 |
| `-d` / `--detail` / `-detail` | Windows 风格详细信息，一个条目一行，全部白色 |
| `-t` / `--tree` / `-tree` | 递归打印目录树 |

默认输出为 Linux `ls` 风格：名称按列排列，按文件类型使用不同颜色。

`-d` 输出格式：

```text
2026-10-25  14:03    <DIR>         folder
2026-10-25  14:03               3  file.txt
```

行为说明：

- 未指定路径时使用当前目录。
- 支持多个路径或通配符，例如 `dir src include`、`ls *.cpp A*.h`。
- `dir file.txt` 列出该文件。
- `dir *.txt` 列出匹配文件。
- 默认隐藏隐藏文件和系统文件；`-a` 显示它们。
- 非树状模式下，`-a` 还会显示 `.` 和 `..`；树状模式不会递归 `.` 和 `..`。
- `-t` 和 `-d` 可以同时使用，树状输出中每个条目带详细信息。
- 树状模式不会递归进入符号链接或重解析点，避免循环。
- 树状通配符只支持文件名部分包含通配符；目录名中包含通配符时返回 `ERR_INVALID_OPTION`。
- 多个目标时，非树状输出会为目录和通配符目标打印标题，普通文件目标直接连续列出；树状输出会为每个目标打印根路径。
- 目录读取使用 Win32 `FindFirstFileW` / `FindNextFileW`，不依赖 `cmd` 或其他 shell。

## 8. 修改和删除命令

修改命令时只编辑对应的 `commands/<命令名>.cpp`。

删除命令时删除对应 `.cpp` 文件并重新构建。`help` 列表会自动更新。

命令名必须唯一。不同文件注册同名命令会导致不确定的覆盖行为，应避免。

## 9. 错误码

常用错误码定义在 `error_code.h`：

| 错误码 | 值 | 含义 |
| --- | --- | --- |
| `ERR_NONE` | `0x0000` | 成功 |
| `ERR_UNKNOWN` | `0x0001` | 未知错误 |
| `ERR_INVALID_COMMAND` | `0x0003` | 命令不存在 |
| `ERR_PERMISSION_DENIED` | `0x0004` | 权限不足 |
| `ERR_FILE_NOT_FOUND` | `0x0005` | 文件或路径不存在 |
| `ERR_TOO_MANY_ARGUMENTS` | `0x0008` | 参数过多 |
| `ERR_TOO_FEW_ARGUMENTS` | `0x000A` | 参数过少 |
| `ERR_INVALID_OPTION` | `0x000B` | 未知或非法选项 |
| `ERR_DUPLICATE_OPTION` | `0x000C` | 选项重复 |
| `ERR_CANCELLED` | `0x000D` | 命令被 Ctrl+C 取消 |
| `ERR_DANGEROUS_OPERATION` | `0x000E` | 危险操作被拒绝 |
| `ERR_IS_A_DIRECTORY` | `0x000F` | 目标目录需要递归删除 |

参数解析错误由 `execute_command_line` 自动输出，处理函数通常不需要重复打印。

## 10. 代码约定

- 每个 `.cpp` 只包含 `#include "global.h"`。
- 新命令放在 `commands/` 下，一个命令一个文件。
- 新命令使用静态 `CommandRegistration` 对象自动注册。
- 需要提升权限的命令把第五个参数设为 `2`、`3` 或 `4`；普通命令保持默认 `0`。
- 不要修改 `register_builtin_commands`，该中央注册函数已移除。
- 不要直接使用 `std::cout`，使用 `context.out`。
- 命令名和处理函数名使用小写或下划线风格。
- 新增项目头文件时，按当前集中包含约定加入 `global.h`。
- 新命令文件不需要加入构建脚本，`tools/build-release.ps1` 会自动收集 `commands/*.cpp`。

## 11. 构建说明

x64：

```powershell
.\tools\build-release.ps1 -Arch x64 -Zip
```

x86：

```powershell
.\tools\build-release.ps1 -Arch x86 -Zip
```

构建脚本不再硬编码 MSYS2 安装路径，按以下顺序解析编译器：

| 环境变量 | 用途 |
| --- | --- |
| `SCT_X64_COMPILER` | x64 的 `g++.exe` 路径 |
| `SCT_X86_COMPILER` | x86 的 `g++.exe` 路径 |

未设置时从 `PATH` 查找：

- x64：`x86_64-w64-mingw32-g++.exe`，然后 `g++.exe`
- x86：`i686-w64-mingw32-g++.exe`

如果找不到编译器，构建脚本会提示需要设置的环境变量或加入 `PATH` 的命令名。项目在 `global.h` 中统一设置 Windows 10 API 基线（`_WIN32_WINNT` / `WINVER` = `0x0A00`），确保 x86/x64 头文件暴露一致的 API。

构建脚本会自动：

- 编译根目录 `*.cpp`
- 编译 `commands/*.cpp`
- 添加项目根目录为 include 路径
- 将项目根路径映射为 `.`，避免调试信息写入项目绝对路径
- 将编译器所在目录临时置于 `PATH` 前面，避免加载错误架构的 MSYS2 运行库
- 复制 NSudo 运行时
- 生成 `release/SCT_<arch>.zip`；`-Arch all -Zip` 还会同时生成 `SCT_x64.zip`、`SCT_x86.zip` 和 `SCT_all.zip`

### VS Code 调试

在 VS Code 中选择 `Debug SCT x64` 并按 F5。`.vscode/launch.json` 会先执行 `C/C++: Build SCT x64 debug`，通过 `tools/build-debug.ps1` 编译所有源文件：

```text
build/debug/main.exe
```

`launch.json` 使用 `PATH` 中的 `gdb.exe`。请把 MSYS2 UCRT64 的 `bin` 目录加入 `PATH`，或自行在 `launch.json` 中配置 `miDebuggerPath`。

调试构建会带 `-g3 -O0` 并复制 NSudo 运行时，不会覆盖 release 输出。不要使用“生成活动文件”任务调试 `main.cpp`，因为它只编译单个文件，无法链接其他 `.cpp`。

文档中的 `C:\`、`C:\Windows` 等是系统路径示例，不是项目路径。

## 12. Ctrl+C 与命令取消

按下 Ctrl+C 时：

- 如果当前有命令正在执行，会请求取消当前命令，并阻止 Ctrl+C 关闭 SuperCommandTerminal。
- 如果当前在输入提示符，会取消当前输入行并重新显示提示符。
- 提升会话中的命令通过同一个取消事件通知 helper，因此 `sudo` 会话中的命令也可以取消。
- 进程同时忽略 CRT 的 `SIGINT` / `SIGBREAK` 默认处理，避免绕过控制台处理器导致主程序退出。

命令终止由 `command.cpp` 统一处理，不需要每个命令单独实现：

- 每个顶层命令在独立 worker 线程中执行。
- 命令输出通过匿名管道实时转发到主输出流，避免 worker 直接持有主输出流的锁。
- Ctrl+C 设置取消事件；worker 输出流检测到事件后会抛出内部 `CommandAborted`，让命令正常展开清理。
- 如果命令在 250ms 内没有退出，底层会强制终止 worker，确保所有命令都能被停止。
- 终止完成后，主线程恢复命令状态并重置取消事件，返回 `ERR_CANCELLED`。

新增命令时：

- 必须使用 `context.out` 输出，不要直接使用 `std::cout`。
- 使用 RAII 管理文件、句柄等资源，便于正常取消时清理。
- 不要把关键持久化工作只放在析构函数中，因为强制终止不会执行析构函数。
- 不需要在命令内部重复检查取消状态，也不需要打印 `Command cancelled.`；底层统一处理并输出。

## 13. PPL 手动启用

项目通过 `ppl.h` / `ppl.cpp` 动态加载 `ppl64.dll` 或 `ppl32.dll`，并调用：

```text
PPLcontrolRunCommandLineW
```

PPL 不会自动启用，需要手动执行：

```text
ppl on [TYPE]
ppl off
```

- `ppl on` 默认使用 `SCT_PPL_TYPE`，未设置时为 `WinTcb`。
- `ppl` 命令要求 Administrator 权限，并且必须在已提升的主 SCT 进程中执行。
- 不要通过 `sudo ad` / `sudo sys` / `sudo su` 会话执行 `ppl on/off`；请直接以管理员或更高权限启动 SCT。
- 普通发布包会在 `release\` 根目录包含 `ppl64.dll` 和 `ppl32.dll`，并打入 `SCT_x64.zip` / `SCT_x86.zip`；`-Arch all -Zip` 还会生成包含两者的 `SCT_all.zip`。
- PPL 操作在独立的短生命周期 `--sct-ppl-helper` 子进程中执行，避免 PPL DLL 或驱动异常影响主 SCT。
- `install`、`protect`、`unprotect`、`uninstall` 分别使用独立的 helper 进程；`unprotect` helper 退出后再启动新的 `uninstall` helper，确保驱动设备句柄已经释放。
- `uninstall` 直接通过 SCM 停止 `RTCore64` / `RTCore32` 服务，等待进入 `STOPPED` 状态后再删除服务，避免驱动句柄未释放导致 `STOP_PENDING`。
- 如果驱动长时间停留在 `STOP_PENDING`，会标记删除并提示需要重启。
- `ppl on` 调用 `PPLcontrolInstallDriver` 加载并启动驱动，然后执行 `protect`；如果保护失败，会尝试卸载驱动。
- 如果驱动已经加载导致安装接口返回失败，但 `protect` 成功，`ppl on` 仍视为成功。
- `ppl off` 调用 `unprotect`，成功后通过 SCM 停止并删除驱动服务。
- 如果 `unprotect` 失败，`ppl off` 不会卸载驱动，以便保留恢复手段。

环境变量：

| 变量 | 含义 |
| --- | --- |
| `SCT_PPL_TYPE` | `ppl on` 的默认保护类型，默认 `WinTcb` |

注意：

- PPL 操作需要管理员权限，并会加载 PPLcontrol 附带的内核驱动。
- 如果 `ppl off` 的 `unprotect` 失败，会保留驱动并返回失败信息，以便重试。
- 在启用 DSE、Secure Boot 或易受攻击驱动黑名单的系统上，驱动可能无法加载。
- 建议在测试虚拟机中验证。
- PPL 保护会限制普通管理员终止或调试 SCT；必要时使用 `ppl off`。

## 14. autoexec.sct 启动脚本

启动时会在以下位置查找 `autoexec.sct`：

1. SCT 可执行文件所在目录
2. 当前工作目录

找到后会在显示欢迎信息之后、进入交互提示符之前，按行执行其中的 SCT 命令。

- 每行一个命令。
- 空行会被忽略。
- 支持 UTF-8 BOM。
- 命令前加 `ens:`（大小写不敏感）表示该命令失败时终止 SCT，并返回命令的错误码。
- 不加 `ens:` 的命令失败后继续执行下一行。
- `exit` 命令会结束脚本并跳过交互 Shell。
- 脚本按启动时的 SCT 进程权限执行；需要提升的单条命令请使用 `sudo <command>`。

示例：

```text
whoami
cd C:\Windows
ens:dir
shell -c echo autoexec done
```

如果 `ens:` 命令失败，SCT 会立即退出，不会进入交互提示符。

## 15. 插件与 GUI 终端

### 15.1 插件命令

```text
plugin -list
plugin -enable <sha256>
plugin -disable <sha256>
```

- 插件放在程序目录的 `plugin\x64\*.dll` 或 `plugin\x86\*.dll`。
- `plugin -list` 显示文件 SHA-256、签名者、SPKI SHA-256 和信任状态。
- 未签名或未固定（pin）的插件必须手动执行 `plugin -enable <sha256>`。
- 已固定签名者的插件在签名有效时自动加载。
- 启用列表保存在程序目录的 `plugin-state.dat`，不是缓存目录；文件使用 AES-256-GCM 加密并原子替换，便于随程序目录移动。
- 状态加密只能防止普通未授权修改；密钥嵌入程序，不能抵抗逆向分析。不要把 `plugin-state.dat` 当作安全边界。

### 15.2 签名者 pin

在 `plugin_trusted_signers.h` 的 `trusted_plugin_signer_pins()` 中加入签名证书的 SubjectPublicKeyInfo SHA-256（小写十六进制）。可以同时加入多张证书，用于多个开发者或证书续期。

```cpp
static const std::vector<std::string> pins = {
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
    "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210",
};
```

信任条件是 Authenticode 签名链有效且 SPKI pin 匹配。仅 pin 不会绕过证书链验证。

### 15.3 autoexec 限制

`autoexec.sct` 中禁止 `plugin -enable`，启动时会先检查整个文件并拒绝执行；`plugin -disable` 允许。需要启用插件时，请在交互提示符中手动执行，避免启动脚本静默加载未授权 DLL。

### 15.4 GUI 终端

- 默认启动 Win32 GUI 终端，不创建控制台窗口。
- 使用 `main.exe --console` 强制控制台模式；如果从已有控制台启动，会附加到父控制台，否则创建新控制台。
- GUI 中 `shell` 默认捕获输出并显示在终端内；`shell -window`（或 `-w`）让子进程使用独立窗口，`-hide`（或 `-h`）保持隐藏。
- GUI 使用单个 RichEdit 控件，输入直接显示在提示符后，支持 ANSI SGR 颜色、清屏、历史记录、光标编辑和 Ctrl+C 取消当前输入/命令。
- 提示符和已输出内容不会被编辑；Backspace、Delete、粘贴等操作只作用于当前输入行。

### 15.5 构建

构建脚本使用 `-mwindows` 生成 GUI 子系统程序。发布包仍包含 `--console` 入口和 PPL/elevation helper 模式。
