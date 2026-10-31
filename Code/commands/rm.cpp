#include "global.h"

namespace {

bool is_separator(wchar_t ch) {
    return ch == L'\\' || ch == L'/';
}

std::wstring trim_trailing_separators(const std::wstring& path) {
    std::wstring result = path;
    while (result.size() > 1 && is_separator(result.back())) {
        result.pop_back();
    }
    return result;
}

std::wstring get_full_path(const std::wstring& path) {
    DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (needed == 0) return path;

    std::wstring result(needed, L'\0');
    DWORD length = GetFullPathNameW(path.c_str(), needed, result.data(),
                                    nullptr);
    if (length == 0 || length >= needed) return path;
    result.resize(length);
    return result;
}

bool is_root_directory(const std::wstring& path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return false;
    }

    std::wstring normalized = trim_trailing_separators(path);
    if (normalized.size() == 2 && normalized[1] == L':') return true;

    std::vector<wchar_t> volume(32768, L'\0');
    if (GetVolumePathNameW(path.c_str(), volume.data(),
                           static_cast<DWORD>(volume.size()))) {
        std::wstring volume_root =
            trim_trailing_separators(volume.data());
        if (CompareStringOrdinal(normalized.c_str(), -1, volume_root.c_str(),
                                 -1, TRUE) == CSTR_EQUAL) {
            return true;
        }
    }
    return false;
}

int error_code_from_win32(DWORD code) {
    switch (code) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_NAME:
            return ERR_FILE_NOT_FOUND;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
            return ERR_PERMISSION_DENIED;
        default:
            return ERR_UNKNOWN;
    }
}

std::string error_text_from_win32(DWORD code) {
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
        return "No such file or directory";
    }
    if (code == ERROR_ACCESS_DENIED) {
        return "Permission denied";
    }
    return format_win32_error(code);
}

void print_remove_error(CommandContext& context,
                        const std::wstring& path,
                        DWORD code) {
    print(context.out,
          "Cannot remove '" + wide_to_utf8(path) + "': " +
              error_text_from_win32(code) + "\n",
          CLR_BrightRed);
}

void print_removed(CommandContext& context, const std::wstring& path) {
    print(context.out, "removed '" + wide_to_utf8(path) + "'\n",
          CLR_BrightGreen);
}

std::wstring join_path(const std::wstring& directory,
                       const std::wstring& name) {
    if (directory.empty()) return name;
    if (is_separator(directory.back())) return directory + name;
    return directory + L'\\' + name;
}

void clear_readonly(const std::wstring& path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_READONLY) != 0) {
        SetFileAttributesW(path.c_str(),
                           attributes & ~FILE_ATTRIBUTE_READONLY);
    }
}

int remove_path(CommandContext& context,
                const std::wstring& path,
                bool recursive,
                bool force);

int remove_children(CommandContext& context,
                    const std::wstring& directory,
                    bool force) {
    std::wstring pattern = directory;
    if (pattern.empty() || !is_separator(pattern.back())) {
        pattern += L'\\';
    }
    pattern += L'*';

    WIN32_FIND_DATAW data{};
    HANDLE handle = FindFirstFileW(pattern.c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return ERR_NONE;
        }
        print_remove_error(context, directory, code);
        return error_code_from_win32(code);
    }

    int first_error = ERR_NONE;
    do {
        std::wstring name = data.cFileName;
        if (name == L"." || name == L"..") continue;

        std::wstring child = join_path(directory, name);
        int code = remove_path(context, child, true, force);
        if (first_error == ERR_NONE && code != ERR_NONE) {
            first_error = code;
        }
    } while (FindNextFileW(handle, &data));

    DWORD code = GetLastError();
    FindClose(handle);
    if (code != ERROR_NO_MORE_FILES) {
        print_remove_error(context, directory, code);
        if (first_error == ERR_NONE) {
            first_error = error_code_from_win32(code);
        }
    }
    return first_error;
}

int remove_path(CommandContext& context,
                const std::wstring& path,
                bool recursive,
                bool force) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD code = GetLastError();
        if (force && (code == ERROR_FILE_NOT_FOUND ||
                      code == ERROR_PATH_NOT_FOUND)) {
            return ERR_NONE;
        }
        print_remove_error(context, path, code);
        return error_code_from_win32(code);
    }

    bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    bool reparse = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

    if (directory) {
        if (!recursive && !reparse) {
            print(context.out,
                  "Cannot remove '" + wide_to_utf8(path) +
                      "': Is a directory\n",
                  CLR_BrightRed);
            return ERR_IS_A_DIRECTORY;
        }

        if (reparse) {
            if (force) clear_readonly(path);
            if (RemoveDirectoryW(path.c_str())) {
                print_removed(context, path);
                return ERR_NONE;
            }
            DWORD code = GetLastError();
            print_remove_error(context, path, code);
            return error_code_from_win32(code);
        }

        int first_error = remove_children(context, path, force);
        if (force) clear_readonly(path);
        if (RemoveDirectoryW(path.c_str())) {
            print_removed(context, path);
            return first_error;
        }

        DWORD code = GetLastError();
        if (first_error == ERR_NONE) {
            print_remove_error(context, path, code);
            first_error = error_code_from_win32(code);
        }
        return first_error;
    }

    if (force) clear_readonly(path);
    if (DeleteFileW(path.c_str())) {
        print_removed(context, path);
        return ERR_NONE;
    }

    DWORD code = GetLastError();
    if (force && (code == ERROR_FILE_NOT_FOUND ||
                  code == ERROR_PATH_NOT_FOUND)) {
        return ERR_NONE;
    }
    print_remove_error(context, path, code);
    return error_code_from_win32(code);
}

int handle_rm(CommandContext& context) {
    bool recursive = context.args.has("r") ||
                     context.args.has("recursive") ||
                     context.args.has("rf") ||
                     context.args.has("fr");
    bool force = context.args.has("f") ||
                 context.args.has("force") ||
                 context.args.has("rf") ||
                 context.args.has("fr");
    bool no_preserve_root = context.args.has("no-preserve-root");

    int first_error = ERR_NONE;
    for (const std::string& argument : context.args.positionals()) {
        if (argument.empty()) {
            print(context.out,
                  "Cannot remove '': No such file or directory\n",
                  CLR_BrightRed);
            if (first_error == ERR_NONE) first_error = ERR_FILE_NOT_FOUND;
            continue;
        }

        std::wstring full_path = get_full_path(utf8_to_wide(argument));
        bool root = is_root_directory(full_path);

        if (root && !no_preserve_root) {
            print(context.out,
                  "Dangerous operation. Make sure you know what you are "
                  "doing. To continue, enter: rm -rf \"" +
                      wide_to_utf8(full_path) +
                      "\" --no-preserve-root\n",
                  CLR_BrightRed);
            if (first_error == ERR_NONE) {
                first_error = ERR_DANGEROUS_OPERATION;
            }
            continue;
        }

        if (root) {
            if (!recursive) {
                print(context.out,
                      "Cannot remove '" + wide_to_utf8(full_path) +
                          "': Is a directory\n",
                      CLR_BrightRed);
                if (first_error == ERR_NONE) {
                    first_error = ERR_IS_A_DIRECTORY;
                }
                continue;
            }

            int code = remove_children(context, full_path, force);
            if (first_error == ERR_NONE && code != ERR_NONE) {
                first_error = code;
            }
            continue;
        }

        int code = remove_path(context, full_path, recursive, force);
        if (first_error == ERR_NONE && code != ERR_NONE) {
            first_error = code;
        }
    }

    return first_error;
}

CommandSchema make_rm_schema() {
    CommandSchema schema;
    schema.options.push_back(CommandOption{"r", OptionValueMode::Flag});
    schema.options.push_back(
        CommandOption{"recursive", OptionValueMode::Flag});
    schema.options.push_back(CommandOption{"f", OptionValueMode::Flag});
    schema.options.push_back(CommandOption{"force", OptionValueMode::Flag});
    schema.options.push_back(CommandOption{"rf", OptionValueMode::Flag});
    schema.options.push_back(CommandOption{"fr", OptionValueMode::Flag});
    schema.options.push_back(
        CommandOption{"no-preserve-root", OptionValueMode::Flag});
    schema.min_positionals = 1;
    schema.max_positionals = std::numeric_limits<size_t>::max();
    return schema;
}

const CommandRegistration register_rm_command(
    "rm",
    "Remove files or directories. Options: -r, -f, -rf, --no-preserve-root.",
    make_rm_schema(), handle_rm);

}
