#include "global.h"

namespace {

int print_command_help(CommandContext& context, const Command& command) {
    print(context.out, command.name + " - " + command.description + "\n",
          CLR_BrightCyan);
    if (!command.help.empty()) {
        print(context.out, command.help + "\n", CLR_White);
    } else {
        print(context.out, "No detailed help is available.\n", CLR_Gray);
    }
    return ERR_NONE;
}

int handle_help(CommandContext& context) {
    if (context.args.positional_count() > 0) {
        std::string name = *context.args.positional(0);
        const Command* command = command_registry().find(name);
        if (!command) {
            print(context.out,
                  "'" + name + "' is not recognized as a SCT command.\n",
                  CLR_BrightRed);
            return ERR_INVALID_COMMAND;
        }
        return print_command_help(context, *command);
    }

    print(context.out, "Available commands:\n", CLR_BrightCyan);
    for (const Command* command : command_registry().list()) {
        std::ostringstream line;
        line << "  " << std::left << std::setw(10) << command->name
             << " - " << command->description << '\n';
        print(context.out, line.str(), CLR_White);
    }
    print(context.out, "Run 'help <command>' for details.\n", CLR_Gray);
    return ERR_NONE;
}

CommandSchema make_help_schema() {
    CommandSchema schema;
    schema.max_positionals = 1;
    return schema;
}

const CommandRegistration register_help_command(
    "help", "Show available commands or details for one command.",
    make_help_schema(), handle_help, 0,
    "Usage: help [command]\n"
    "Without a command, lists every registered command.\n"
    "With a command, shows the help text supplied when that command was "
    "registered.");

}
