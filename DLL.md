# SCT DLL 插件编写规范

本文面向 DLL 插件开发者，说明 SuperCommandTerminal（以下简称 SCT）的插件 ABI、必需导出函数、命令描述符、Host API、构建方式、启用流程和安全要求。

插件运行在 SCT 进程内，与 SCT 拥有相同权限。SCT 不对插件做沙箱隔离；一个插件崩溃或卡死，可能直接导致 SCT 崩溃或无响应。只加载可信、可审计的插件。

## 1. 目录与加载规则

插件放在 SCT 资源目录下，按架构分目录：

```text
<resource-dir>/
├── SCT64.exe
├── SCT32.exe
├── plugin/
│   ├── x64/
│   │   └── hello.dll
│   └── x86/
│       └── hello.dll
└── plugin-state.dat
```

- SCT64 只加载 `plugin/x64/*.dll`，SCT32 只加载 `plugin/x86/*.dll`；插件架构必须与 SCT 一致。
- 普通发布版的资源目录就是发布目录，例如 `release/normal/`。
- random 发布版的实际映像位于 `%LOCALAPPDATA%\SCT\runtime\`，但插件、`autoexec.sct`、NSudo 和 PPL DLL 仍从原始发布目录读取，例如 `release/random/plugin/x64/`。
- 调试版从 `Code/build/debug/plugin/x64/` 或 `Code/build/debug/plugin/x86/` 读取。
- 只扫描 `*.dll`，按文件名排序加载；建议文件名使用稳定的 ASCII 名称，例如 `hello.dll`。
- 启用状态保存在资源目录的 `plugin-state.dat`，不在缓存目录。
- DLL 依赖使用 `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS` 加载。插件自己的依赖应放在插件目录中，不要依赖当前工作目录。

## 2. 安全模型

- 未签名或未固定（pin）的插件不会自动加载。必须由用户在交互命令行中执行 `plugin -enable <sha256>`。
- `autoexec.sct` 中禁止 `plugin -enable`；`plugin -disable` 允许。
- 已固定签名者的插件，只有在 Authenticode 签名链有效且签名者 SPKI pin 匹配时才会自动加载。
- `plugin-state.dat` 使用 AES-256-GCM 加密并原子替换，但密钥嵌入程序，不能作为安全边界。
- 插件以 SCT 的权限运行。不要把“能加载”误解为“已隔离”或“低权限”。
- 应限制插件目录和 `plugin-state.dat` 的写权限；能替换 DLL 的用户可以在当前 SCT 用户权限下执行代码。
- SCT 的 `plugin -disable` 会直接 `FreeLibrary` 卸载 DLL；当前 ABI 没有卸载回调。插件不得留下长期运行的线程、窗口、钩子、定时器或全局回调。

## 3. ABI 版本与必需导出

插件必须使用 `Code/sdk/sct_plugin.h`，不要复制后修改结构体布局。ABI 版本为：

```c
#define SCT_PLUGIN_ABI_VERSION 1u
```

编译插件时必须定义 `SCT_PLUGIN_BUILD`，这样头文件中的 `SCT_PLUGIN_EXPORT` 才会展开为 `__declspec(dllexport)`。

DLL 必须导出以下两个函数：

```c
SCT_PLUGIN_EXPORT uint32_t SCT_PLUGIN_CALL sct_plugin_abi_version(void);
SCT_PLUGIN_EXPORT int SCT_PLUGIN_CALL
sct_plugin_register(const SctHostApi* api);
```

可选导出：

```c
SCT_PLUGIN_EXPORT const char* SCT_PLUGIN_CALL sct_plugin_dll_ver(void);
```

| 导出函数 | 必需 | 说明 |
| --- | --- | --- |
| `sct_plugin_abi_version` | 是 | 返回 `SCT_PLUGIN_ABI_VERSION`。版本不匹配时 SCT 拒绝加载。 |
| `sct_plugin_register` | 是 | 注册插件命令。返回 `SCT_ERR_NONE` 表示成功；其他值表示失败。 |
| `sct_plugin_dll_ver` | 否 | 返回静态生命周期的版本字符串，例如 `"1.0.0"`。未导出时回退到 DLL 的 `FileVersion` 资源。 |

调用约定：

- 使用头文件中的 `SCT_PLUGIN_CALL`。
- x86 为 `__cdecl`，x64 使用默认调用约定。
- 不要使用 `__stdcall`，也不要让 C++ 异常穿过 C ABI 边界。
- 导出函数使用 `extern "C"`，避免 C++ 名称修饰。

SCT 会优先按原始名称查找导出，也兼容 x86 常见的 `_name` 和 `_name@0` 修饰形式。仍建议保持导出名称完全一致。

## 4. 最小可运行插件

### 4.1 hello.cpp

下面的插件注册一个 `hello` 命令，支持 `-n|--name <name>` 和 `--loud`。

```cpp
#include "sct_plugin.h"

#include <cstring>

namespace {

int SCT_PLUGIN_CALL handle_hello(SctCommandContext* context) {
    if (!context || !context->api) {
        return SCT_ERR_UNKNOWN;
    }

    const SctHostApi* api = context->api;
    void* host = context->host;

    api->set_color(host, SCT_CLR_BRIGHT_GREEN, SCT_CLR_NONE, SCT_CLR_NONE);

    const char greeting[] = "Hello from SCT plugin.\n";
    api->write_utf8(host, greeting, sizeof(greeting) - 1);

    if (api->has_option(host, "name")) {
        const char* name = api->option_value(host, "name");
        if (name) {
            const char label[] = "name: ";
            api->write_utf8(host, label, sizeof(label) - 1);
            api->write_utf8(host, name,
                            static_cast<uint32_t>(std::strlen(name)));
            api->write_utf8(host, "\n", 1);
        }
    }

    if (api->has_option(host, "loud")) {
        const char loud[] = "LOUD MODE\n";
        api->write_utf8(host, loud, sizeof(loud) - 1);
    }

    api->reset_color(host);
    return SCT_ERR_NONE;
}

const SctCommandOption kHelloOptions[] = {
    {"name", SCT_OPTION_REQUIRED},
    {"loud", SCT_OPTION_FLAG},
};

const SctCommandDescriptor kHelloCommands[] = {
    {
        "hello",
        "Print a greeting from a DLL plugin.",
        {kHelloOptions, 2, 0, 0, 0},
        0,
        &handle_hello,
    },
};

}  // namespace

extern "C" SCT_PLUGIN_EXPORT uint32_t SCT_PLUGIN_CALL
sct_plugin_abi_version(void) {
    return SCT_PLUGIN_ABI_VERSION;
}

extern "C" SCT_PLUGIN_EXPORT const char* SCT_PLUGIN_CALL
sct_plugin_dll_ver(void) {
    return "1.0.0";
}

extern "C" SCT_PLUGIN_EXPORT int SCT_PLUGIN_CALL
sct_plugin_register(const SctHostApi* api) {
    if (!api || api->abi_version != SCT_PLUGIN_ABI_VERSION ||
        api->struct_size < sizeof(SctHostApi) ||
        !api->register_commands) {
        return SCT_ERR_INVALID_OPTION;
    }

    return api->register_commands(
        api->host, kHelloCommands,
        static_cast<uint32_t>(sizeof(kHelloCommands) /
                              sizeof(kHelloCommands[0])));
}
```

说明：

- `SctCommandSchema` 的字段顺序是：`options`、`option_count`、`min_positionals`、`max_positionals`、`raw_tail`。
- `SctCommandDescriptor` 的字段顺序是：`name`、`description`、`schema`、`require_elevation`、`handler`。
- 命令名、描述、选项名和 schema 会复制到 SCT 内部；handler 函数指针仍指向 DLL，因此 DLL 必须保持加载。注册后修改描述符表不会改变已注册命令。
- `sct_plugin_dll_ver` 返回的指针必须长期有效；不要在函数内返回局部缓冲区。
- 插件命令的详细帮助目前不能通过 ABI 提供；`help <command>` 只显示 `description`。

### 4.2 MinGW 构建

在对应架构的 MSYS2 环境中执行；x64 使用 x64 编译器，x86 使用 i686 编译器。

x64：

```powershell
g++ `
    -std=c++17 -O2 -shared `
    -DSCT_PLUGIN_BUILD `
    -I .\Code\sdk `
    -o hello.dll hello.cpp `
    -static-libgcc -static-libstdc++
```

x86：

```powershell
i686-w64-mingw32-g++ `
    -std=c++17 -O2 -shared `
    -DSCT_PLUGIN_BUILD `
    -I .\Code\sdk `
    -o hello.dll hello.cpp `
    -static-libgcc -static-libstdc++
```

### 4.3 MSVC 构建

在 Developer Command Prompt 中：

```bat
cl /nologo /LD /EHsc /std:c++17 /DSCT_PLUGIN_BUILD ^
   /I Code\sdk hello.cpp /Fe:hello.dll
```

x86 构建请使用 x86 Native Tools 命令提示符，x64 构建请使用 x64 Native Tools 命令提示符。

### 4.4 安装和启用

将 DLL 放到对应目录：

```text
release\normal\plugin\x64\hello.dll
release\normal\plugin\x86\hello.dll
```

启动 SCT 后：

```text
plugin -list
plugin -enable <hello.dll 的 SHA-256>
hello --name Alice --loud
```

计算 SHA-256：

```powershell
Get-FileHash -Algorithm SHA256 .\release\normal\plugin\x64\hello.dll
```

`plugin -enable` 使用完整文件 SHA-256，文件任何变化都会变成新的插件身份。签名后必须对签名后的文件重新计算 SHA-256。

## 5. 命令描述符规范

### 5.1 基本字段

```c
typedef struct SctCommandDescriptor {
    const char* name;
    const char* description;
    SctCommandSchema schema;
    int32_t require_elevation;
    SctCommandHandler handler;
} SctCommandDescriptor;
```

- `name`：命令名，必须非空。SCT 内部按小写匹配；建议全部使用小写 ASCII，不要与内置命令冲突。
- `description`：显示在 `help` 列表和 `help <command>` 中。
- `schema`：参数规则。
- `require_elevation`：最低权限要求，见 5.3。
- `handler`：命令处理函数，不能为空。

一个插件一次最多注册 256 个命令；单个命令最多 128 个选项。注册是“全有或全无”的：任意一个命令注册失败，SCT 会移除该插件本次注册的所有命令，并拒绝加载该插件。

### 5.2 参数 schema

```c
typedef struct SctCommandOption {
    const char* name;
    uint32_t value_mode;
} SctCommandOption;

typedef struct SctCommandSchema {
    const SctCommandOption* options;
    uint32_t option_count;
    uint32_t min_positionals;
    uint32_t max_positionals;
    uint32_t raw_tail;
} SctCommandSchema;
```

选项模式：

| 模式 | 值 | 说明 |
| --- | --- | --- |
| `SCT_OPTION_FLAG` | 0 | 不带值，例如 `--loud`。 |
| `SCT_OPTION_REQUIRED` | 1 | 必须带值，例如 `--name Alice` 或 `--name=Alice`。 |
| `SCT_OPTION_OPTIONAL` | 2 | 可带值；没有值时 `option_value` 返回 `NULL`。 |

参数解析规则：

- 支持 `-name`、`--name`、`/name`，也支持 `--name=value`。
- 选项名大小写不敏感；schema 中建议使用小写。
- 选项值不能消费另一个“已知选项”；需要把选项样子的文本作为值时，使用 `--` 结束选项解析。
- 同一个选项重复出现会报错。
- 未声明的选项会报错。
- `min_positionals` 和 `max_positionals` 限制非选项参数数量；不需要位置参数时都设为 0。
- `raw_tail` 非 0 时，解析器只把第一个 token 作为位置参数，剩余内容不解析；此时应通过 `raw_line` 获取完整原始命令行。不要同时依赖 `raw_tail` 和选项解析。

### 5.3 require_elevation

`require_elevation` 会被限制在 0 到 4：

| 值 | 最低权限 |
| --- | --- |
| 0 或 1 | 不限制 |
| 2 | Administrator |
| 3 | System |
| 4 | TrustedInstaller |

SCT 在调用 handler 前检查当前进程权限；不满足时不会调用插件。该字段不会自动提升权限，用户需要先使用 `sudo` 切换到所需权限级别。

### 5.4 handler 返回值

返回 `SCT_ERR_NONE` 表示成功，其他值表示失败。推荐使用 SDK 中的 `SCT_ERR_*` 常量：

```c
return SCT_ERR_FILE_NOT_FOUND;
return SCT_ERR_PERMISSION_DENIED;
return SCT_ERR_CANCELLED;
```

不要在插件中调用 `ExitProcess`、`TerminateProcess`、`abort` 或 `std::terminate`。这些调用会直接结束 SCT 进程。

## 6. Host API

`SctHostApi` 由 SCT 提供，插件通过 `api->host` 作为不透明句柄调用：

```c
typedef struct SctHostApi {
    uint32_t abi_version;
    uint32_t struct_size;
    void* host;

    int (SCT_PLUGIN_CALL *register_commands)(
        void* host, const SctCommandDescriptor* commands,
        uint32_t command_count);
    void (SCT_PLUGIN_CALL *write_utf8)(
        void* host, const char* text, uint32_t length);
    void (SCT_PLUGIN_CALL *set_color)(
        void* host, int32_t fg, int32_t bg, int32_t style);
    void (SCT_PLUGIN_CALL *reset_color)(void* host);
    int (SCT_PLUGIN_CALL *has_option)(void* host, const char* name);
    const char* (SCT_PLUGIN_CALL *option_value)(void* host, const char* name);
    uint32_t (SCT_PLUGIN_CALL *positional_count)(void* host);
    const char* (SCT_PLUGIN_CALL *positional)(void* host, uint32_t index);
    const char* (SCT_PLUGIN_CALL *raw_line)(void* host);
    int (SCT_PLUGIN_CALL *is_cancelled)(void* host);
    void (SCT_PLUGIN_CALL *request_exit)(void* host);
} SctHostApi;
```

| 函数 | 说明 |
| --- | --- |
| `register_commands` | 只能在 `sct_plugin_register` 执行期间调用。成功后命令加入 SCT 注册表。 |
| `write_utf8` | 输出指定长度的 UTF-8 字节，不要求以 `\0` 结尾；长度必须准确。 |
| `set_color` | 设置前景色、背景色和样式；`SCT_CLR_NONE` 表示该通道保持不变。 |
| `reset_color` | 重置颜色和样式。输出结束后应调用。 |
| `has_option` | 选项是否出现，返回 1/0。 |
| `option_value` | 返回选项值；选项无值、未出现或为 flag 时返回 `NULL`。 |
| `positional_count` | 返回位置参数数量。 |
| `positional` | 返回指定位置参数；越界返回 `NULL`。 |
| `raw_line` | 返回完整原始命令行；不会返回 `NULL`。 |
| `is_cancelled` | 用户是否已请求取消当前命令，返回 1/0。 |
| `request_exit` | 请求当前命令返回后退出 SCT。 |

API 使用规则：

- handler 中优先使用 `context->api` 和 `context->host`。不要自己构造 `SctCommandContext`。
- `option_value`、`positional`、`raw_line` 返回的指针只在当前 handler 调用期间有效；不要保存到全局变量或异步线程。
- `write_utf8` 必须通过 Host API 调用。不要用 `std::cout`、`printf` 或直接写控制台/GUI，这些输出不会进入 SCT 的 GUI 终端。
- 颜色常量使用 `SCT_CLR_*`。使用 `reset_color` 恢复默认状态。
- 不要从其他线程调用 Host API。命令 handler 运行在 SCT 的命令 worker 线程中，Host API 使用线程局部上下文。
- 不要把 C++ 标准库对象（`std::string`、`std::vector` 等）通过 ABI 传给 SCT；ABI 边界只使用 C 结构体、函数指针和 `const char*`。

## 7. 线程、取消和生命周期

- handler 在命令 worker 线程中执行。SCT 同一时间通常只执行一个命令，但插件仍应避免无保护的全局可变状态。
- 长循环、文件遍历、等待和网络操作应定期调用 `api->is_cancelled(host)`；检测到取消后应立即返回 `SCT_ERR_CANCELLED`。
- SCT 在用户取消后会等待约 250ms；仍未返回的 worker 线程可能被强制终止。强制终止时不会执行插件清理代码，因此不要长时间阻塞，也不要在阻塞期间持有必须释放的全局资源。
- DLL 被 `plugin -disable` 或 SCT 退出时会 `FreeLibrary`。当前 ABI 没有 `sct_plugin_unload` 回调。
- 不要在 `DllMain` 中调用 Host API、创建线程、加载 DLL 或执行复杂初始化。需要初始化的内容放在 `sct_plugin_register` 中。
- 不要在插件中创建长期后台线程、窗口、钩子或定时器。若确实需要资源，必须在命令返回前释放；不要假设禁用前会收到通知。
- 插件抛出的 C++ 异常不应穿过 C ABI。请在插件内部捕获所有异常并转换成 `SCT_ERR_*`。
- 插件不得依赖 SCT 的未导出内部符号；只使用 `Code/sdk/sct_plugin.h` 中声明的 ABI。

## 8. 输出和 UTF-8

- 所有文本输出使用 UTF-8 编码。
- `write_utf8` 的第三个参数是字节数，不是字符数。
- 字符串字面量可以使用 `sizeof(text) - 1` 作为长度。
- 不要把 `\0` 计入输出长度，除非确实需要输出嵌入的 NUL。
- 输出可以分段调用；SCT 会按调用顺序写入当前命令输出。
- 需要颜色时先 `set_color`，输出完成后调用 `reset_color`，避免颜色泄漏到后续输出。

## 9. 签名、pin 和信任

### 9.1 未签名插件

未签名或未固定签名者的插件默认禁用。流程：

```text
plugin -list
plugin -enable <sha256>
```

`plugin -list` 显示插件文件名、`dll_ver`、SHA-256、签名者、SPKI SHA-256、信任状态、加载状态和错误信息。`plugin -enable` 不能在 `autoexec.sct` 中使用。

### 9.2 Authenticode 签名

SCT 自带 `cert` 命令。签名用法：

```text
cert -sign <pfx> [-pwd <password>] <file>
```

示例：

```text
cert -sign developer.pfx -pwd secret .\release\normal\plugin\x64\hello.dll
```

签名后重新计算文件 SHA-256，再执行 `plugin -enable`。修改 DLL 后旧 hash 的启用状态不会自动适用于新文件。

### 9.3 固定签名者

在 `Code/plugin_trusted_signers.h` 的 `trusted_plugin_signer_pins()` 中加入签名证书的 SubjectPublicKeyInfo SHA-256（小写十六进制），然后重新构建 SCT：

```cpp
inline const std::vector<std::string>& trusted_plugin_signer_pins() {
    static const std::vector<std::string> pins = {
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
    };
    return pins;
}
```

- pin 匹配只是必要条件；还必须通过 Authenticode 证书链验证。
- 仅把证书加入 pin 列表不会绕过证书链验证。
- 自签名开发证书通常还需要被系统信任，或者使用受信任 CA 签发的代码签名证书。
- 匹配 pin 且签名有效的插件会自动加载；无需手动 `plugin -enable`。
- pin 列表编译进 SCT，适合固定发布者；不要从可被普通用户修改的文件读取信任列表。

## 10. 发布前检查清单

- [ ] 使用与 SCT 相同架构的编译器构建（x64 对应 SCT64，x86 对应 SCT32）。
- [ ] 定义了 `SCT_PLUGIN_BUILD`。
- [ ] 导出 `sct_plugin_abi_version`、`sct_plugin_register`，可选导出 `sct_plugin_dll_ver`。
- [ ] `sct_plugin_abi_version` 返回 `SCT_PLUGIN_ABI_VERSION`。
- [ ] 所有命令名、选项名使用小写 ASCII，且不与内置命令冲突。
- [ ] handler 不抛出 C++ 异常，不调用 `ExitProcess` / `TerminateProcess` / `abort`。
- [ ] 输出全部通过 `write_utf8`，长度使用字节数，并在需要时重置颜色。
- [ ] 长操作轮询 `is_cancelled` 并返回 `SCT_ERR_CANCELLED`。
- [ ] 没有长期后台线程、窗口、钩子或定时器，或已确保在命令返回前释放。
- [ ] 依赖 DLL 放在插件目录，未依赖当前工作目录。
- [ ] 已用 `plugin -list` 检查 SHA-256、签名和加载状态。
- [ ] 需要自动加载时已正确签名，并把 SPKI pin 加入 `plugin_trusted_signers.h`。
- [ ] 已用 x64 和 x86 版本分别验证。

## 11. 常见问题

### 插件显示 unsupported plugin ABI

`SCT_PLUGIN_ABI_VERSION` 不一致。更新 `sct_plugin.h` 并重新编译插件；不要修改头文件里的结构体布局来绕过检查。

### 插件显示 does not export the SCT plugin ABI

检查导出名称、`extern "C"`、`SCT_PLUGIN_BUILD` 和调用约定。用 `dumpbin /exports hello.dll`（MSVC）或 `objdump -p hello.dll`（MinGW）确认导出。

### 命令注册失败

常见原因：

- 命令名与内置命令或其他已加载插件冲突。
- 命令名、选项名或 handler 为空。
- 选项数量超过 128，或命令数超过 256。
- schema 的 `option_count` 与 `options` 指针不一致。
- `register_commands` 不是在 `sct_plugin_register` 执行期间调用。

注册失败时，插件本次注册的所有命令都会被移除，DLL 不会保持加载。

### 命令没有输出

确认使用 `api->write_utf8(host, text, length)`，而不是 `std::cout`/`printf`；确认长度正确，且命令确实执行成功。

### 修改 DLL 后没有生效

SCT 按 SHA-256 识别插件。重新计算 hash，执行 `plugin -disable <旧 hash>` 和 `plugin -enable <新 hash>`，或重启 SCT。已固定签名者的插件重新签名并匹配 pin 后可自动加载。

### 插件导致 SCT 无响应

检查是否存在阻塞调用、无限循环或未处理的对话框。命令 worker 被取消后会等待约 250ms，之后可能被强制终止；插件无法在强制终止时执行清理。
