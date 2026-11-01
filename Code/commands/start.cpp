#include "global.h"

namespace {

enum class LaunchResult {
    Success,
    NotSupported,
    Failed
};

std::string start_usage() {
    return "Usage: start <program|script|document> [arguments...]\n";
}

bool split_start_command(const std::string& tail,
                         std::string& target,
                         std::string& arguments) {
    size_t position = 0;
    while (position < tail.size() &&
           (tail[position] == ' ' || tail[position] == '\t')) {
        ++position;
    }
    if (position >= tail.size()) return false;

    if (tail[position] == '"') {
        size_t end = tail.find('"', position + 1);
        if (end == std::string::npos) return false;
        target = tail.substr(position + 1, end - position - 1);
        position = end + 1;
    } else {
        size_t end = position;
        while (end < tail.size() && tail[end] != ' ' && tail[end] != '\t') {
            ++end;
        }
        target = tail.substr(position, end - position);
        position = end;
    }

    while (position < tail.size() &&
           (tail[position] == ' ' || tail[position] == '\t')) {
        ++position;
    }
    arguments = tail.substr(position);
    return !target.empty();
}

std::wstring system_file(const wchar_t* name) {
    std::vector<wchar_t> buffer(32768, L'\0');
    UINT length = GetSystemDirectoryW(
        buffer.data(), static_cast<UINT>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return name;
    return std::wstring(buffer.data(), length) + L"\\" + name;
}

std::wstring cmd_path() {
    std::vector<wchar_t> buffer(32768, L'\0');
    DWORD length = GetEnvironmentVariableW(
        L"ComSpec", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) {
        return std::wstring(buffer.data(), length);
    }
    return system_file(L"cmd.exe");
}

std::wstring powershell_path() {
    return system_file(L"WindowsPowerShell\\v1.0\\powershell.exe");
}

std::wstring wscript_path() {
    return system_file(L"wscript.exe");
}

bool file_exists(const std::wstring& path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

int start_error_code(DWORD code) {
    switch (code) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return ERR_FILE_NOT_FOUND;
        case ERROR_ACCESS_DENIED:
        case ERROR_ELEVATION_REQUIRED:
            return ERR_PERMISSION_DENIED;
        default:
            return ERR_UNKNOWN;
    }
}

bool create_detached_process(const std::wstring& application,
                             const std::wstring& command_line,
                             std::string& error,
                             DWORD& code) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_SHOWNORMAL;

    PROCESS_INFORMATION process{};
    std::vector<wchar_t> buffer(command_line.begin(), command_line.end());
    buffer.push_back(L'\0');

    DWORD flags = CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT;
    BOOL created = CreateProcessW(
        application.empty() ? nullptr : application.c_str(), buffer.data(),
        nullptr, nullptr, FALSE, flags | CREATE_BREAKAWAY_FROM_JOB, nullptr,
        nullptr, &startup, &process);

    if (!created) {
        code = GetLastError();
        if (code == ERROR_ACCESS_DENIED) {
            created = CreateProcessW(
                application.empty() ? nullptr : application.c_str(),
                buffer.data(), nullptr, nullptr, FALSE, flags, nullptr,
                nullptr, &startup, &process);
        }
    }

    if (!created) {
        code = GetLastError();
        error = format_win32_error(code);
        return false;
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

std::string file_extension(const std::string& target) {
    std::string lower = lower_ascii(target);
    size_t separator = lower.find_last_of("\\/");
    size_t dot = lower.find_last_of('.');
    if (dot == std::string::npos ||
        (separator != std::string::npos && dot < separator)) {
        return {};
    }
    return lower.substr(dot);
}

LaunchResult launch_native(const std::wstring& target,
                           const std::wstring& arguments,
                           std::string& error,
                           DWORD& code) {
    std::string target_utf8 = wide_to_utf8(target);
    if (target_utf8.find("://") != std::string::npos) {
        return LaunchResult::NotSupported;
    }

    std::string extension = file_extension(target_utf8);
    if (extension.empty() || extension == ".exe" || extension == ".com" ||
        extension == ".scr") {
        std::wstring command_line = quote_wide_arg(target);
        if (!arguments.empty()) {
            command_line += L" " + arguments;
        }
        return create_detached_process(L"", command_line, error, code)
                   ? LaunchResult::Success
                   : LaunchResult::Failed;
    }

    if (extension == ".ps1") {
        std::wstring powershell = powershell_path();
        if (!file_exists(powershell)) {
            error = "PowerShell was not found.";
            return LaunchResult::Failed;
        }
        std::wstring command_line =
            quote_wide_arg(powershell) +
            L" -NoLogo -NoProfile -ExecutionPolicy Bypass -File " +
            quote_wide_arg(target);
        if (!arguments.empty()) {
            command_line += L" " + arguments;
        }
        return create_detached_process(powershell, command_line, error, code)
                   ? LaunchResult::Success
                   : LaunchResult::Failed;
    }

    if (extension == ".bat" || extension == ".cmd") {
        std::wstring cmd = cmd_path();
        if (!file_exists(cmd)) {
            error = "cmd.exe was not found.";
            return LaunchResult::Failed;
        }
        std::wstring script = quote_wide_arg(target);
        if (!arguments.empty()) {
            script += L" " + arguments;
        }
        std::wstring command_line =
            quote_wide_arg(cmd) + L" /D /S /C \"" + script + L"\"";
        return create_detached_process(cmd, command_line, error, code)
                   ? LaunchResult::Success
                   : LaunchResult::Failed;
    }

    if (extension == ".vbs" || extension == ".vbe" ||
        extension == ".js" || extension == ".jse") {
        std::wstring wscript = wscript_path();
        if (!file_exists(wscript)) {
            error = "wscript.exe was not found.";
            return LaunchResult::Failed;
        }
        std::wstring command_line =
            quote_wide_arg(wscript) + L" " + quote_wide_arg(target);
        if (!arguments.empty()) {
            command_line += L" " + arguments;
        }
        return create_detached_process(wscript, command_line, error, code)
                   ? LaunchResult::Success
                   : LaunchResult::Failed;
    }

    return LaunchResult::NotSupported;
}

bool launch_shell(const std::wstring& target,
                  const std::wstring& arguments,
                  std::string& error,
                  DWORD& code) {
    HRESULT com_result = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"open";
    info.lpFile = target.c_str();
    if (!arguments.empty()) {
        info.lpParameters = arguments.c_str();
    }
    info.nShow = SW_SHOWNORMAL;

    BOOL launched = ShellExecuteExW(&info);
    code = GetLastError();
    if (SUCCEEDED(com_result)) CoUninitialize();

    if (!launched) {
        error = format_win32_error(code);
        return false;
    }

    if (info.hProcess) CloseHandle(info.hProcess);
    return true;
}

}

static int handle_start(CommandContext& context) {
    std::string tail = rest_after_first_token(context.raw_line);

    std::string target;
    std::string arguments;
    if (!split_start_command(tail, target, arguments)) {
        print(context.out, start_usage(), CLR_BrightYellow);
        return ERR_TOO_FEW_ARGUMENTS;
    }

    std::wstring target_wide = utf8_to_wide(target);
    std::wstring arguments_wide = utf8_to_wide(arguments);

    std::string error;
    DWORD code = 0;
    LaunchResult native =
        launch_native(target_wide, arguments_wide, error, code);
    if (native == LaunchResult::Success) return ERR_NONE;
    if (native == LaunchResult::Failed) {
        print(context.out,
              "Failed to start '" + target + "': " + error + "\n",
              CLR_BrightRed);
        return start_error_code(code);
    }

    if (!launch_shell(target_wide, arguments_wide, error, code)) {
        print(context.out,
              "Failed to start '" + target + "': " + error + "\n",
              CLR_BrightRed);
        return start_error_code(code);
    }

    return ERR_NONE;
}

static CommandSchema make_start_schema() {
    CommandSchema schema;
    schema.raw_tail = true;
    schema.min_positionals = 1;
    schema.max_positionals = 1;
    return schema;
}

static const CommandRegistration register_start_command(
    "start",
    "Start a program, script, or document through Windows APIs.",
    make_start_schema(), handle_start, 0,
    "Usage: start <program|script|document> [arguments...]\n"
    "Executables and known scripts are launched directly with "
    "CreateProcessW; documents and URLs use ShellExecuteExW as a fallback. "
    "The process is launched detached and the command returns immediately.");
