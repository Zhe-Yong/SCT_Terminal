#include "global.h"

namespace {

struct AutoexecLine {
    std::string text;
    size_t number = 0;
    bool require_success = false;
};

bool file_exists(const std::wstring& path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring find_autoexec_path() {
    std::vector<std::wstring> candidates;

    std::string directory = get_module_directory();
    if (!directory.empty()) {
        candidates.push_back(utf8_to_wide(directory) + L"\\autoexec.sct");
    }

    std::string cwd = get_current_directory();
    if (!cwd.empty()) {
        candidates.push_back(utf8_to_wide(cwd) + L"\\autoexec.sct");
    }

    for (const std::wstring& candidate : candidates) {
        if (file_exists(candidate)) return candidate;
    }
    return {};
}

bool read_file(const std::wstring& path, std::string& content, DWORD& error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) {
        error = GetLastError();
        CloseHandle(file);
        return false;
    }
    if (size.QuadPart < 0 || size.QuadPart > 16ll * 1024ll * 1024ll) {
        error = ERROR_NOT_ENOUGH_MEMORY;
        CloseHandle(file);
        return false;
    }

    content.assign(static_cast<size_t>(size.QuadPart), '\0');
    size_t offset = 0;
    while (offset < content.size()) {
        DWORD read = 0;
        DWORD remaining = static_cast<DWORD>(content.size() - offset);
        if (!ReadFile(file, content.data() + offset, remaining, &read,
                      nullptr) ||
            read == 0) {
            error = GetLastError();
            if (error == ERROR_SUCCESS) error = ERROR_READ_FAULT;
            CloseHandle(file);
            return false;
        }
        offset += read;
    }

    CloseHandle(file);
    error = ERROR_SUCCESS;
    return true;
}

void trim(std::string& text) {
    size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) {
        text.clear();
        return;
    }
    size_t last = text.find_last_not_of(" \t\r");
    text = text.substr(first, last - first + 1);
}

bool is_enable_option(const std::string& token) {
    if (token.size() < 2 || (token[0] != '-' && token[0] != '/')) return false;
    size_t prefix = token[0] == '-' && token.size() > 1 && token[1] == '-' ? 2 : 1;
    std::string rest = token.substr(prefix);
    size_t equals = rest.find('=');
    std::string name = lower_ascii(rest.substr(0, equals));
    return name == "enable";
}

bool tokens_attempt_plugin_enable(const std::vector<std::string>& tokens,
                                  int depth) {
    if (tokens.empty() || depth > 8) return false;

    std::string command = lower_ascii(tokens[0]);
    if (command == "sudo") {
        if (tokens.size() < 2) return false;
        std::string subcommand = lower_ascii(tokens[1]);
        if (subcommand == "ad" || subcommand == "sys" || subcommand == "su" ||
            subcommand == "exit") {
            return false;
        }
        std::vector<std::string> nested(tokens.begin() + 1, tokens.end());
        return tokens_attempt_plugin_enable(nested, depth + 1);
    }

    if (command != "plugin") return false;

    std::vector<std::string> arguments(tokens.begin() + 1, tokens.end());
    CommandArguments parsed;
    int error_code = ERR_NONE;
    std::string error;
    if (CommandArguments::parse(plugin_command_schema(), arguments, parsed,
                                error_code, error)) {
        if (parsed.has("enable")) return true;
    }

    for (const std::string& token : arguments) {
        if (token == "--") break;
        if (is_enable_option(token)) return true;
    }
    return false;
}

bool line_attempts_plugin_enable(const std::string& line) {
    return tokens_attempt_plugin_enable(split_command_line(line), 0);
}

}

AutoexecResult run_autoexec_file() {
    AutoexecResult result;
    std::wstring path = find_autoexec_path();
    if (path.empty()) return result;

    std::string content;
    DWORD error = ERROR_SUCCESS;
    if (!read_file(path, content, error)) {
        print("autoexec.sct: failed to read " + wide_to_utf8(path) +
                  ": " + format_win32_error(error) + "\n",
              CLR_BrightYellow);
        print_rst();
        return result;
    }

    if (content.size() >= 3 &&
        static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        content.erase(0, 3);
    }

    std::vector<AutoexecLine> lines;
    size_t position = 0;
    size_t line_number = 0;
    while (position <= content.size()) {
        size_t end = content.find('\n', position);
        std::string line = end == std::string::npos
                               ? content.substr(position)
                               : content.substr(position, end - position);
        position = end == std::string::npos ? content.size() + 1 : end + 1;
        ++line_number;

        trim(line);
        if (line.empty()) continue;

        bool require_success = false;
        if (line.size() >= 4 &&
            lower_ascii(line.substr(0, 4)) == "ens:") {
            require_success = true;
            line = line.substr(4);
            trim(line);
        }
        if (line.empty()) continue;

        if (line_attempts_plugin_enable(line)) {
            result.rejected = true;
            result.error_line = line_number;
            result.message = "Line " + std::to_string(line_number) +
                             ": plugin -enable is not allowed in autoexec.sct.";
            return result;
        }

        lines.push_back(AutoexecLine{line, line_number, require_success});
    }

    for (const AutoexecLine& line : lines) {
        CommandContext context{std::cout};
        context.origin = CommandOrigin::Autoexec;
        int exit_code = execute_command_line(command_registry(), line.text,
                                             context);
        print_rst();

        if (context.exit_requested) {
            result.exit_requested = true;
            return result;
        }

        if (line.require_success && exit_code != ERR_NONE) {
            print("autoexec.sct: required command failed on line " +
                      std::to_string(line.number) + ": " + line.text + "\n",
                  CLR_BrightRed);
            print_rst();
            result.exit_code = exit_code;
            return result;
        }
    }

    return result;
}
