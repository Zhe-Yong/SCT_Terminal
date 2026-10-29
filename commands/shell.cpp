#include "global.h"

namespace {

struct ShellInvocation {
    bool hide = false;
    bool use_cmd = false;
    std::string command;
};

struct Token {
    std::string text;
    size_t start = 0;
    bool quoted = false;
};

struct ChildGuard {
    HANDLE job = nullptr;
    bool job_assigned = false;
    HANDLE process = nullptr;
    HANDLE thread = nullptr;
    HANDLE output = nullptr;

    ~ChildGuard() {
        if (job && job_assigned) {
            TerminateJobObject(job, 0);
        } else if (process) {
            DWORD exit_code = 0;
            if (GetExitCodeProcess(process, &exit_code) &&
                exit_code == STILL_ACTIVE) {
                TerminateProcess(process, 0);
            }
        }
        if (job) CloseHandle(job);
        if (thread) CloseHandle(thread);
        if (process) CloseHandle(process);
        if (output != nullptr && output != INVALID_HANDLE_VALUE) {
            CloseHandle(output);
        }
    }
};

bool is_space(char ch) {
    return ch == ' ' || ch == '\t';
}

bool read_token(const std::string& text, size_t& position, Token& token) {
    while (position < text.size() && is_space(text[position])) {
        ++position;
    }
    if (position >= text.size()) return false;

    token = Token{};
    token.start = position;
    bool in_quotes = false;

    while (position < text.size()) {
        char ch = text[position];
        if (ch == '"') {
            in_quotes = !in_quotes;
            token.quoted = true;
            ++position;
            continue;
        }
        if (!in_quotes && is_space(ch)) break;
        token.text.push_back(ch);
        ++position;
    }

    return true;
}

bool is_option_token(const std::string& token) {
    return token.size() > 1 && (token[0] == '-' || token[0] == '/');
}

std::string option_name(const std::string& token) {
    size_t prefix = 1;
    if (token.size() >= 2 && token[0] == '-' && token[1] == '-') {
        prefix = 2;
    }
    return lower_ascii(token.substr(prefix));
}

std::string shell_usage() {
    return "Usage: shell [-hide|-h] [-window|-w] [-c|-p] <command>\n";
}

std::wstring get_system_directory() {
    std::vector<wchar_t> buffer(32768, L'\0');
    UINT length = GetSystemDirectoryW(buffer.data(),
                                      static_cast<UINT>(buffer.size()));
    if (length == 0 || static_cast<size_t>(length) >= buffer.size()) {
        return {};
    }
    return std::wstring(buffer.data(), length);
}

bool file_exists(const std::wstring& path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring cmd_path() {
    std::wstring system = get_system_directory();
    if (!system.empty()) {
        std::wstring path = system + L"\\cmd.exe";
        if (file_exists(path)) return path;
    }
    return L"cmd.exe";
}

std::wstring powershell_path() {
    std::wstring system = get_system_directory();
    if (!system.empty()) {
        std::wstring path =
            system + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
        if (file_exists(path)) return path;
    }
    return L"powershell.exe";
}

std::string oem_to_utf8(const std::string& input) {
    if (input.empty()) return {};

    UINT code_page = GetOEMCP();
    int wide_length = MultiByteToWideChar(
        code_page, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (wide_length <= 0) return input;

    std::wstring wide(wide_length, L'\0');
    MultiByteToWideChar(code_page, 0, input.data(),
                        static_cast<int>(input.size()), wide.data(),
                        wide_length);
    return wide_to_utf8(wide);
}

int error_code_from_win32(DWORD code) {
    switch (code) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return ERR_FILE_NOT_FOUND;
        case ERROR_ACCESS_DENIED:
            return ERR_PERMISSION_DENIED;
        default:
            return ERR_UNKNOWN;
    }
}

int handle_shell(CommandContext& context) {
    std::string tail = rest_after_first_token(context.raw_line);
    ShellInvocation invocation;
    invocation.hide = terminal_gui_active();

    size_t position = 0;
    bool options_ended = false;

    while (true) {
        Token token;
        if (!read_token(tail, position, token)) break;

        if (!options_ended && !token.quoted && token.text == "--") {
            options_ended = true;
            continue;
        }

        if (!options_ended && !token.quoted &&
            is_option_token(token.text)) {
            std::string name = option_name(token.text);
            if (name == "hide" || name == "h") {
                invocation.hide = true;
            } else if (name == "window" || name == "w") {
                invocation.hide = false;
            } else if (name == "c" || name == "cmd") {
                invocation.use_cmd = true;
            } else if (name == "p" || name == "powershell") {
                invocation.use_cmd = false;
            } else {
                print(context.out, "Unknown option: " + token.text + ".\n",
                      CLR_BrightRed);
                return ERR_INVALID_OPTION;
            }
            continue;
        }

        invocation.command = tail.substr(token.start);
        break;
    }

    if (invocation.command.empty()) {
        print(context.out, shell_usage(), CLR_BrightYellow);
        return ERR_TOO_FEW_ARGUMENTS;
    }

    std::wstring command_wide = utf8_to_wide(invocation.command);
    std::wstring executable;
    std::wstring arguments;

    if (invocation.use_cmd) {
        executable = cmd_path();
        arguments = L"/D /S /C \"" + command_wide + L"\"";
    } else {
        executable = powershell_path();
        std::wstring script =
            L"[Console]::OutputEncoding=[System.Text.Encoding]::UTF8; " +
            command_wide;
        arguments = L"-NoLogo -NoProfile -Command " +
                    quote_wide_arg(script);
    }

    std::wstring command_line =
        quote_wide_arg(executable) + L" " + arguments;
    std::vector<wchar_t> command_buffer(command_line.begin(),
                                        command_line.end());
    command_buffer.push_back(L'\0');

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE output_read = nullptr;
    HANDLE output_write = nullptr;
    if (!CreatePipe(&output_read, &output_write, &security, 0)) {
        print(context.out,
              "Failed to create output pipe: " +
                  format_win32_error(GetLastError()) + "\n",
              CLR_BrightRed);
        return ERR_UNKNOWN;
    }
    SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0);

    HANDLE null_input = CreateFileW(
        L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
        OPEN_EXISTING, 0, nullptr);
    if (null_input == INVALID_HANDLE_VALUE) {
        CloseHandle(output_read);
        CloseHandle(output_write);
        print(context.out,
              "Failed to open NUL device: " +
                  format_win32_error(GetLastError()) + "\n",
              CLR_BrightRed);
        return ERR_UNKNOWN;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.hStdInput = null_input;
    startup.hStdOutput = output_write;
    startup.hStdError = output_write;
    startup.wShowWindow = invocation.hide ? SW_HIDE : SW_SHOWNORMAL;

    PROCESS_INFORMATION process{};
    DWORD flags = invocation.hide ? CREATE_NO_WINDOW : CREATE_NEW_CONSOLE;
    if (!CreateProcessW(executable.c_str(), command_buffer.data(), nullptr,
                        nullptr, TRUE, flags, nullptr, nullptr, &startup,
                        &process)) {
        DWORD code = GetLastError();
        CloseHandle(null_input);
        CloseHandle(output_read);
        CloseHandle(output_write);
        print(context.out,
              "Failed to start " + wide_to_utf8(executable) + ": " +
                  format_win32_error(code) + "\n",
              CLR_BrightRed);
        return error_code_from_win32(code);
    }

    CloseHandle(null_input);
    CloseHandle(output_write);

    ChildGuard guard;
    guard.process = process.hProcess;
    guard.thread = process.hThread;
    guard.output = output_read;

    guard.job = CreateJobObjectW(nullptr, nullptr);
    if (guard.job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(guard.job, JobObjectExtendedLimitInformation,
                                &limits, sizeof(limits));
        guard.job_assigned =
            AssignProcessToJobObject(guard.job, process.hProcess) != FALSE;
    }

    print_rst(context.out);

    bool cancelled = false;
    bool capture_as_oem = invocation.use_cmd;
    std::string buffered_output;
    char buffer[4096];

    while (true) {
        if (command_cancelled()) {
            cancelled = true;
            break;
        }

        DWORD available = 0;
        if (!PeekNamedPipe(output_read, nullptr, 0, nullptr, &available,
                           nullptr)) {
            break;
        }

        if (available > 0) {
            DWORD to_read = std::min<DWORD>(
                available, static_cast<DWORD>(sizeof(buffer)));
            DWORD read = 0;
            if (!ReadFile(output_read, buffer, to_read, &read, nullptr) ||
                read == 0) {
                break;
            }
            if (capture_as_oem) {
                buffered_output.append(buffer, read);
            } else {
                context.out.write(buffer, read);
                context.out.flush();
            }
            continue;
        }

        DWORD wait = WaitForSingleObject(process.hProcess, 20);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) break;
    }

    if (cancelled) {
        if (guard.job_assigned) {
            TerminateJobObject(guard.job, 0);
        } else {
            TerminateProcess(process.hProcess, 0);
        }
        WaitForSingleObject(process.hProcess, 5000);
    } else {
        while (true) {
            if (command_cancelled()) {
                cancelled = true;
                if (guard.job_assigned) {
                    TerminateJobObject(guard.job, 0);
                } else {
                    TerminateProcess(process.hProcess, 0);
                }
                WaitForSingleObject(process.hProcess, 5000);
                break;
            }

            DWORD wait = WaitForSingleObject(process.hProcess, 50);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) break;
        }
    }

    if (!cancelled) {
        while (true) {
            DWORD available = 0;
            if (!PeekNamedPipe(output_read, nullptr, 0, nullptr, &available,
                               nullptr) ||
                available == 0) {
                break;
            }

            DWORD to_read = std::min<DWORD>(
                available, static_cast<DWORD>(sizeof(buffer)));
            DWORD read = 0;
            if (!ReadFile(output_read, buffer, to_read, &read, nullptr) ||
                read == 0) {
                break;
            }
            if (capture_as_oem) {
                buffered_output.append(buffer, read);
            } else {
                context.out.write(buffer, read);
                context.out.flush();
            }
        }
    }

    if (!cancelled && capture_as_oem && !buffered_output.empty()) {
        std::string converted = oem_to_utf8(buffered_output);
        context.out.write(converted.data(),
                          static_cast<std::streamsize>(converted.size()));
        context.out.flush();
    }

    int exit_code = ERR_UNKNOWN;
    if (!cancelled) {
        DWORD code = 0;
        if (GetExitCodeProcess(process.hProcess, &code)) {
            exit_code = static_cast<int>(code);
        }
    }

    print_rst(context.out);

    if (cancelled) return ERR_CANCELLED;
    return exit_code;
}

CommandSchema make_shell_schema() {
    CommandSchema schema;
    schema.raw_tail = true;
    schema.min_positionals = 0;
    schema.max_positionals = 1;
    return schema;
}

const CommandRegistration register_shell_command(
    "shell",
    "Run a command with cmd or PowerShell using SCT privileges.",
    make_shell_schema(), handle_shell);

}
