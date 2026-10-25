#include "global.h"

std::string unix2time(time_t unix_time) {
    std::time_t t = unix_time;
    std::tm* tm_ptr = std::localtime(&t);
    char buffer[20];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tm_ptr);
    return std::string(buffer);
}

std::string get_uuid_v4() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;

    uint64_t a = dis(gen);
    uint64_t b = dis(gen);

    // 设置版本号 (v4) 和 variant 位
    a = (a & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    b = (b & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    std::ostringstream oss;
    oss << std::hex << std::setfill('0')
        << std::setw(8)  << (uint32_t)(a >> 32) << '-'
        << std::setw(4)  << (uint16_t)(a >> 16) << '-'
        << std::setw(4)  << (uint16_t)(a)       << '-'
        << std::setw(4)  << (uint16_t)(b >> 48) << '-'
        << std::setw(12) << (b & 0xFFFFFFFFFFFFULL);
    return oss.str();
}

std::string wide_to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(),
                                   nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(),
                        result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(),
                                   nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(),
                        result.data(), size);
    return result;
}

std::wstring quote_wide_arg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
        return arg;
    }

    std::wstring result = L"\"";
    size_t backslashes = 0;
    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(ch);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::string get_current_directory() {
    DWORD needed = GetCurrentDirectoryW(0, nullptr);
    if (needed == 0) return {};
    std::wstring buffer(needed, L'\0');
    DWORD length = GetCurrentDirectoryW(needed, buffer.data());
    if (length == 0 || length >= needed) return {};
    buffer.resize(length);
    return wide_to_utf8(buffer);
}

bool set_current_directory(const std::string& path) {
    return SetCurrentDirectoryW(utf8_to_wide(path).c_str()) != FALSE;
}

std::string get_module_path() {
    std::vector<wchar_t> buffer(32768, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(), (DWORD)buffer.size());
    if (length == 0) return {};
    return wide_to_utf8(std::wstring(buffer.data(), length));
}

std::string get_current_user_name() {
    wchar_t buffer[256];
    DWORD size = (DWORD)std::size(buffer);
    if (!GetUserNameW(buffer, &size)) return {};
    if (size > 0) --size;
    return wide_to_utf8(std::wstring(buffer, size));
}

std::string format_win32_error(DWORD code) {
    wchar_t* buffer = nullptr;
    DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    if (length == 0 || buffer == nullptr) {
        return "error " + std::to_string(code);
    }

    std::wstring message(buffer, length);
    LocalFree(buffer);
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n' ||
            message.back() == L' ' || message.back() == L'\t')) {
        message.pop_back();
    }
    return wide_to_utf8(message);
}

std::vector<std::string> get_command_line_args() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return {};
    std::vector<std::string> result;
    result.reserve(argc);
    for (int i = 0; i < argc; ++i) result.push_back(wide_to_utf8(argv[i]));
    LocalFree(argv);
    return result;
}

bool read_line_utf8(std::string& line) {
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (input != INVALID_HANDLE_VALUE && GetConsoleMode(input, &mode)) {
        std::wstring wide_line;
        wchar_t buffer[256];
        while (true) {
            DWORD read = 0;
            if (!ReadConsoleW(input, buffer, (DWORD)std::size(buffer), &read, nullptr)) {
                return false;
            }
            if (read == 0) return false;
            wide_line.append(buffer, read);
            if (wide_line.back() == L'\n') {
                wide_line.pop_back();
                if (!wide_line.empty() && wide_line.back() == L'\r') {
                    wide_line.pop_back();
                }
                break;
            }
        }
        line = wide_to_utf8(wide_line);
        return true;
    }
    if (!std::getline(std::cin, line)) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return true;
}

void setup_console() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (output != INVALID_HANDLE_VALUE && GetConsoleMode(output, &mode)) {
        SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    SetConsoleTitleW(L"SuperCommandShell");
}