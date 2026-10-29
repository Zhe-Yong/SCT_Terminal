#include "global.h"

namespace {

constexpr size_t kMaxTreeDepth = 64;

struct DirEntry {
    std::wstring name;
    DWORD attributes = 0;
    uint64_t size = 0;
    FILETIME modified{};
};

struct ListOptions {
    bool all = false;
    bool detail = false;
    bool tree = false;
};

struct TargetSpec {
    std::string display;
    std::wstring full_path;
    std::wstring pattern;
    std::wstring parent_directory;
    bool directory_contents = false;
    bool explicit_file = false;
    bool wildcard = false;
};

struct FindHandle {
    explicit FindHandle(HANDLE value) : handle(value) {}
    ~FindHandle() {
        if (handle != INVALID_HANDLE_VALUE) FindClose(handle);
    }

    FindHandle(const FindHandle&) = delete;
    FindHandle& operator=(const FindHandle&) = delete;

    HANDLE handle = INVALID_HANDLE_VALUE;
};

bool is_directory(DWORD attributes) {
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool is_reparse_point(DWORD attributes) {
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool is_hidden_or_system(DWORD attributes) {
    return (attributes &
            (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
}

bool has_wildcard(const std::wstring& text) {
    return text.find_first_of(L"*?") != std::wstring::npos;
}

bool ends_with_separator(const std::wstring& text) {
    return !text.empty() &&
           (text.back() == L'\\' || text.back() == L'/');
}

std::wstring join_path(const std::wstring& directory,
                       const std::wstring& name) {
    if (directory.empty()) return name;
    if (ends_with_separator(directory)) return directory + name;
    return directory + L'\\' + name;
}

std::wstring get_full_path(const std::wstring& path) {
    DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (needed == 0) return path;

    std::wstring result(needed, L'\0');
    DWORD length = GetFullPathNameW(path.c_str(), needed, result.data(), nullptr);
    if (length == 0 || length >= needed) return path;
    result.resize(length);
    return result;
}

std::wstring parent_directory_of(const std::wstring& path) {
    size_t separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return L".";
    if (separator == 0) return path.substr(0, 1);
    if (separator == 2 && path.size() >= 2 && path[1] == L':') {
        return path.substr(0, 3);
    }
    return path.substr(0, separator);
}

uint64_t entry_size(const WIN32_FIND_DATAW& data) {
    return (static_cast<uint64_t>(data.nFileSizeHigh) << 32) |
           static_cast<uint64_t>(data.nFileSizeLow);
}

int error_code_from_win32(DWORD code) {
    switch (code) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_NAME:
            return ERR_FILE_NOT_FOUND;
        case ERROR_ACCESS_DENIED:
            return ERR_PERMISSION_DENIED;
        default:
            return ERR_UNKNOWN;
    }
}

std::string error_text_from_win32(DWORD code) {
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
        return "File Not Found.";
    }
    if (code == ERROR_ACCESS_DENIED) {
        return "Access is denied.";
    }
    return format_win32_error(code);
}

bool collect_entries(const TargetSpec& target,
                     bool all,
                     bool include_dot_entries,
                     std::vector<DirEntry>& entries,
                     int& error_code,
                     std::string& error) {
    entries.clear();

    WIN32_FIND_DATAW data{};
    HANDLE raw_handle = FindFirstFileW(target.pattern.c_str(), &data);
    if (raw_handle == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        error_code = error_code_from_win32(code);
        error = error_text_from_win32(code);
        return false;
    }
    FindHandle handle(raw_handle);

    while (true) {
        std::wstring name(data.cFileName);
        bool dot_entry = name == L"." || name == L"..";
        bool hidden = is_hidden_or_system(data.dwFileAttributes);

        if ((!dot_entry || include_dot_entries) &&
            (all || !hidden || target.explicit_file)) {
            DirEntry entry;
            entry.name = std::move(name);
            entry.attributes = data.dwFileAttributes;
            entry.size = entry_size(data);
            entry.modified = data.ftLastWriteTime;
            entries.push_back(std::move(entry));
        }

        if (!FindNextFileW(handle.handle, &data)) {
            DWORD code = GetLastError();
            if (code != ERROR_NO_MORE_FILES) {
                error_code = error_code_from_win32(code);
                error = error_text_from_win32(code);
                return false;
            }
            break;
        }
    }

    error_code = ERR_NONE;
    error.clear();
    return true;
}

int compare_names(const std::wstring& left, const std::wstring& right) {
    int result = CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE);
    if (result == CSTR_LESS_THAN) return -1;
    if (result == CSTR_GREATER_THAN) return 1;
    return 0;
}

bool entry_less(const DirEntry& left,
                const DirEntry& right,
                bool directories_first) {
    if (directories_first) {
        bool left_directory = is_directory(left.attributes);
        bool right_directory = is_directory(right.attributes);
        if (left_directory != right_directory) return left_directory;
    }

    if (left.name == L".") return right.name != L".";
    if (right.name == L".") return false;
    if (left.name == L"..") return right.name != L"." && right.name != L"..";
    if (right.name == L"..") return false;

    return compare_names(left.name, right.name) < 0;
}

void sort_entries(std::vector<DirEntry>& entries, bool directories_first) {
    std::sort(entries.begin(), entries.end(),
              [directories_first](const DirEntry& left,
                                  const DirEntry& right) {
                  return entry_less(left, right, directories_first);
              });
}

std::string extension_of(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) return {};
    return lower_ascii(wide_to_utf8(name.substr(dot)));
}

bool extension_in(const std::string& extension,
                  std::initializer_list<const char*> values) {
    for (const char* value : values) {
        if (extension == value) return true;
    }
    return false;
}

int entry_color(const DirEntry& entry) {
    if (is_reparse_point(entry.attributes)) return CLR_BrightCyan;
    if (is_directory(entry.attributes)) return CLR_BrightBlue;

    std::string extension = extension_of(entry.name);

    if (extension_in(extension, {".exe", ".com", ".bat", ".cmd", ".ps1",
                                 ".msi", ".scr", ".dll"})) {
        return CLR_BrightGreen;
    }
    if (extension_in(extension, {".zip", ".7z", ".rar", ".tar", ".gz",
                                 ".bz2", ".xz", ".cab", ".iso", ".tgz",
                                 ".tbz2", ".zst", ".lz", ".lzma"})) {
        return CLR_Red;
    }
    if (extension_in(extension, {".png", ".jpg", ".jpeg", ".gif", ".bmp",
                                 ".ico", ".webp", ".svg", ".tif", ".tiff",
                                 ".heic"})) {
        return CLR_Magenta;
    }
    if (extension_in(extension, {".mp3", ".wav", ".flac", ".aac", ".ogg",
                                 ".m4a", ".wma", ".opus"})) {
        return CLR_Cyan;
    }
    if (extension_in(extension, {".mp4", ".mkv", ".avi", ".mov", ".wmv",
                                 ".flv", ".webm", ".m4v", ".mpg", ".mpeg"})) {
        return CLR_BrightMagenta;
    }

    return CLR_White;
}

bool is_wide_character(wchar_t ch) {
    return (ch >= 0x1100 && ch <= 0x115F) ||
           (ch >= 0x2E80 && ch <= 0xA4CF) ||
           (ch >= 0xAC00 && ch <= 0xD7A3) ||
           (ch >= 0xF900 && ch <= 0xFAFF) ||
           (ch >= 0xFE30 && ch <= 0xFE4F) ||
           (ch >= 0xFF00 && ch <= 0xFF60) ||
           (ch >= 0xFFE0 && ch <= 0xFFE6);
}

bool is_combining_character(wchar_t ch) {
    return (ch >= 0x0300 && ch <= 0x036F) ||
           (ch >= 0x1AB0 && ch <= 0x1AFF) ||
           (ch >= 0x1DC0 && ch <= 0x1DFF) ||
           (ch >= 0x20D0 && ch <= 0x20FF) ||
           (ch >= 0xFE20 && ch <= 0xFE2F);
}

size_t display_width(const std::wstring& text) {
    size_t width = 0;
    for (wchar_t ch : text) {
        if (ch == L'\t') {
            width += 4;
        } else if (ch < 0x20 || (ch >= 0x7F && ch < 0xA0)) {
            continue;
        } else if (is_wide_character(ch)) {
            width += 2;
        } else if (!is_combining_character(ch)) {
            ++width;
        }
    }
    return width;
}

size_t terminal_width() {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output != INVALID_HANDLE_VALUE &&
        GetConsoleScreenBufferInfo(output, &info)) {
        int width = info.srWindow.Right - info.srWindow.Left + 1;
        if (width > 0) return static_cast<size_t>(width);
    }
    return 80;
}

void print_ls_columns(CommandContext& context,
                      const std::vector<DirEntry>& entries) {
    if (entries.empty()) return;

    std::vector<size_t> widths;
    widths.reserve(entries.size());
    size_t max_width = 0;
    for (const DirEntry& entry : entries) {
        size_t width = display_width(entry.name);
        widths.push_back(width);
        if (width > max_width) max_width = width;
    }

    const size_t gap = 2;
    size_t column_width = max_width + gap;
    size_t columns = terminal_width() / column_width;
    if (columns == 0) columns = 1;
    if (columns > entries.size()) columns = entries.size();
    size_t rows = (entries.size() + columns - 1) / columns;

    for (size_t row = 0; row < rows; ++row) {
        size_t last_column = 0;
        for (size_t column = 0; column < columns; ++column) {
            if (column * rows + row < entries.size()) last_column = column;
        }

        for (size_t column = 0; column <= last_column; ++column) {
            size_t index = column * rows + row;
            if (index >= entries.size()) continue;

            const DirEntry& entry = entries[index];
            print(context.out, wide_to_utf8(entry.name), entry_color(entry));

            if (column < last_column) {
                size_t padding = column_width - widths[index];
                context.out << std::string(padding, ' ');
            }
        }
        context.out << '\n';
    }
}

SYSTEMTIME local_time(const FILETIME& file_time) {
    SYSTEMTIME utc{};
    SYSTEMTIME local{};
    if (!FileTimeToSystemTime(&file_time, &utc)) return {};
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return utc;
    return local;
}

std::string format_size(uint64_t size) {
    std::string result = std::to_string(size);
    for (int index = static_cast<int>(result.size()) - 3; index > 0; index -= 3) {
        result.insert(static_cast<size_t>(index), ",");
    }
    return result;
}

std::string format_detail(const DirEntry& entry) {
    SYSTEMTIME time = local_time(entry.modified);

    std::ostringstream line;
    line << std::setfill('0')
         << std::setw(4) << time.wYear << '-'
         << std::setw(2) << time.wMonth << '-'
         << std::setw(2) << time.wDay << "  "
         << std::setw(2) << time.wHour << ':'
         << std::setw(2) << time.wMinute << "    ";

    line << std::setfill(' ');
    if (is_directory(entry.attributes)) {
        line << std::left << std::setw(12) << "<DIR>" << "  ";
    } else {
        line << std::right << std::setw(12) << format_size(entry.size) << "  ";
    }
    line << wide_to_utf8(entry.name);
    return line.str();
}

TargetSpec make_target(const std::string& argument) {
    TargetSpec target;
    target.display = argument.empty() ? get_current_directory() : argument;

    std::wstring input = utf8_to_wide(argument.empty() ? "." : argument);
    target.full_path = get_full_path(input);
    target.wildcard = has_wildcard(target.full_path);

    DWORD attributes = GetFileAttributesW(target.full_path.c_str());
    if (!target.wildcard && attributes != INVALID_FILE_ATTRIBUTES &&
        is_directory(attributes)) {
        target.directory_contents = true;
        target.pattern = target.full_path;
        if (!ends_with_separator(target.pattern)) target.pattern += L'\\';
        target.pattern += L'*';
        target.parent_directory = target.full_path;
    } else {
        target.pattern = target.full_path;
        target.parent_directory = parent_directory_of(target.full_path);
        target.explicit_file = !target.wildcard;
    }

    return target;
}

int tree_children(CommandContext& context,
                  const std::wstring& directory,
                  const ListOptions& options,
                  const std::string& prefix,
                  size_t depth) {
    if (depth >= kMaxTreeDepth) {
        print(context.out, prefix + "└── [max depth reached]\n",
              CLR_BrightYellow);
        return ERR_NONE;
    }

    TargetSpec target;
    target.display = wide_to_utf8(directory);
    target.full_path = directory;
    target.parent_directory = directory;
    target.pattern = directory;
    if (!ends_with_separator(target.pattern)) target.pattern += L'\\';
    target.pattern += L'*';
    target.directory_contents = true;

    std::vector<DirEntry> entries;
    int error_code = ERR_NONE;
    std::string error;
    if (!collect_entries(target, options.all, false, entries, error_code,
                         error)) {
        print(context.out, prefix + "└── " + error + "\n", CLR_BrightRed);
        return error_code;
    }

    sort_entries(entries, true);

    int first_error = ERR_NONE;
    for (size_t index = 0; index < entries.size(); ++index) {
        const DirEntry& entry = entries[index];
        bool last = index + 1 == entries.size();
        std::string line_prefix = prefix + (last ? "└── " : "├── ");

        if (options.detail) {
            print(context.out, line_prefix + format_detail(entry) + "\n",
                  CLR_White);
        } else {
            print(context.out, line_prefix, CLR_Gray);
            print(context.out, wide_to_utf8(entry.name), entry_color(entry));
            context.out << '\n';
        }

        if (is_directory(entry.attributes) &&
            !is_reparse_point(entry.attributes)) {
            std::string child_prefix = prefix + (last ? "    " : "│   ");
            std::wstring child_path = join_path(directory, entry.name);
            int child_error = tree_children(context, child_path, options,
                                            child_prefix, depth + 1);
            if (first_error == ERR_NONE && child_error != ERR_NONE) {
                first_error = child_error;
            }
        }
    }

    return first_error;
}

int tree_target(CommandContext& context,
                const TargetSpec& target,
                const ListOptions& options) {
    if (target.wildcard && has_wildcard(target.parent_directory)) {
        print(context.out,
              "Tree mode does not support wildcards in directory names.\n",
              CLR_BrightRed);
        return ERR_INVALID_OPTION;
    }

    if (target.directory_contents) {
        print(context.out, wide_to_utf8(target.full_path) + "\n",
              options.detail ? CLR_White : CLR_BrightCyan);

        DWORD attributes = GetFileAttributesW(target.full_path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            is_reparse_point(attributes)) {
            return ERR_NONE;
        }

        return tree_children(context, target.full_path, options, "", 0);
    }

    std::vector<DirEntry> entries;
    int error_code = ERR_NONE;
    std::string error;
    if (!collect_entries(target, options.all, false, entries, error_code,
                         error)) {
        print(context.out, error + "\n", CLR_BrightRed);
        return error_code;
    }

    sort_entries(entries, true);

    int first_error = ERR_NONE;
    for (const DirEntry& entry : entries) {
        std::wstring full_path = join_path(target.parent_directory, entry.name);
        print(context.out, wide_to_utf8(full_path) + "\n",
              options.detail ? CLR_White : CLR_BrightCyan);

        if (is_directory(entry.attributes) &&
            !is_reparse_point(entry.attributes)) {
            int child_error = tree_children(context, full_path, options, "", 0);
            if (first_error == ERR_NONE && child_error != ERR_NONE) {
                first_error = child_error;
            }
        }
    }

    return first_error;
}

int list_target(CommandContext& context,
                const TargetSpec& target,
                const ListOptions& options,
                bool multiple) {
    if (options.tree) return tree_target(context, target, options);

    if (multiple && !target.explicit_file) {
        print(context.out, target.display + ":\n",
              options.detail ? CLR_White : CLR_BrightCyan);
    }

    std::vector<DirEntry> entries;
    int error_code = ERR_NONE;
    std::string error;
    if (!collect_entries(target, options.all, options.all, entries, error_code,
                         error)) {
        print(context.out, error + "\n", CLR_BrightRed);
        return error_code;
    }

    sort_entries(entries, options.detail);

    if (options.detail) {
        for (const DirEntry& entry : entries) {
            print(context.out, format_detail(entry) + "\n", CLR_White);
        }
    } else {
        print_ls_columns(context, entries);
    }

    return ERR_NONE;
}

int handle_dir(CommandContext& context) {
    ListOptions options;
    options.all = context.args.has("a") || context.args.has("all");
    options.detail = context.args.has("d") || context.args.has("detail");
    options.tree = context.args.has("t") || context.args.has("tree");

    std::vector<std::string> targets = context.args.positionals();
    if (targets.empty()) targets.push_back("");

    bool multiple = targets.size() > 1;
    bool first_output = true;
    bool previous_group = false;
    int first_error = ERR_NONE;

    for (const std::string& argument : targets) {
        TargetSpec target = make_target(argument);
        bool group = !target.explicit_file;

        if (!first_output && multiple) {
            if (options.tree || group || previous_group) {
                context.out << '\n';
            }
        }

        int error_code = list_target(context, target, options, multiple);
        if (first_error == ERR_NONE && error_code != ERR_NONE) {
            first_error = error_code;
        }

        first_output = false;
        previous_group = group;
    }

    return first_error;
}

CommandSchema make_dir_schema() {
    CommandSchema schema;
    schema.options.push_back(CommandOption{"a", OptionValueMode::Flag});
    schema.options.push_back(CommandOption{"all", OptionValueMode::Flag});
    schema.options.push_back(CommandOption{"d", OptionValueMode::Flag});
    schema.options.push_back(CommandOption{"detail", OptionValueMode::Flag});
    schema.options.push_back(CommandOption{"t", OptionValueMode::Flag});
    schema.options.push_back(CommandOption{"tree", OptionValueMode::Flag});
    schema.max_positionals = std::numeric_limits<size_t>::max();
    return schema;
}

const CommandRegistration register_dir_command(
    "dir", "List directory contents. Options: -a, -d, -t.",
    make_dir_schema(), handle_dir);

const CommandRegistration register_ls_command(
    "ls", "List directory contents. Options: -a, -d, -t.",
    make_dir_schema(), handle_dir);

}
