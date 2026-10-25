#include "global.h"

static int handle_help(CommandContext& context) {
    print(context.out, "Available commands:\n", CLR_BrightCyan);
    for (const Command* command : command_registry().list()) {
        std::ostringstream line;
        line << "  " << std::left << std::setw(10) << command->name
             << " - " << command->description << '\n';
        print(context.out, line.str(), CLR_White);
    }
    return ERR_NONE;
}

static const CommandRegistration register_help_command(
    "help", "Show available commands.", CommandSchema{}, handle_help);
