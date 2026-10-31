#include "global.h"

namespace {

bool file_exists(const std::wstring& path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool is_sct_window(HWND window) {
    wchar_t class_name[64]{};
    if (!GetClassNameW(window, class_name,
                       static_cast<int>(sizeof(class_name) /
                                        sizeof(class_name[0])))) {
        return false;
    }
    return wcscmp(class_name, L"SCTTerminalWindow") == 0;
}

bool contains_process_id(const std::vector<DWORD>& process_ids,
                         DWORD process_id) {
    for (DWORD value : process_ids) {
        if (value == process_id) return true;
    }
    return false;
}

BOOL CALLBACK collect_sct_window(HWND window, LPARAM parameter) {
    if (!is_sct_window(window)) return TRUE;

    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id == 0) return TRUE;

    auto* process_ids =
        reinterpret_cast<std::vector<DWORD>*>(parameter);
    if (!contains_process_id(*process_ids, process_id)) {
        process_ids->push_back(process_id);
    }
    return TRUE;
}

struct NewSctWindowSearch {
    const std::vector<DWORD>* existing = nullptr;
    DWORD found = 0;
};

BOOL CALLBACK find_new_sct_window(HWND window, LPARAM parameter) {
    if (!is_sct_window(window) || !IsWindowVisible(window)) return TRUE;

    auto* search = reinterpret_cast<NewSctWindowSearch*>(parameter);
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id == 0 || process_id == GetCurrentProcessId()) return TRUE;
    if (contains_process_id(*search->existing, process_id)) return TRUE;

    search->found = process_id;
    return FALSE;
}

std::wstring nsudo_launcher_path(const std::string& executable) {
    size_t separator = executable.find_last_of("\\/");
    std::string directory = separator == std::string::npos
                                ? "."
                                : executable.substr(0, separator);
#ifdef _WIN64
    std::string path = directory + "\\NSudo\\x64\\NSudo64.exe";
#else
    std::string path = directory + "\\NSudo\\Win32\\NSudo32.exe";
#endif
    return utf8_to_wide(path);
}

bool shell_execute_process(const std::wstring& file,
                           const std::wstring& parameters,
                           const std::wstring& directory,
                           bool elevate,
                           bool show,
                           std::string& error) {
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = elevate ? L"runas" : nullptr;
    info.lpFile = file.c_str();
    info.lpParameters = parameters.c_str();
    info.lpDirectory = directory.c_str();
    info.nShow = show ? SW_SHOWNORMAL : SW_HIDE;

    if (!ShellExecuteExW(&info)) {
        DWORD code = GetLastError();
        if (code == ERROR_CANCELLED) {
            error = "Command cancelled.";
        } else {
            error = "ShellExecuteEx failed: " + format_win32_error(code);
        }
        return false;
    }

    if (info.hProcess) CloseHandle(info.hProcess);
    return true;
}

bool launch_process_direct(const std::wstring& file,
                           const std::wstring& parameters,
                           const std::wstring& directory,
                           std::string& error) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION info{};
    std::wstring command_line = quote_wide_arg(file) + L" " + parameters;

    if (!CreateProcessW(file.c_str(), command_line.data(), nullptr, nullptr,
                        FALSE, CREATE_NO_WINDOW, nullptr, directory.c_str(),
                        &startup, &info)) {
        error = "CreateProcess failed: " + format_win32_error(GetLastError());
        return false;
    }

    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return true;
}

}

std::vector<DWORD> sct_window_process_ids() {
    std::vector<DWORD> process_ids;
    EnumWindows(&collect_sct_window,
                reinterpret_cast<LPARAM>(&process_ids));
    return process_ids;
}

bool wait_for_new_sct_window(const std::vector<DWORD>& existing,
                             DWORD timeout_ms,
                             std::string& error) {
    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (GetTickCount64() < deadline) {
        NewSctWindowSearch search{&existing, 0};
        EnumWindows(&find_new_sct_window,
                    reinterpret_cast<LPARAM>(&search));
        if (search.found != 0) return true;
        Sleep(50);
    }

    error = "The new SCT window did not open.";
    return false;
}

bool is_process_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    BOOL ok = GetTokenInformation(token, TokenElevation, &elevation,
                                  sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

std::wstring get_process_user_sid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return {};
    }

    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        CloseHandle(token);
        return {};
    }

    std::vector<BYTE> buffer(size);
    if (!GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        CloseHandle(token);
        return {};
    }
    CloseHandle(token);

    TOKEN_USER* user = reinterpret_cast<TOKEN_USER*>(buffer.data());
    LPWSTR sid_text = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &sid_text)) {
        return {};
    }

    std::wstring result(sid_text);
    LocalFree(sid_text);
    return result;
}

static bool current_process_has_sid(const std::wstring& sid_text) {
    PSID sid = nullptr;
    if (!ConvertStringSidToSidW(sid_text.c_str(), &sid)) {
        return false;
    }

    BOOL is_member = FALSE;
    BOOL ok = CheckTokenMembership(nullptr, sid, &is_member);
    LocalFree(sid);
    return ok && is_member;
}

PrivilegeLevel get_process_privilege_level() {
    static const std::wstring trusted_installer_sid =
        L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";

    std::wstring sid = get_process_user_sid();

    if (sid == trusted_installer_sid ||
        current_process_has_sid(trusted_installer_sid)) {
        return PrivilegeLevel::TrustedInstaller;
    }
    if (sid == L"S-1-5-18") {
        return PrivilegeLevel::System;
    }
    return is_process_elevated() ? PrivilegeLevel::Administrator
                                 : PrivilegeLevel::User;
}

const char* privilege_level_name(PrivilegeLevel level) {
    switch (level) {
        case PrivilegeLevel::User:
            return "User";
        case PrivilegeLevel::Administrator:
            return "Administrator";
        case PrivilegeLevel::System:
            return "System";
        case PrivilegeLevel::TrustedInstaller:
            return "TrustedInstaller";
    }
    return "User";
}

char privilege_level_tag(PrivilegeLevel level) {
    switch (level) {
        case PrivilegeLevel::User:
            return 'U';
        case PrivilegeLevel::Administrator:
            return 'A';
        case PrivilegeLevel::System:
            return 'S';
        case PrivilegeLevel::TrustedInstaller:
            return 'T';
    }
    return 'U';
}

int privilege_color(PrivilegeLevel level) {
    switch (level) {
        case PrivilegeLevel::User:
            return CLR_BrightYellow;
        case PrivilegeLevel::Administrator:
            return CLR_BrightGreen;
        case PrivilegeLevel::System:
            return CLR_BrightCyan;
        case PrivilegeLevel::TrustedInstaller:
            return CLR_BrightMagenta;
    }
    return CLR_White;
}

bool launch_elevated_self(ElevationMode mode,
                          const std::string& command_line,
                          std::string& error) {
    std::string executable = get_module_path();
    if (executable.empty()) {
        error = "Failed to get executable path.";
        return false;
    }

    std::string cwd = get_current_directory();
    if (cwd.empty()) cwd = ".";

    std::wstring executable_wide = utf8_to_wide(executable);
    std::wstring cwd_wide = utf8_to_wide(cwd);
    std::wstring parameters =
        L"--sct-cwd " + quote_wide_arg(cwd_wide);
    if (!command_line.empty()) {
        parameters += L" --sct-run-command " +
                      quote_wide_arg(utf8_to_wide(command_line));
    }

    if (mode == ElevationMode::Administrator) {
        return shell_execute_process(executable_wide, parameters,
                                     cwd_wide, true, true, error);
    }

    std::wstring nsudo_path = nsudo_launcher_path(executable);
    if (!file_exists(nsudo_path)) {
        error = "NSudo launcher not found: " + wide_to_utf8(nsudo_path);
        return false;
    }

    std::wstring user_option =
        mode == ElevationMode::TrustedInstaller ? L"-U:T" : L"-U:S";
    std::wstring nsudo_parameters =
        user_option + L" -P:E -M:S -ShowWindowMode:Show " +
        quote_wide_arg(executable_wide);
    if (!parameters.empty()) nsudo_parameters += L" " + parameters;

    if (get_process_privilege_level() == PrivilegeLevel::User) {
        return shell_execute_process(nsudo_path, nsudo_parameters,
                                     cwd_wide, true, false, error);
    }
    return launch_process_direct(nsudo_path, nsudo_parameters,
                                 cwd_wide, error);
}

bool relaunch_as_user(std::string& error) {
    if (get_process_privilege_level() == PrivilegeLevel::User) {
        error = "Process already runs as User.";
        return false;
    }

    std::string executable = get_module_path();
    if (executable.empty()) {
        error = "Failed to get executable path.";
        return false;
    }

    std::string cwd = get_current_directory();
    if (cwd.empty()) cwd = ".";
    return shell_execute_process(L"explorer.exe",
                                 quote_wide_arg(utf8_to_wide(executable)),
                                 utf8_to_wide(cwd), false, true, error);
}
