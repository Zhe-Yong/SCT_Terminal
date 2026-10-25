#include "global.h"

#ifndef PIPE_REJECT_REMOTE_CLIENTS
#define PIPE_REJECT_REMOTE_CLIENTS 0x00000008
#endif

static bool generate_session_key(std::vector<BYTE>& key) {
    key.resize(32);
    NTSTATUS status = BCryptGenRandom(
        nullptr, key.data(), static_cast<ULONG>(key.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(status)) {
        key.clear();
        return false;
    }
    return true;
}

static bool hmac_sha256(const std::vector<BYTE>& key,
                        const std::vector<BYTE>& data,
                        std::vector<BYTE>& digest) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
        BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!BCRYPT_SUCCESS(status)) return false;

    DWORD object_length = 0;
    DWORD property_size = 0;
    status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                               reinterpret_cast<PUCHAR>(&object_length),
                               sizeof(object_length), &property_size, 0);
    if (!BCRYPT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }

    std::vector<BYTE> hash_object(object_length);
    BCRYPT_HASH_HANDLE hash = nullptr;
    status = BCryptCreateHash(algorithm, &hash, hash_object.data(),
                              object_length,
                              const_cast<PUCHAR>(key.data()),
                              static_cast<ULONG>(key.size()), 0);
    if (!BCRYPT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }

    status = BCryptHashData(hash, const_cast<PUCHAR>(data.data()),
                            static_cast<ULONG>(data.size()), 0);
    if (BCRYPT_SUCCESS(status)) {
        digest.resize(32);
        status = BCryptFinishHash(hash, digest.data(),
                                  static_cast<ULONG>(digest.size()), 0);
    }

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);

    if (!BCRYPT_SUCCESS(status)) {
        digest.clear();
        return false;
    }
    return digest.size() == 32;
}

static bool constant_time_equal(const BYTE* left,
                                const BYTE* right,
                                size_t length) {
    unsigned char difference = 0;
    for (size_t index = 0; index < length; ++index) {
        difference |= static_cast<unsigned char>(left[index] ^ right[index]);
    }
    return difference == 0;
}

static void encode_sequence(uint64_t value, BYTE output[8]) {
    for (int index = 0; index < 8; ++index) {
        output[index] = static_cast<BYTE>(value >> (56 - index * 8));
    }
}

static uint64_t decode_sequence(const BYTE* data) {
    uint64_t value = 0;
    for (int index = 0; index < 8; ++index) {
        value = (value << 8) | data[index];
    }
    return value;
}

static bool compute_command_tag(const std::vector<BYTE>& key,
                                uint64_t sequence,
                                const std::string& command,
                                std::vector<BYTE>& digest) {
    std::vector<BYTE> message(8 + command.size());
    encode_sequence(sequence, message.data());
    if (!command.empty()) {
        std::memcpy(message.data() + 8, command.data(), command.size());
    }
    return hmac_sha256(key, message, digest);
}

static bool verify_pipe_client(HANDLE pipe, DWORD expected_pid) {
    ULONG client_pid = 0;
    if (!GetNamedPipeClientProcessId(pipe, &client_pid)) return false;
    return static_cast<DWORD>(client_pid) == expected_pid;
}

static bool verify_pipe_server(HANDLE pipe, DWORD expected_pid) {
    ULONG server_pid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &server_pid)) return false;
    return static_cast<DWORD>(server_pid) == expected_pid;
}

static std::wstring get_process_user_sid();

static DWORD get_process_parent_pid(DWORD process_id) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD parent_pid = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == process_id) {
                parent_pid = entry.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return parent_pid;
}

static bool file_exists(const std::wstring& path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static PrivilegeLevel elevation_mode_level(ElevationMode mode) {
    switch (mode) {
        case ElevationMode::Administrator:
            return PrivilegeLevel::Administrator;
        case ElevationMode::System:
            return PrivilegeLevel::System;
        case ElevationMode::TrustedInstaller:
            return PrivilegeLevel::TrustedInstaller;
    }
    return PrivilegeLevel::Administrator;
}

static std::wstring nsudo_launcher_path(const std::string& executable) {
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

static PSECURITY_DESCRIPTOR create_pipe_security_descriptor() {
    std::wstring current_sid = get_process_user_sid();
    if (current_sid.empty()) current_sid = L"OW";

    std::wstring sddl =
        L"D:(A;;GA;;;SY)"
        L"(A;;GA;;;BA)"
        L"(A;;GA;;;S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464)"
        L"(A;;GA;;;" + current_sid + L")";

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
        return nullptr;
    }
    return descriptor;
}

static bool shell_execute(const std::wstring& file,
                          const std::wstring& parameters,
                          const std::wstring& directory,
                          bool elevate,
                          HANDLE& process,
                          std::string& error) {
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = elevate ? L"runas" : nullptr;
    info.lpFile = file.c_str();
    info.lpParameters = parameters.c_str();
    info.lpDirectory = directory.c_str();
    info.nShow = SW_HIDE;

    if (!ShellExecuteExW(&info)) {
        DWORD code = GetLastError();
        if (code == ERROR_CANCELLED) {
            error = "Elevation was cancelled.";
        } else {
            error = "ShellExecuteEx failed: " + format_win32_error(code);
        }
        return false;
    }

    if (!info.hProcess) {
        error = "Failed to obtain process handle.";
        return false;
    }

    process = info.hProcess;
    return true;
}

static bool launch_process_direct(const std::wstring& file,
                                  const std::wstring& parameters,
                                  const std::wstring& directory,
                                  HANDLE& process,
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
    process = info.hProcess;
    return true;
}

static bool write_all(HANDLE pipe, const void* data, DWORD length) {
    const char* cursor = static_cast<const char*>(data);
    while (length > 0) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) return false;

        DWORD written = 0;
        BOOL ok = WriteFile(pipe, cursor, length, &written, &overlapped);
        if (!ok) {
            DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING) {
                CloseHandle(overlapped.hEvent);
                return false;
            }
            if (!GetOverlappedResult(pipe, &overlapped, &written, TRUE)) {
                CloseHandle(overlapped.hEvent);
                return false;
            }
        }

        CloseHandle(overlapped.hEvent);
        if (written == 0) return false;
        cursor += written;
        length -= written;
    }
    return true;
}

static bool read_all(HANDLE pipe, void* data, DWORD length) {
    char* cursor = static_cast<char*>(data);
    while (length > 0) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) return false;

        DWORD read = 0;
        BOOL ok = ReadFile(pipe, cursor, length, &read, &overlapped);
        if (!ok) {
            DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING) {
                CloseHandle(overlapped.hEvent);
                return false;
            }
            if (!GetOverlappedResult(pipe, &overlapped, &read, TRUE)) {
                CloseHandle(overlapped.hEvent);
                return false;
            }
        }

        CloseHandle(overlapped.hEvent);
        if (read == 0) return false;
        cursor += read;
        length -= read;
    }
    return true;
}

static bool write_frame(HANDLE pipe, const std::string& data) {
    if (data.size() > 64ull * 1024ull * 1024ull) return false;
    uint32_t length = static_cast<uint32_t>(data.size());
    if (!write_all(pipe, &length, sizeof(length))) return false;
    if (length > 0 && !write_all(pipe, data.data(), length)) return false;
    return true;
}

static bool read_frame(HANDLE pipe, std::string& data) {
    uint32_t length = 0;
    if (!read_all(pipe, &length, sizeof(length))) return false;
    if (length > 64u * 1024u * 1024u) return false;

    data.resize(length);
    if (length > 0 && !read_all(pipe, data.data(), length)) return false;
    return true;
}

static bool write_response(HANDLE pipe,
                           int exit_code,
                           PrivilegeLevel level,
                           const std::string& cwd,
                           const std::string& output) {
    uint32_t code = static_cast<uint32_t>(exit_code);
    uint32_t cwd_length = static_cast<uint32_t>(cwd.size());
    uint32_t level_value = static_cast<uint32_t>(level);
    uint32_t output_length = static_cast<uint32_t>(output.size());

    std::string frame;
    frame.reserve(16 + cwd.size() + output.size());
    frame.append(reinterpret_cast<const char*>(&code), sizeof(code));
    frame.append(reinterpret_cast<const char*>(&cwd_length), sizeof(cwd_length));
    frame.append(reinterpret_cast<const char*>(&level_value),
                 sizeof(level_value));
    frame.append(cwd);
    frame.append(reinterpret_cast<const char*>(&output_length),
                 sizeof(output_length));
    frame.append(output);
    return write_frame(pipe, frame);
}

static bool parse_response(const std::string& frame,
                           ElevatedResponse& response,
                           std::string& error) {
    if (frame.size() < 16) {
        error = "Malformed response from elevated helper.";
        return false;
    }

    uint32_t code = 0;
    uint32_t cwd_length = 0;
    uint32_t level_value = 0;
    uint32_t output_length = 0;
    std::memcpy(&code, frame.data(), sizeof(code));
    std::memcpy(&cwd_length, frame.data() + 4, sizeof(cwd_length));
    std::memcpy(&level_value, frame.data() + 8, sizeof(level_value));

    if (frame.size() < 12ull + cwd_length + 4ull) {
        error = "Malformed response from elevated helper.";
        return false;
    }

    std::memcpy(&output_length, frame.data() + 12 + cwd_length,
                sizeof(output_length));

    if (frame.size() != 12ull + cwd_length + 4ull + output_length) {
        error = "Malformed response from elevated helper.";
        return false;
    }

    response.exit_code = static_cast<int>(static_cast<int32_t>(code));
    response.level = level_value <= static_cast<uint32_t>(
                                         PrivilegeLevel::TrustedInstaller)
                         ? static_cast<PrivilegeLevel>(level_value)
                         : PrivilegeLevel::User;
    response.cwd.assign(frame.data() + 12, cwd_length);
    response.output.assign(frame.data() + 16 + cwd_length, output_length);
    return true;
}

static bool connect_pipe(HANDLE pipe, DWORD timeout_ms) {
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent) return false;

    BOOL ok = ConnectNamedPipe(pipe, &overlapped);
    if (!ok) {
        DWORD error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED) {
            CloseHandle(overlapped.hEvent);
            return true;
        }
        if (error != ERROR_IO_PENDING) {
            CloseHandle(overlapped.hEvent);
            return false;
        }

        DWORD wait = WaitForSingleObject(overlapped.hEvent, timeout_ms);
        if (wait != WAIT_OBJECT_0) {
            CancelIoEx(pipe, &overlapped);
            CloseHandle(overlapped.hEvent);
            return false;
        }

        DWORD transferred = 0;
        if (!GetOverlappedResult(pipe, &overlapped, &transferred, FALSE)) {
            CloseHandle(overlapped.hEvent);
            return false;
        }
    }

    CloseHandle(overlapped.hEvent);
    return true;
}

static HANDLE create_pipe(const std::wstring& name,
                          DWORD direction,
                          LPSECURITY_ATTRIBUTES security) {
    DWORD open_mode = direction | FILE_FLAG_OVERLAPPED;
    DWORD pipe_mode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                      PIPE_REJECT_REMOTE_CLIENTS;
    return CreateNamedPipeW(name.c_str(), open_mode, pipe_mode, 1,
                            65536, 65536, 0, security);
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

static std::wstring get_process_user_sid() {
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

ElevatedSession::~ElevatedSession() {
    stop(false);
}

bool ElevatedSession::start(ElevationMode mode, std::string& error) {
    if (active_) return true;

    PrivilegeLevel current_level = get_process_privilege_level();
    PrivilegeLevel target_level = elevation_mode_level(mode);
    if (current_level >= target_level) {
        error = "Process already has the requested privilege level.";
        return false;
    }

    std::string cwd = get_current_directory();
    if (cwd.empty()) cwd = ".";
    std::string executable = get_module_path();
    if (executable.empty()) {
        error = "Failed to get executable path.";
        return false;
    }

    if (!generate_session_key(session_key_)) {
        error = "Failed to generate session key.";
        return false;
    }

    std::wstring token = utf8_to_wide(get_uuid_v4());
    std::wstring cmd_name = L"\\\\.\\pipe\\SCT_" + token + L"_cmd";
    std::wstring out_name = L"\\\\.\\pipe\\SCT_" + token + L"_out";

    PSECURITY_DESCRIPTOR descriptor = create_pipe_security_descriptor();
    if (!descriptor) {
        error = "Failed to create IPC security descriptor: " +
                format_win32_error(GetLastError());
        session_key_.clear();
        return false;
    }

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.lpSecurityDescriptor = descriptor;
    security.bInheritHandle = FALSE;

    HANDLE cmd_pipe = create_pipe(cmd_name, PIPE_ACCESS_OUTBOUND, &security);
    HANDLE out_pipe = create_pipe(out_name, PIPE_ACCESS_INBOUND, &security);
    LocalFree(descriptor);

    if (cmd_pipe == INVALID_HANDLE_VALUE || out_pipe == INVALID_HANDLE_VALUE) {
        error = "Failed to create IPC pipe: " +
                format_win32_error(GetLastError());
        if (cmd_pipe != INVALID_HANDLE_VALUE) CloseHandle(cmd_pipe);
        if (out_pipe != INVALID_HANDLE_VALUE) CloseHandle(out_pipe);
        session_key_.clear();
        return false;
    }

    std::wstring executable_wide = utf8_to_wide(executable);
    std::wstring cwd_wide = utf8_to_wide(cwd);
    size_t executable_separator = executable.find_last_of("\\/");
    std::string executable_directory =
        executable_separator == std::string::npos
            ? "."
            : executable.substr(0, executable_separator);
    std::wstring executable_directory_wide =
        utf8_to_wide(executable_directory);
    std::wstring helper_parameters =
        L"--sct-elevated-helper " +
        quote_wide_arg(cmd_name) + L" " +
        quote_wide_arg(out_name) + L" " +
        quote_wide_arg(cwd_wide) + L" " +
        std::to_wstring(GetCurrentProcessId());

    HANDLE launched_process = nullptr;

    auto fail = [&](const std::string& message) {
        error = message;
        if (launched_process) {
            TerminateProcess(launched_process, 1);
            WaitForSingleObject(launched_process, 5000);
            CloseHandle(launched_process);
            launched_process = nullptr;
        }
        if (cmd_pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(cmd_pipe);
            cmd_pipe = INVALID_HANDLE_VALUE;
        }
        if (out_pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(out_pipe);
            out_pipe = INVALID_HANDLE_VALUE;
        }
        session_key_.clear();
        return false;
    };

    if (mode == ElevationMode::Administrator) {
        if (!shell_execute(executable_wide, helper_parameters,
                           executable_directory_wide, true,
                           launched_process, error)) {
            return fail(error);
        }
    } else {
        std::wstring nsudo_path = nsudo_launcher_path(executable);
        if (!file_exists(nsudo_path)) {
            return fail("NSudo launcher not found: " +
                        wide_to_utf8(nsudo_path));
        }

        std::wstring user_option =
            mode == ElevationMode::TrustedInstaller ? L"-U:T" : L"-U:S";
        std::wstring nsudo_parameters =
            user_option + L" -P:E -M:S -ShowWindowMode:Hide -Wait " +
            quote_wide_arg(executable_wide) + L" " + helper_parameters;

        size_t separator = nsudo_path.find_last_of(L"\\/");
        std::wstring nsudo_directory =
            separator == std::wstring::npos ? L"."
                                            : nsudo_path.substr(0, separator);

        bool elevate_launcher = current_level == PrivilegeLevel::User;
        bool launched = elevate_launcher
                            ? shell_execute(nsudo_path, nsudo_parameters,
                                            nsudo_directory, true,
                                            launched_process, error)
                            : launch_process_direct(nsudo_path, nsudo_parameters,
                                                    nsudo_directory,
                                                    launched_process, error);
        if (!launched) return fail(error);
    }

    if (!connect_pipe(cmd_pipe, 60000) || !connect_pipe(out_pipe, 60000)) {
        std::string message = "Elevated helper did not connect.";
        if (launched_process) {
            DWORD exit_code = 0;
            if (GetExitCodeProcess(launched_process, &exit_code) &&
                exit_code != STILL_ACTIVE) {
                message += " Launcher exited with code " +
                           std::to_string(exit_code) + ".";
            }
        }
        return fail(message);
    }

    if (mode == ElevationMode::Administrator) {
        DWORD helper_pid = GetProcessId(launched_process);
        if (helper_pid == 0 ||
            !verify_pipe_client(cmd_pipe, helper_pid) ||
            !verify_pipe_client(out_pipe, helper_pid)) {
            return fail("Elevated helper identity verification failed.");
        }
    } else {
        ULONG cmd_client = 0;
        ULONG out_client = 0;
        if (!GetNamedPipeClientProcessId(cmd_pipe, &cmd_client) ||
            !GetNamedPipeClientProcessId(out_pipe, &out_client) ||
            cmd_client != out_client) {
            return fail("Elevated helper identity verification failed.");
        }

        DWORD launcher_pid = GetProcessId(launched_process);
        if (launcher_pid == 0 ||
            get_process_parent_pid(static_cast<DWORD>(cmd_client)) !=
                launcher_pid) {
            return fail("Elevated helper identity verification failed.");
        }
    }

    std::string key_frame(
        reinterpret_cast<const char*>(session_key_.data()),
        session_key_.size());
    if (!write_frame(cmd_pipe, key_frame)) {
        return fail("Failed to send session key to elevated helper.");
    }

    std::string ready_frame;
    if (!read_frame(out_pipe, ready_frame)) {
        return fail("Elevated helper did not complete authentication.");
    }

    ElevatedResponse ready;
    if (!parse_response(ready_frame, ready, error)) {
        return fail(error);
    }

    if (ready.exit_code != ERR_NONE) {
        return fail(ready.output.empty()
                        ? "Elevated helper authentication failed."
                        : ready.output);
    }

    if (ready.level != target_level) {
        return fail("Elevated helper reported unexpected privilege level: " +
                    std::string(privilege_level_name(ready.level)) +
                    " (expected " +
                    std::string(privilege_level_name(target_level)) + ").");
    }

    cmd_pipe_ = cmd_pipe;
    out_pipe_ = out_pipe;
    process_ = launched_process;
    cwd_ = ready.cwd.empty() ? cwd : ready.cwd;
    level_ = ready.level;
    command_sequence_ = 1;
    active_ = true;

    job_ = CreateJobObjectW(nullptr, nullptr);
    if (job_) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job_, JobObjectExtendedLimitInformation,
                                &limits, sizeof(limits));
        AssignProcessToJobObject(job_, process_);
    }

    return true;
}

bool ElevatedSession::send(const std::string& command_line,
                           ElevatedResponse& response,
                           std::string& error) {
    if (!active_) {
        error = "No elevated session is active.";
        return false;
    }

    if (session_key_.empty()) {
        error = "No authenticated session key.";
        return false;
    }

    uint64_t sequence = command_sequence_;
    std::vector<BYTE> digest;
    if (!compute_command_tag(session_key_, sequence, command_line, digest)) {
        error = "Failed to authenticate command.";
        return false;
    }

    BYTE sequence_bytes[8];
    encode_sequence(sequence, sequence_bytes);

    std::string payload;
    payload.reserve(8 + digest.size() + command_line.size());
    payload.append(reinterpret_cast<const char*>(sequence_bytes),
                   sizeof(sequence_bytes));
    payload.append(reinterpret_cast<const char*>(digest.data()), digest.size());
    payload.append(command_line);

    if (!write_frame(cmd_pipe_, payload)) {
        error = "Failed to send command to elevated helper: " +
                format_win32_error(GetLastError());
        return false;
    }
    ++command_sequence_;

    std::string frame;
    if (!read_frame(out_pipe_, frame)) {
        error = "Failed to read response from elevated helper: " +
                format_win32_error(GetLastError());
        return false;
    }

    if (!parse_response(frame, response, error)) return false;

    if (response.exit_code == ERR_AUTH_FAILED) {
        error = "Command authentication failed.";
        return false;
    }

    level_ = response.level;
    if (!response.cwd.empty()) cwd_ = response.cwd;
    return true;
}

bool ElevatedSession::run_once(const std::string& command_line,
                               ElevatedResponse& response,
                               std::string& error) {
    if (active_) return send(command_line, response, error);
    if (!start(ElevationMode::Administrator, error)) return false;

    bool ok = send(command_line, response, error);
    stop(false);
    return ok;
}

void ElevatedSession::stop(bool adopt_cwd) {
    if (!active_) {
        cleanup();
        return;
    }

    if (cmd_pipe_ != INVALID_HANDLE_VALUE) {
        write_frame(cmd_pipe_, std::string());
    }

    if (process_) {
        WaitForSingleObject(process_, 3000);
        DWORD exit_code = 0;
        if (GetExitCodeProcess(process_, &exit_code) &&
            exit_code == STILL_ACTIVE) {
            TerminateProcess(process_, 0);
            WaitForSingleObject(process_, 3000);
        }
    }

    if (adopt_cwd && !cwd_.empty()) {
        set_current_directory(cwd_);
    }
    cleanup();
}

bool ElevatedSession::active() const {
    return active_;
}

std::string ElevatedSession::cwd() const {
    return cwd_;
}

PrivilegeLevel ElevatedSession::level() const {
    return level_;
}

void ElevatedSession::cleanup() {
    if (cmd_pipe_ != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(cmd_pipe_);
        CloseHandle(cmd_pipe_);
        cmd_pipe_ = INVALID_HANDLE_VALUE;
    }
    if (out_pipe_ != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(out_pipe_);
        CloseHandle(out_pipe_);
        out_pipe_ = INVALID_HANDLE_VALUE;
    }
    if (process_) {
        CloseHandle(process_);
        process_ = nullptr;
    }
    if (job_) {
        CloseHandle(job_);
        job_ = nullptr;
    }
    active_ = false;
    cwd_.clear();
    level_ = PrivilegeLevel::User;
    session_key_.clear();
    command_sequence_ = 1;
}

ElevatedSession& elevated_session() {
    static ElevatedSession session;
    return session;
}

int run_elevated_helper(const std::string& cmd_pipe_name,
                        const std::string& out_pipe_name,
                        const std::string& cwd,
                        DWORD parent_pid) {
    DWORD console_processes[4] = {};
    DWORD console_process_count =
        GetConsoleProcessList(console_processes, (DWORD)std::size(console_processes));
    HWND console = GetConsoleWindow();
    if (console && console_process_count == 1) ShowWindow(console, SW_HIDE);
    FreeConsole();

    HANDLE cmd_pipe = CreateFileW(utf8_to_wide(cmd_pipe_name).c_str(),
                                  GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OVERLAPPED, nullptr);
    if (cmd_pipe == INVALID_HANDLE_VALUE) return 1;

    HANDLE out_pipe = CreateFileW(utf8_to_wide(out_pipe_name).c_str(),
                                  GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OVERLAPPED, nullptr);
    if (out_pipe == INVALID_HANDLE_VALUE) {
        CloseHandle(cmd_pipe);
        return 2;
    }

    if (!verify_pipe_server(cmd_pipe, parent_pid) ||
        !verify_pipe_server(out_pipe, parent_pid)) {
        CloseHandle(out_pipe);
        CloseHandle(cmd_pipe);
        return 3;
    }

    std::wstring cwd_wide = utf8_to_wide(cwd);
    if (!cwd_wide.empty()) SetCurrentDirectoryW(cwd_wide.c_str());

    std::string key_frame;
    if (!read_frame(cmd_pipe, key_frame) || key_frame.size() != 32) {
        CloseHandle(out_pipe);
        CloseHandle(cmd_pipe);
        return 4;
    }
    std::vector<BYTE> session_key(key_frame.begin(), key_frame.end());

    CommandRegistry& registry = command_registry();
    PrivilegeLevel helper_level = get_process_privilege_level();
    if (!write_response(out_pipe, ERR_NONE, helper_level,
                        get_current_directory(), "")) {
        CloseHandle(out_pipe);
        CloseHandle(cmd_pipe);
        return 5;
    }
    uint64_t expected_sequence = 1;

    while (true) {
        std::string payload;
        if (!read_frame(cmd_pipe, payload)) break;
        if (payload.empty()) break;

        if (payload.size() < 40) {
            write_response(out_pipe, ERR_AUTH_FAILED, helper_level,
                           get_current_directory(),
                           "Command authentication failed.\n");
            break;
        }

        uint64_t sequence = decode_sequence(
            reinterpret_cast<const BYTE*>(payload.data()));
        const BYTE* received_tag =
            reinterpret_cast<const BYTE*>(payload.data() + 8);
        std::string line = payload.substr(40);

        std::vector<BYTE> digest;
        bool authenticated =
            sequence == expected_sequence &&
            compute_command_tag(session_key, sequence, line, digest) &&
            digest.size() == 32 &&
            constant_time_equal(received_tag, digest.data(), 32);

        if (!authenticated) {
            write_response(out_pipe, ERR_AUTH_FAILED, helper_level,
                           get_current_directory(),
                           "Command authentication failed.\n");
            break;
        }
        ++expected_sequence;

        std::ostringstream buffer;
        CommandContext context{buffer};
        int exit_code = execute_command_line(registry, line, context);
        print_rst(buffer);

        std::string current_cwd = get_current_directory();
        if (!write_response(out_pipe, exit_code, helper_level, current_cwd,
                            buffer.str())) {
            break;
        }

        if (context.exit_requested) break;
    }

    CloseHandle(out_pipe);
    CloseHandle(cmd_pipe);
    return 0;
}
