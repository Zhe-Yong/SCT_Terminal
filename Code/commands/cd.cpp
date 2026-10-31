#include "global.h"

static int handle_cd(CommandContext& context) {
    if (context.args.positional_count() == 0) {
        if (context.args.has("d")) {
            print(context.out, "Usage: cd [/d] <directory>\n",
                  CLR_BrightYellow);
            return ERR_TOO_FEW_ARGUMENTS;
        }
        print(context.out, get_current_directory() + "\n");
        return ERR_NONE;
    }

    std::string target = *context.args.positional(0);
    if (!set_current_directory(target)) {
        print(context.out, "The system cannot find the path specified.\n",
              CLR_BrightRed);
        return ERR_FILE_NOT_FOUND;
    }
    return ERR_NONE;
}

static CommandSchema make_cd_schema() {
    CommandSchema schema;
    schema.options.push_back(CommandOption{"d", OptionValueMode::Flag});
    schema.max_positionals = 1;
    return schema;
}

static const CommandRegistration register_cd_command(
    "cd", "Show or change the current directory.", make_cd_schema(), handle_cd);
