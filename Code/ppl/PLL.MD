# PPLcontrol DLL 调用说明

本文说明如何使用 `ppl64.dll` 与 `ppl32.dll`。

- `ppl64.dll`：64 位版本，内嵌加密的 `RTCore64.sys`
- `ppl32.dll`：32 位版本，内嵌加密的 `RTCore32.sys`

DLL 在加载时不会解密驱动。首次执行需要访问内核的操作时，才会解密内嵌驱动、写入随机临时文件、注册并启动驱动服务，随后删除临时文件。

---

## 1. 架构选择

| 运行环境 | 使用 DLL |
| --- | --- |
| 64 位 Windows | `ppl64.dll` |
| 32 位 Windows | `ppl32.dll` |

注意：`ppl32.dll` 不能用于在 64 位 Windows 上操作 64 位内核。它面向真正的 32 位 Windows。

---

## 2. 命令格式

```text
ppl64.dll <CMD> <ARGS>
```

支持的命令：

```text
list
get <PID>
set <PID> <PP|PPL> <TYPE>
protect <PID> <PP|PPL> <TYPE>
unprotect <PID>
```

`TYPE` 可选值：

```text
Authenticode, CodeGen, Antimalware, Lsa, Windows, WinTcb, WinSystem
```

示例：

```text
list
get 1234
set 1234 PPL WinTcb
protect 1234 PPL WinTcb
unprotect 1234
```

---

## 3. 使用 rundll32 调用

```bat
rundll32.exe ppl64.dll,PPLcontrolRunDll32 list
rundll32.exe ppl64.dll,PPLcontrolRunDll32 get 1234
rundll32.exe ppl64.dll,PPLcontrolRunDll32 set 1234 PPL WinTcb
rundll32.exe ppl64.dll,PPLcontrolRunDll32 protect 1234 PPL WinTcb
rundll32.exe ppl64.dll,PPLcontrolRunDll32 unprotect 1234
```

建议使用 DLL 的完整路径：

```bat
rundll32.exe "C:\Tools\ppl64.dll",PPLcontrolRunDll32 list
```

说明：

- `rundll32` 是 GUI 程序，部分环境下控制台输出可能不可见。
- 如果需要稳定查看输出和返回码，建议使用下面的加载器方式。
- 驱动加载和内核读写需要管理员权限，请以管理员身份运行终端。

---

## 4. 导出函数

所有导出函数均为 C 链接。除 `PPLcontrolRunDll32` 为 `__stdcall` 外，其余为 `__cdecl`。

```c
int PPLcontrolRun(int argc, wchar_t** argv);

int PPLcontrolRunCommandLineW(LPCWSTR CommandLine);
int PPLcontrolRunCommandLineA(LPCSTR CommandLine);

void __stdcall PPLcontrolRunDll32(
    HWND hWnd,
    HINSTANCE hInst,
    LPSTR CmdLine,
    INT nCmdShow
);

BOOL PPLcontrolExtractDriver(LPCWSTR Path);
BOOL PPLcontrolInstallDriver(void);
BOOL PPLcontrolUninstallDriver(void);
LPCWSTR PPLcontrolGetDriverName(void);
```

### 返回值

`PPLcontrolRun` / `PPLcontrolRunCommandLineW` / `PPLcontrolRunCommandLineA`：

| 返回值 | 含义 |
| --- | --- |
| `0` | 成功 |
| `1` | 参数错误、未知命令或打印帮助 |
| `2` | 操作失败，例如偏移查找失败、驱动加载失败、读写失败 |

`PPLcontrolExtractDriver` / `PPLcontrolInstallDriver` / `PPLcontrolUninstallDriver`：

- 返回非零表示成功
- 返回 `0` 表示失败

`PPLcontrolGetDriverName`：

- 64 位返回 `RTCore64`
- 32 位返回 `RTCore32`

---

## 5. 使用加载器调用（推荐）

将下面的代码保存为 `loader.cpp`，编译出的 EXE 与 DLL 放在同一目录：

```cpp
#include <windows.h>
#include <cstdio>
#include <string>

typedef int (__cdecl *PFN_RunCommandLineW)(LPCWSTR);

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        wprintf(L"Usage: loader.exe <command line>\n");
        return 1;
    }

    HMODULE hModule = LoadLibraryW(L"ppl64.dll");
    if (!hModule)
    {
        wprintf(L"LoadLibraryW failed: %lu\n", GetLastError());
        return 2;
    }

    PFN_RunCommandLineW pfnRun =
        (PFN_RunCommandLineW)GetProcAddress(hModule, "PPLcontrolRunCommandLineW");

    if (!pfnRun)
    {
        wprintf(L"GetProcAddress failed: %lu\n", GetLastError());
        FreeLibrary(hModule);
        return 3;
    }

    std::wstring CommandLine;

    for (int i = 1; i < argc; i++)
    {
        if (i > 1)
            CommandLine += L" ";

        CommandLine += argv[i];
    }

    int result = pfnRun(CommandLine.c_str());

    wprintf(L"result = %d\n", result);

    FreeLibrary(hModule);
    return result;
}
```

64 位编译示例：

```bat
g++ -O2 -municode -o loader64.exe loader.cpp
```

32 位编译示例：

```bat
i686-w64-mingw32-g++ -O2 -municode -o loader32.exe loader.cpp
```

调用示例：

```bat
loader64.exe list
loader64.exe get 1234
loader64.exe protect 1234 PPL WinTcb
loader64.exe unprotect 1234
```

如果需要切换 DLL，将源码中的 `ppl64.dll` 改为 `ppl32.dll`，并使用 32 位编译器生成 32 位加载器。

---

## 6. 直接使用 `PPLcontrolRun`

也可以直接传入 argv：

```cpp
#include <windows.h>

typedef int (__cdecl *PFN_Run)(int, wchar_t**);

int wmain()
{
    HMODULE hModule = LoadLibraryW(L"ppl64.dll");
    if (!hModule)
        return 1;

    PFN_Run pfnRun = (PFN_Run)GetProcAddress(hModule, "PPLcontrolRun");
    if (!pfnRun)
    {
        FreeLibrary(hModule);
        return 2;
    }

    wchar_t program[] = L"ppl64.dll";
    wchar_t command[] = L"list";
    wchar_t* argv[] = { program, command, nullptr };

    int result = pfnRun(2, argv);

    FreeLibrary(hModule);
    return result;
}
```

注意：`PPLcontrolRun` 的 `argv[0]` 是程序名，`argv[1]` 才是命令。

---

## 7. 驱动管理接口

### 7.1 自动加载

正常执行 `list`、`get`、`set`、`protect`、`unprotect` 时，如果需要设备但驱动尚未加载，DLL 会自动：

1. 从 DLL 资源中读取加密驱动
2. 使用 AES-256-CBC 解密
3. 写入随机临时文件
4. 创建并启动 `RTCore64` 或 `RTCore32` 服务
5. 打开设备
6. 删除临时文件

### 7.2 手动安装

```cpp
BOOL ok = PPLcontrolInstallDriver();
```

### 7.3 手动卸载

```cpp
BOOL ok = PPLcontrolUninstallDriver();
```

卸载接口会停止并删除对应驱动服务。

### 7.4 导出明文驱动

```cpp
BOOL ok = PPLcontrolExtractDriver(L"C:\\Temp\\RTCore64.sys");
```

该接口会解密驱动并将明文写入指定路径。仅用于调试或手动安装，使用后请及时删除。

---

## 8. C++ 调用示例：查询进程保护

```cpp
#include <windows.h>
#include <cstdio>

typedef int (__cdecl *PFN_RunCommandLineW)(LPCWSTR);

int wmain(int argc, wchar_t** argv)
{
    HMODULE hModule = LoadLibraryW(L"ppl64.dll");
    if (!hModule)
        return 1;

    PFN_RunCommandLineW pfnRun =
        (PFN_RunCommandLineW)GetProcAddress(hModule, "PPLcontrolRunCommandLineW");

    if (!pfnRun)
    {
        FreeLibrary(hModule);
        return 2;
    }

    int result = pfnRun(L"get 1234");
    wprintf(L"result = %d\n", result);

    FreeLibrary(hModule);
    return result;
}
```

---

## 9. 32 位调用注意事项

32 位下必须使用正确的函数指针调用约定：

```cpp
typedef int (__cdecl *PFN_RunCommandLineW)(LPCWSTR);
typedef BOOL (__cdecl *PFN_Extract)(LPCWSTR);
```

不要对除 `PPLcontrolRunDll32` 以外的导出函数使用 `WINAPI` / `__stdcall` 函数指针，否则可能造成栈不平衡并崩溃。

`PPLcontrolRunDll32` 的调用约定是 `__stdcall`，供 `rundll32` 使用。

---

## 10. 运行要求与注意事项

- 需要管理员权限。
- 需要加载内核驱动，建议在测试虚拟机中使用。
- `RTCore32.sys` 未签名。
- `RTCore64.sys` 虽然带签名，但在启用 DSE、Secure Boot 或微软易受攻击驱动黑名单的系统中可能被拦截。
- 64 位 DLL 依赖系统 UCRT，Windows 10/11 默认自带。
- 自动加载驱动时会短暂写入解密后的临时驱动文件，随后删除；如果进程异常终止，临时文件可能残留，可手动清理 `%TEMP%\ppl*.tmp`。
- 不要在不需要时保持驱动长期加载。

---

## 11. 重新构建

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File PPLcontrol\build_dll.ps1
```

输出目录：

```text
dist\ppl64.dll
dist\ppl32.dll
```

构建脚本会生成加密资源与密钥头文件：

```text
PPLcontrol\generated\RTCore64.enc
PPLcontrol\generated\RTCore32.enc
PPLcontrol\generated\DriverKey.h
PPLcontrol\generated\driver.key
```

`driver.key` 用于构建时加密驱动，发布时只需要分发 DLL。

---

## 12. 快速检查

```bat
rundll32.exe ppl64.dll,PPLcontrolRunDll32 list
```

或使用加载器：

```bat
loader64.exe list
loader64.exe get 1234
loader64.exe protect 1234 PPL WinTcb
loader64.exe unprotect 1234
```
