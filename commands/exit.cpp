#include "global.h"

static int handle_exit(CommandContext& context) {
    context.exit_requested = true;
    return ERR_NONE;
}

static const CommandRegistration register_exit_command(
    "exit", "Exit SuperCommandTerminal.", CommandSchema{}, handle_exit);
