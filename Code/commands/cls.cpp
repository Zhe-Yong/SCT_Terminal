#include "global.h"

static int handle_cls(CommandContext& context) {
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (output != INVALID_HANDLE_VALUE &&
        GetConsoleScreenBufferInfo(output, &info)) {
        DWORD cell_count =
            static_cast<DWORD>(info.dwSize.X) * info.dwSize.Y;
        COORD home{0, 0};
        DWORD written = 0;
        FillConsoleOutputCharacterW(output, L' ', cell_count, home, &written);
        FillConsoleOutputAttribute(output, info.wAttributes, cell_count, home,
                                   &written);
        SetConsoleCursorPosition(output, home);
    }

    context.out << "\033[2J\033[3J\033[H";
    context.out.flush();
    return ERR_NONE;
}

static const CommandRegistration register_cls_command(
    "cls", "Clear the console display.", CommandSchema{}, handle_cls);
