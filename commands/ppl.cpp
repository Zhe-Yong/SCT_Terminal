#include "global.h"

static std::string ppl_usage() {
    return "Usage: ppl on [TYPE] | ppl off\n";
}

static int handle_ppl(CommandContext& context) {
    if (ppl_target_pid() != GetCurrentProcessId()) {
        print(context.out,
              "PPL on/off must be run from the main SCT process with "
              "Administrator or higher privileges. Do not run PPL through "
              "a sudo session; start SCT elevated instead.\n",
              CLR_BrightRed);
        return ERR_PERMISSION_DENIED;
    }

    std::string action = lower_ascii(*context.args.positional(0));
    std::string error;

    if (action == "on") {
        std::string type = context.args.positional(1).value_or(
            ppl_default_type());
        bool installed = ppl_run_isolated(ppl_target_pid(), "install", "",
                                          error);
        if (!ppl_run_isolated(ppl_target_pid(), "protect", type, error)) {
            if (installed) {
                std::string cleanup_error;
                ppl_run_isolated(ppl_target_pid(), "uninstall", "",
                                 cleanup_error);
            }
            print(context.out,
                  "Failed to enable PPL protection: " + error + "\n",
                  CLR_BrightRed);
            return ERR_UNKNOWN;
        }

        print(context.out,
              "PPL protection enabled for PID " +
                  std::to_string(ppl_target_pid()) + " (PPL " + type +
                  "); PPL driver loaded.\n",
              CLR_BrightGreen);
        return ERR_NONE;
    }

    if (action == "off") {
        PplProtectionState state = ppl_target_protection(error);
        if (state != PplProtectionState::Unprotected) {
            if (!ppl_run_isolated(ppl_target_pid(), "unprotect", "",
                                  error)) {
                print(context.out,
                      "Failed to remove PPL protection: " + error + "\n",
                      CLR_BrightRed);
                return ERR_UNKNOWN;
            }
        }

        if (!ppl_run_isolated(ppl_target_pid(), "uninstall", "", error)) {
            print(context.out,
                  "Failed to unload the PPL driver: " + error + "\n",
                  CLR_BrightRed);
            return ERR_UNKNOWN;
        }

        print(context.out,
              "PPL protection disabled for PID " +
                  std::to_string(ppl_target_pid()) +
                  "; PPL driver unloaded.\n",
              CLR_BrightGreen);
        return ERR_NONE;
    }

    print(context.out, ppl_usage(), CLR_BrightYellow);
    return ERR_INVALID_OPTION;
}

static CommandSchema make_ppl_schema() {
    CommandSchema schema;
    schema.min_positionals = 1;
    schema.max_positionals = 2;
    return schema;
}

static const CommandRegistration register_ppl_command(
    "ppl",
    "Enable or disable PPL protection for the SCT process.",
    make_ppl_schema(), handle_ppl, 2);
