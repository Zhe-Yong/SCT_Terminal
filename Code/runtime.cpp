#include "global.h"

#ifdef SCT_RANDOM_IMAGE
namespace {

bool directory_exists(const std::wstring& path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool ensure_directory(const std::wstring& path) {
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS && directory_exists(path);
}

std::wstring local_app_data_directory() {
    wchar_t buffer[32768]{};
    DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", buffer, static_cast<DWORD>(std::size(buffer)));
    if (length > 0 && length < std::size(buffer)) {
        return std::wstring(buffer, length);
    }

    wchar_t temp[32768]{};
    DWORD temp_length = GetTempPathW(
        static_cast<DWORD>(std::size(temp)), temp);
    if (temp_length > 0 && temp_length < std::size(temp)) {
        return std::wstring(temp, temp_length);
    }
    return {};
}

std::wstring runtime_directory() {
    static const std::wstring directory = [] {
        std::wstring base = local_app_data_directory();
        if (base.empty()) return std::wstring();

        if (base.back() == L'\\') base.pop_back();
        std::wstring sct = base + L"\\SCT";
        if (!ensure_directory(sct)) return std::wstring();

        std::wstring runtime = sct + L"\\runtime";
        if (ensure_directory(runtime)) return runtime;

        std::wstring fallback = base + L"\\SCT-runtime";
        if (ensure_directory(fallback)) return fallback;
        return std::wstring();
    }();
    return directory;
}

void cleanup_old_runtime_files(const std::wstring& directory,
                               const std::wstring& current_path) {
    std::wstring pattern = directory + L"\\*.exe";
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return;

    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        std::wstring path = directory + L"\\" + data.cFileName;
        if (_wcsicmp(path.c_str(), current_path.c_str()) == 0) continue;
        DeleteFileW(path.c_str());
    } while (FindNextFileW(find, &data));

    FindClose(find);
}

bool copy_self_to_runtime(std::wstring& runtime_path, std::string& error) {
    std::string module_path = get_module_path();
    if (module_path.empty()) {
        error = "Unable to locate the current executable.";
        return false;
    }

    std::wstring directory = runtime_directory();
    if (directory.empty()) {
        error = "Unable to create the runtime directory.";
        return false;
    }

    std::wstring source = utf8_to_wide(module_path);
    cleanup_old_runtime_files(directory, source);
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::wstring name = utf8_to_wide(get_random_identifier(16)) + L".exe";
        std::wstring destination = directory + L"\\" + name;
        if (GetFileAttributesW(destination.c_str()) != INVALID_FILE_ATTRIBUTES) {
            continue;
        }
        if (CopyFileW(source.c_str(), destination.c_str(), TRUE)) {
            runtime_path = destination;
            return true;
        }

        DWORD code = GetLastError();
        if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS) {
            continue;
        }
        error = "Unable to copy the runtime executable: " +
                format_win32_error(code);
        return false;
    }

    error = "Unable to choose a runtime executable name.";
    return false;
}

std::wstring child_command_line(const std::wstring& executable,
                                const std::vector<std::string>& args) {
    std::wstring command = quote_wide_arg(executable);
    for (size_t index = 1; index < args.size(); ++index) {
        command += L" ";
        command += quote_wide_arg(utf8_to_wide(args[index]));
    }
    return command;
}

bool launch_child(const std::wstring& executable,
                  const std::vector<std::string>& args,
                  std::string& error) {
    std::wstring command = child_command_line(executable, args);
    std::wstring working_directory = utf8_to_wide(get_current_directory());

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};

    DWORD flags = CREATE_BREAKAWAY_FROM_JOB;
    BOOL created = CreateProcessW(
        executable.c_str(), command.data(), nullptr, nullptr, FALSE, flags,
        nullptr, working_directory.empty() ? nullptr : working_directory.c_str(),
        &startup, &process);
    if (!created && GetLastError() == ERROR_ACCESS_DENIED) {
        created = CreateProcessW(
            executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
            nullptr,
            working_directory.empty() ? nullptr : working_directory.c_str(),
            &startup, &process);
    }
    if (!created) {
        error = "Unable to launch the runtime executable: " +
                format_win32_error(GetLastError());
        return false;
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

}
#endif

RuntimeBootstrapResult bootstrap_random_runtime(
    const std::vector<std::string>& args, std::string& error) {
#ifdef SCT_RANDOM_IMAGE
    wchar_t marker[8]{};
    DWORD marker_length = GetEnvironmentVariableW(
        L"SCT_RANDOM_CHILD", marker, static_cast<DWORD>(std::size(marker)));
    if (marker_length > 0) return RuntimeBootstrapResult::NotNeeded;

    std::wstring runtime_path;
    if (!copy_self_to_runtime(runtime_path, error)) {
        return RuntimeBootstrapResult::Failed;
    }

    std::string resource_directory = get_resource_directory();
    if (resource_directory.empty()) resource_directory = get_module_directory();
    std::wstring resource_directory_wide = utf8_to_wide(resource_directory);

    SetEnvironmentVariableW(L"SCT_RANDOM_CHILD", L"1");
    if (!resource_directory_wide.empty()) {
        SetEnvironmentVariableW(L"SCT_RESOURCE_DIR",
                                resource_directory_wide.c_str());
    }

    if (!launch_child(runtime_path, args, error)) {
        SetEnvironmentVariableW(L"SCT_RANDOM_CHILD", nullptr);
        return RuntimeBootstrapResult::Failed;
    }
    return RuntimeBootstrapResult::Launched;
#else
    (void)args;
    (void)error;
    return RuntimeBootstrapResult::NotNeeded;
#endif
}
