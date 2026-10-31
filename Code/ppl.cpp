#include "global.h"

namespace {

DWORD ppl_target_pid_override = 0;

std::wstring ppl_dll_name() {
#ifdef _WIN64
    return L"ppl64.dll";
#else
    return L"ppl32.dll";
#endif
}

bool file_exists(const std::wstring& path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring module_directory() {
    std::string module_path = get_module_path();
    if (module_path.empty()) return {};

    size_t separator = module_path.find_last_of("\\/");
    std::string directory =
        separator == std::string::npos ? "." : module_path.substr(0, separator);
    return utf8_to_wide(directory);
}

}

std::string ppl_default_type() {
    const char* value = std::getenv("SCT_PPL_TYPE");
    if (value != nullptr && value[0] != '\0') return value;
    return "WinTcb";
}

std::string ppl_dll_path() {
    std::wstring directory = module_directory();
    if (directory.empty()) return {};

    std::wstring name = ppl_dll_name();
    std::wstring candidates[] = {
        directory + L"\\" + name,
        directory + L"\\ppl\\" + name
    };

    for (const std::wstring& candidate : candidates) {
        if (file_exists(candidate)) return wide_to_utf8(candidate);
    }
    return {};
}

void ppl_set_target_pid(DWORD pid) {
    ppl_target_pid_override = pid;
}

DWORD ppl_target_pid() {
    if (ppl_target_pid_override != 0) return ppl_target_pid_override;
    return GetCurrentProcessId();
}

PplProtectionState ppl_target_protection(std::string& error) {
    DWORD pid = ppl_target_pid();
    HANDLE process = GetCurrentProcess();
    bool close_process = false;

    if (pid != GetCurrentProcessId()) {
        process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) {
            error = "Failed to query PPL protection state: " +
                    format_win32_error(GetLastError());
            return PplProtectionState::Unknown;
        }
        close_process = true;
    }

    PROCESS_PROTECTION_LEVEL_INFORMATION information{};
    BOOL ok = GetProcessInformation(
        process, ProcessProtectionLevelInfo, &information,
        sizeof(information));
    DWORD code = GetLastError();
    if (close_process) CloseHandle(process);

    if (!ok) {
        error = "Failed to query PPL protection state: " +
                format_win32_error(code);
        return PplProtectionState::Unknown;
    }

    error.clear();
    return information.ProtectionLevel == 0
               ? PplProtectionState::Unprotected
               : PplProtectionState::Protected;
}

static bool ppl_call_bool_export(const char* export_name,
                                 const char* failure_message,
                                 std::string& error) {
    std::string path = ppl_dll_path();
    if (path.empty()) {
        error = "PPL DLL not found.";
        return false;
    }

    HMODULE module = LoadLibraryW(utf8_to_wide(path).c_str());
    if (!module) {
        error = "Failed to load PPL DLL: " +
                format_win32_error(GetLastError());
        return false;
    }

    FARPROC proc = GetProcAddress(module, export_name);
    if (!proc) {
        FreeLibrary(module);
        error = std::string(export_name) + " was not found in the PPL DLL.";
        return false;
    }

    using PplBoolExport = BOOL(__cdecl*)();
    PplBoolExport function = nullptr;
    static_assert(sizeof(function) == sizeof(proc));
    std::memcpy(&function, &proc, sizeof(function));

    BOOL result = function();
    FreeLibrary(module);

    if (!result) {
        error = failure_message;
        DWORD code = GetLastError();
        if (code != 0) {
            error += " (" + format_win32_error(code) + ")";
        }
        return false;
    }

    error.clear();
    return true;
}

bool ppl_install_driver(std::string& error) {
    return ppl_call_bool_export("PPLcontrolInstallDriver",
                                "Failed to install or start the PPL driver.",
                                error);
}

static bool wait_for_service_state(SC_HANDLE service,
                                   DWORD desired_state,
                                   DWORD timeout_ms,
                                   DWORD& last_state) {
    DWORD start = GetTickCount();
    while (true) {
        SERVICE_STATUS status{};
        if (!QueryServiceStatus(service, &status)) return false;
        last_state = status.dwCurrentState;
        if (last_state == desired_state) return true;
        if (GetTickCount() - start >= timeout_ms) return false;
        Sleep(100);
    }
}

bool ppl_uninstall_driver(std::string& error) {
#ifdef _WIN64
    const wchar_t* service_name = L"RTCore64";
#else
    const wchar_t* service_name = L"RTCore32";
#endif

    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) {
        error = "Failed to open service manager: " +
                format_win32_error(GetLastError());
        return false;
    }

    SC_HANDLE service = OpenServiceW(
        manager, service_name, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (!service) {
        DWORD code = GetLastError();
        CloseServiceHandle(manager);
        if (code == ERROR_SERVICE_DOES_NOT_EXIST) {
            error.clear();
            return true;
        }
        error = "Failed to open PPL driver service: " +
                format_win32_error(code);
        return false;
    }

    SERVICE_STATUS status{};
    if (!QueryServiceStatus(service, &status)) {
        DWORD code = GetLastError();
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        error = "Failed to query PPL driver state: " +
                format_win32_error(code);
        return false;
    }

    if (status.dwCurrentState != SERVICE_STOPPED) {
        if (!ControlService(service, SERVICE_CONTROL_STOP, &status)) {
            DWORD code = GetLastError();
            if (code != ERROR_SERVICE_NOT_ACTIVE) {
                SERVICE_STATUS current{};
                bool stopped =
                    QueryServiceStatus(service, &current) &&
                    current.dwCurrentState == SERVICE_STOPPED;
                if (!stopped) {
                    CloseServiceHandle(service);
                    CloseServiceHandle(manager);
                    error = "Failed to stop PPL driver: " +
                            format_win32_error(code);
                    return false;
                }
            }
        }

        DWORD last_state = 0;
        if (!wait_for_service_state(service, SERVICE_STOPPED, 15000,
                                    last_state)) {
            DeleteService(service);
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            if (last_state == SERVICE_STOP_PENDING) {
                error = "PPL driver is still stopping; reboot may be "
                        "required.";
            } else {
                error = "PPL driver did not reach the stopped state.";
            }
            return false;
        }
    }

    if (!DeleteService(service)) {
        DWORD code = GetLastError();
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        if (code == ERROR_SERVICE_MARKED_FOR_DELETE) {
            error.clear();
            return true;
        }
        error = "Failed to delete PPL driver service: " +
                format_win32_error(code);
        return false;
    }

    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    error.clear();
    return true;
}

int ppl_run_command(const std::string& command, std::string& error) {
    std::string path = ppl_dll_path();
    if (path.empty()) {
        error = "PPL DLL not found.";
        return 2;
    }

    HMODULE module = LoadLibraryW(utf8_to_wide(path).c_str());
    if (!module) {
        error = "Failed to load PPL DLL: " +
                format_win32_error(GetLastError());
        return 2;
    }

    FARPROC proc = GetProcAddress(module, "PPLcontrolRunCommandLineW");
    if (!proc) {
        FreeLibrary(module);
        error = "PPLcontrolRunCommandLineW was not found in the PPL DLL.";
        return 2;
    }

    using PplRunCommandLineW = int(__cdecl*)(LPCWSTR);
    PplRunCommandLineW run = nullptr;
    static_assert(sizeof(run) == sizeof(proc));
    std::memcpy(&run, &proc, sizeof(run));

    std::wstring wide_command = utf8_to_wide(command);
    int result = run(wide_command.c_str());
    FreeLibrary(module);

    if (result != 0) {
        error = "PPL control returned " + std::to_string(result) + ".";
    } else {
        error.clear();
    }
    return result;
}

bool ppl_protect_target(const std::string& type, std::string& error) {
    std::string protection_type = type.empty() ? ppl_default_type() : type;
    std::string command = "protect " + std::to_string(ppl_target_pid()) +
                          " PPL " + protection_type;
    return ppl_run_command(command, error) == 0;
}

bool ppl_unprotect_target(std::string& error) {
    std::string command = "unprotect " + std::to_string(ppl_target_pid());
    return ppl_run_command(command, error) == 0;
}

bool ppl_run_isolated(DWORD target_pid,
                      const std::string& action,
                      const std::string& type,
                      std::string& error) {
    std::string module_path = get_module_path();
    if (module_path.empty()) {
        error = "Failed to get the SCT executable path.";
        return false;
    }

    std::wstring executable = utf8_to_wide(module_path);
    std::wstring command_line =
        quote_wide_arg(executable) + L" --sct-ppl-helper " +
        std::to_wstring(target_pid) + L" " + utf8_to_wide(action) + L" " +
        quote_wide_arg(utf8_to_wide(type));

    std::vector<wchar_t> command_buffer(command_line.begin(),
                                        command_line.end());
    command_buffer.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command_buffer.data(), nullptr,
                        nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr,
                        &startup, &process)) {
        error = "Failed to start PPL helper: " +
                format_win32_error(GetLastError());
        return false;
    }

    CloseHandle(process.hThread);

    DWORD wait = WaitForSingleObject(process.hProcess, 30000);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 2);
        WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hProcess);
        error = "PPL helper timed out.";
        return false;
    }
    if (wait == WAIT_FAILED) {
        DWORD code = GetLastError();
        CloseHandle(process.hProcess);
        error = "Failed to wait for PPL helper: " +
                format_win32_error(code);
        return false;
    }

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process.hProcess, &exit_code)) {
        DWORD code = GetLastError();
        CloseHandle(process.hProcess);
        error = "Failed to get PPL helper exit code: " +
                format_win32_error(code);
        return false;
    }
    CloseHandle(process.hProcess);

    if (exit_code == 6) {
        error = "PPL driver is still stopping; reboot may be required.";
        return false;
    }
    if (exit_code != 0) {
        if (action == "install") {
            error = "Failed to load or start the PPL driver.";
        } else if (action == "protect") {
            error = "Failed to apply PPL protection.";
        } else if (action == "unprotect") {
            error = "Failed to remove PPL protection.";
        } else if (action == "uninstall") {
            error = "Failed to stop or remove the PPL driver.";
        } else {
            error = "PPL helper failed with exit code " +
                    std::to_string(exit_code) + ".";
        }
        return false;
    }
    error.clear();
    return true;
}
