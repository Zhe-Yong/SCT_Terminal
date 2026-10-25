# 命令维护说明

本文说明 SuperCmdToolkit 的命令结构、新增命令方式、参数规则和维护约定。

## 1. 目录结构

```text
SuperCmdToolkit/
├── command.h             命令参数、上下文、注册表接口
├── command.cpp           参数解析、注册表、命令执行
├── commands/             每个命令一个独立 .cpp
│   ├── help.cpp
│   ├── cd.cpp
│   ├── exit.cpp
│   ├── whoami.cpp
│   └── sudo.cpp
├── global.h              集中包含系统头文件和项目头文件
├── tools/
│   └── build-release.ps1 自动编译根目录和 commands/ 下的 .cpp
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
    "echo", "Print text.", make_echo_schema(), handle_echo);
```

程序启动时，`CommandRegistration` 会自动把命令加入全局注册表。命令会出现在 `help` 列表中，不需要修改中央代码。

### 2.5 构建和测试

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

| 命令 | 文件 | 参数规则 |
| --- | --- | --- |
| `help` | `commands/help.cpp` | 无参数 |
| `cd` | `commands/cd.cpp` | `-d` 开关，最多 1 个位置参数 |
| `exit` | `commands/exit.cpp` | 无参数 |
| `whoami` | `commands/whoami.cpp` | 无参数 |
| `sudo` | `commands/sudo.cpp` | `raw_tail`，最多 1 个位置参数 |

`sudo` 支持：

```text
sudo ad
sudo sys
sudo su
sudo exit
sudo <command>
```

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

参数解析错误由 `execute_command_line` 自动输出，处理函数通常不需要重复打印。

## 10. 代码约定

- 每个 `.cpp` 只包含 `#include "global.h"`。
- 新命令放在 `commands/` 下，一个命令一个文件。
- 新命令使用静态 `CommandRegistration` 对象自动注册。
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

x86 需要安装 MSYS2 mingw32 工具链：

```text
C:\msys64\mingw32\bin\g++.exe
```

构建脚本会自动：

- 编译根目录 `*.cpp`
- 编译 `commands/*.cpp`
- 添加项目根目录为 include 路径
- 复制 NSudo 运行时
- 生成 `release/SCT_<arch>.zip`
