#include "global.h"

namespace {

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

int handle_ren(CommandContext& context) {
    std::string source = *context.args.positional(0);
    std::string target = *context.args.positional(1);

    std::wstring source_wide = utf8_to_wide(source);
    std::wstring target_wide = utf8_to_wide(target);

    if (!MoveFileExW(source_wide.c_str(), target_wide.c_str(), 0)) {
        DWORD code = GetLastError();
        print(context.out,
              "Cannot rename '" + source + "' to '" + target + "': " +
                  format_win32_error(code) + "\n",
              CLR_BrightRed);
        return error_code_from_win32(code);
    }

    return ERR_NONE;
}

CommandSchema make_ren_schema() {
    CommandSchema schema;
    schema.min_positionals = 2;
    schema.max_positionals = 2;
    return schema;
}

const CommandRegistration register_ren_command(
    "ren", "Rename a file or directory.", make_ren_schema(), handle_ren);

}
