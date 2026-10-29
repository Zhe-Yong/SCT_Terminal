#include "global.h"

namespace {

struct ProcessInfo {
    DWORD pid = 0;
    DWORD parent = 0;
    std::wstring name;
};

struct WindowSignal {
    DWORD pid = 0;
    int found = 0;
    int sent = 0;
};

BOOL CALLBACK enum_window_callback(HWND window, LPARAM parameter) {
    auto* signal = reinterpret_cast<WindowSignal*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != signal->pid) return TRUE;

    ++signal->found;
    if (PostMessageW(window, WM_CLOSE, 0, 0)) ++signal->sent;
    return TRUE;
}

bool wildcard_match(const std::wstring& pattern, const std::wstring& text) {
    size_t pattern_index = 0;
    size_t text_index = 0;
    size_t star = std::wstring::npos;
    size_t match = 0;

    while (text_index < text.size()) {
        if (pattern_index < pattern.size() &&
            (pattern[pattern_index] == L'?' ||
             std::towlower(pattern[pattern_index]) ==
                 std::towlower(text[text_index]))) {
            ++pattern_index;
            ++text_index;
        } else if (pattern_index < pattern.size() &&
                   pattern[pattern_index] == L'*') {
            star = pattern_index++;
            match = text_index;
        } else if (star != std::wstring::npos) {
            pattern_index = star + 1;
            text_index = ++match;
        } else {
            return false;
        }
    }

    while (pattern_index < pattern.size() &&
           pattern[pattern_index] == L'*') {
        ++pattern_index;
    }
    return pattern_index == pattern.size();
}

std::vector<ProcessInfo> snapshot_processes() {
    std::vector<ProcessInfo> processes;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return processes;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            ProcessInfo process;
            process.pid = entry.th32ProcessID;
            process.parent = entry.th32ParentProcessID;
            process.name = entry.szExeFile;
            processes.push_back(std::move(process));
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return processes;
}

int error_code_from_win32(DWORD code) {
    switch (code) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_PARAMETER:
            return ERR_FILE_NOT_FOUND;
        case ERROR_ACCESS_DENIED:
            return ERR_PERMISSION_DENIED;
        default:
            return ERR_UNKNOWN;
    }
}

std::string process_label(DWORD pid, const std::wstring& name) {
    std::string label = "PID " + std::to_string(pid);
    if (!name.empty()) {
        label += " (" + wide_to_utf8(name) + ")";
    }
    return label;
}

bool send_thread_quit(DWORD pid) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    bool sent = false;

    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID == pid &&
                PostThreadMessageW(entry.th32ThreadID, WM_QUIT, 0, 0)) {
                sent = true;
            }
        } while (Thread32Next(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return sent;
}

int graceful_terminate(CommandContext& context,
                       DWORD pid,
                       const std::wstring& name) {
    WindowSignal signal;
    signal.pid = pid;
    EnumWindows(enum_window_callback, reinterpret_cast<LPARAM>(&signal));

    bool sent = signal.sent > 0 || send_thread_quit(pid);
    if (sent) {
        print(context.out,
              "SUCCESS: Sent termination signal to " +
                  process_label(pid, name) + ".\n",
              CLR_BrightGreen);
        return ERR_NONE;
    }

    print(context.out,
          "ERROR: Could not send termination signal to " +
              process_label(pid, name) +
              ". Use -f to force termination.\n",
          CLR_BrightRed);
    return ERR_UNKNOWN;
}

int force_terminate(CommandContext& context,
                    DWORD pid,
                    const std::wstring& name) {
    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!process) {
        DWORD code = GetLastError();
        print(context.out,
              "ERROR: Could not terminate " + process_label(pid, name) +
                  ". Reason: " + format_win32_error(code) + ".\n",
              CLR_BrightRed);
        return error_code_from_win32(code);
    }

    if (!TerminateProcess(process, 1)) {
        DWORD code = GetLastError();
        CloseHandle(process);
        print(context.out,
              "ERROR: Could not terminate " + process_label(pid, name) +
                  ". Reason: " + format_win32_error(code) + ".\n",
              CLR_BrightRed);
        return error_code_from_win32(code);
    }

    DWORD wait = WaitForSingleObject(process, 5000);
    CloseHandle(process);

    if (wait == WAIT_TIMEOUT || wait == WAIT_FAILED) {
        print(context.out,
              "ERROR: " + process_label(pid, name) +
                  " did not exit after termination.\n",
              CLR_BrightRed);
        return ERR_UNKNOWN;
    }

    print(context.out,
          "SUCCESS: Terminated " + process_label(pid, name) + ".\n",
          CLR_BrightGreen);
    return ERR_NONE;
}

void collect_process_tree(DWORD pid,
                          const std::vector<ProcessInfo>& processes,
                          std::set<DWORD>& seen,
                          std::vector<DWORD>& order) {
    if (!seen.insert(pid).second) return;

    for (const ProcessInfo& process : processes) {
        if (process.parent == pid && process.pid != pid) {
            collect_process_tree(process.pid, processes, seen, order);
        }
    }
    order.push_back(pid);
}

std::string taskkill_usage() {
    return "Usage: taskkill [-f] [-t] (-im <imagename> | -pid <processid>)\n";
}

int handle_taskkill(CommandContext& context) {
    bool force = context.args.has("f") || context.args.has("force");
    bool tree = context.args.has("t") || context.args.has("tree");

    std::optional<std::string> image = context.args.value("im");
    std::optional<std::string> pid_text = context.args.value("pid");

    if (image.has_value() && pid_text.has_value()) {
        print(context.out,
              "ERROR: -im and -pid cannot be used together.\n",
              CLR_BrightRed);
        return ERR_INVALID_OPTION;
    }

    if (!image.has_value() && !pid_text.has_value()) {
        print(context.out, taskkill_usage(), CLR_BrightYellow);
        return ERR_TOO_FEW_ARGUMENTS;
    }

    std::vector<ProcessInfo> processes = snapshot_processes();
    if (processes.empty()) {
        print(context.out,
              "ERROR: Could not enumerate processes: " +
                  format_win32_error(GetLastError()) + ".\n",
              CLR_BrightRed);
        return ERR_UNKNOWN;
    }

    std::map<DWORD, std::wstring> names;
    for (const ProcessInfo& process : processes) {
        names[process.pid] = process.name;
    }

    std::vector<DWORD> targets;

    if (image.has_value()) {
        if (image->empty()) {
            print(context.out, "ERROR: Image name is empty.\n",
                  CLR_BrightRed);
            return ERR_INVALID_OPTION;
        }

        std::wstring pattern = utf8_to_wide(*image);
        for (const ProcessInfo& process : processes) {
            if (process.pid != 0 &&
                wildcard_match(pattern, process.name)) {
                targets.push_back(process.pid);
            }
        }

        if (targets.empty()) {
            print(context.out,
                  "ERROR: The process \"" + *image + "\" not found.\n",
                  CLR_BrightRed);
            return ERR_FILE_NOT_FOUND;
        }
    } else {
        unsigned long long value = 0;
        try {
            size_t consumed = 0;
            value = std::stoull(*pid_text, &consumed);
            if (consumed != pid_text->size() || value == 0 ||
                value > 0xFFFFFFFFull) {
                throw std::invalid_argument("pid");
            }
        } catch (...) {
            print(context.out,
                  "ERROR: Invalid PID: " + *pid_text + ".\n",
                  CLR_BrightRed);
            return ERR_INVALID_OPTION;
        }

        DWORD pid = static_cast<DWORD>(value);
        if (names.find(pid) == names.end()) {
            print(context.out,
                  "ERROR: The process with PID " + std::to_string(pid) +
                      " not found.\n",
                  CLR_BrightRed);
            return ERR_FILE_NOT_FOUND;
        }
        targets.push_back(pid);
    }

    std::set<DWORD> seen;
    std::vector<DWORD> order;

    if (tree) {
        for (DWORD pid : targets) {
            collect_process_tree(pid, processes, seen, order);
        }
    } else {
        std::set<DWORD> unique;
        for (DWORD pid : targets) {
            if (unique.insert(pid).second) order.push_back(pid);
        }
    }

    int first_error = ERR_NONE;
    for (DWORD pid : order) {
        std::wstring name;
        auto found = names.find(pid);
        if (found != names.end()) name = found->second;

        int error = force ? force_terminate(context, pid, name)
                          : graceful_terminate(context, pid, name);
        if (first_error == ERR_NONE && error != ERR_NONE) {
            first_error = error;
        }
    }

    return first_error;
}

CommandSchema make_taskkill_schema() {
    CommandSchema schema;
    schema.options.push_back(CommandOption{"f", OptionValueMode::Flag});
    schema.options.push_back(CommandOption{"force", OptionValueMode::Flag});
    schema.options.push_back(CommandOption{"t", OptionValueMode::Flag});
    schema.options.push_back(CommandOption{"tree", OptionValueMode::Flag});
    schema.options.push_back(CommandOption{"im", OptionValueMode::Required});
    schema.options.push_back(CommandOption{"pid", OptionValueMode::Required});
    schema.max_positionals = 0;
    return schema;
}

const CommandRegistration register_taskkill_command(
    "taskkill",
    "Terminate processes by image name or PID. Options: -f, -t, -im, -pid.",
    make_taskkill_schema(), handle_taskkill);

}
