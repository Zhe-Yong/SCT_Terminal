#include "global.h"

static int handle_whoami(CommandContext& context) {
    std::string user = get_current_user_name();
    if (user.empty()) user = "unknown";
    print(context.out, user + "\n", CLR_White);

    PrivilegeLevel level = get_process_privilege_level();
    print(context.out,
          "Privilege Level: " + std::string(privilege_level_name(level)) + "\n",
          privilege_color(level));
    return ERR_NONE;
}

static const CommandRegistration register_whoami_command(
    "whoami", "Show current user and privilege level.", CommandSchema{},
    handle_whoami);
