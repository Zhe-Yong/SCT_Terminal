#include "global.h"

namespace {

std::string protect_usage() {
    return "Usage: protect ppl on [TYPE] | protect ppl off\n"
           "       protect keep [on|1|off|0]\n";
}

bool vulnerable_driver_blocklist_enabled() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    LONG status = RegGetValueW(
        HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\CI\\Config",
        L"VulnerableDriverBlocklistEnable", RRF_RT_REG_DWORD, &type, &value,
        &size);
    return status == ERROR_SUCCESS && value != 0;
}

bool parse_keep_action(const std::optional<std::string>& value,
                       bool& enable,
                       std::string& error) {
    if (!value) {
        enable = true;
        return true;
    }

    std::string action = lower_ascii(*value);
    if (action == "on" || action == "1") {
        enable = true;
        return true;
    }
    if (action == "off" || action == "0") {
        enable = false;
        return true;
    }

    error = "Invalid keep action: " + *value +
            ". Use on/1 to enable or off/0 to disable.";
    return false;
}

int handle_keep(CommandContext& context, size_t index) {
    bool enable = true;
    std::string error;
    if (!parse_keep_action(context.args.positional(index), enable, error)) {
        print(context.out, error + "\n", CLR_BrightRed);
        print(context.out, protect_usage(), CLR_BrightYellow);
        return ERR_INVALID_OPTION;
    }

    if (enable) {
        if (!enable_keep_protection(error)) {
            print(context.out,
                  "Failed to enable keep protection: " + error + "\n",
                  CLR_BrightRed);
            return ERR_UNKNOWN;
        }
        print(context.out,
              "Keep protection enabled: close requests are blocked. "
              "Use the exit command to quit.\n",
              CLR_BrightGreen);
        terminal().refresh_title_bar();
        return ERR_NONE;
    }

    if (!disable_keep_protection(error)) {
        print(context.out,
              "Failed to disable keep protection: " + error + "\n",
              CLR_BrightRed);
        return ERR_UNKNOWN;
    }
    print(context.out, "Keep protection disabled.\n", CLR_BrightGreen);
    terminal().refresh_title_bar();
    return ERR_NONE;
}

int handle_ppl(CommandContext& context, size_t index) {
#ifndef _WIN64
    BOOL wow64 = FALSE;
    if (IsWow64Process(GetCurrentProcess(), &wow64) && wow64) {
        print(context.out,
              "PPL is not supported by 32-bit SCT on 64-bit Windows. "
              "Use SCT64.exe for PPL protection.\n",
              CLR_BrightRed);
        return ERR_PERMISSION_DENIED;
    }
#endif

    if (get_process_privilege_level() < PrivilegeLevel::Administrator) {
        print(context.out,
              "protect ppl requires Administrator or higher privileges.\n",
              CLR_BrightRed);
        return ERR_PERMISSION_DENIED;
    }

    std::optional<std::string> action_value =
        context.args.positional(index);
    if (!action_value) {
        print(context.out, protect_usage(), CLR_BrightYellow);
        return ERR_TOO_FEW_ARGUMENTS;
    }

    if (ppl_target_pid() != GetCurrentProcessId()) {
        print(context.out,
              "protect ppl must be run from the main SCT process. "
              "Do not run PPL through a sudo session; start SCT elevated "
              "instead.\n",
              CLR_BrightRed);
        return ERR_PERMISSION_DENIED;
    }

    std::string action = lower_ascii(*action_value);
    std::string error;

    if (action == "on") {
        std::string type =
            context.args.positional(index + 1).value_or(ppl_default_type());

        std::string install_error;
        bool installed = ppl_run_isolated(ppl_target_pid(), "install", "",
                                          install_error);
        std::string protect_error;
        if (!ppl_run_isolated(ppl_target_pid(), "protect", type,
                              protect_error)) {
            if (installed) {
                std::string cleanup_error;
                ppl_run_isolated(ppl_target_pid(), "uninstall", "",
                                 cleanup_error);
            }

            std::string message =
                "Failed to enable PPL protection: " + protect_error;
            if (!installed) {
                message += " Driver install failed: " + install_error;
                if (vulnerable_driver_blocklist_enabled()) {
                    message +=
                        " Windows vulnerable driver blocklist is enabled, "
                        "which blocks the RTCore driver.";
                }
            }
            message += " Use 'protect keep on' for DACL-based protection.";
            print(context.out, message + "\n", CLR_BrightRed);
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

    print(context.out, protect_usage(), CLR_BrightYellow);
    return ERR_INVALID_OPTION;
}

int handle_protect(CommandContext& context) {
    if (context.args.positional_count() == 0) {
        print(context.out, protect_usage(), CLR_BrightYellow);
        return ERR_TOO_FEW_ARGUMENTS;
    }

    std::string subcommand = lower_ascii(*context.args.positional(0));
    if (subcommand == "ppl") return handle_ppl(context, 1);
    if (subcommand == "keep") return handle_keep(context, 1);

    print(context.out, protect_usage(), CLR_BrightYellow);
    return ERR_INVALID_OPTION;
}

CommandSchema make_protect_schema() {
    CommandSchema schema;
    schema.min_positionals = 1;
    schema.max_positionals = 3;
    return schema;
}

const CommandRegistration register_protect_command(
    "protect",
    "Enable PPL protection or keep-close protection for SCT.",
    make_protect_schema(), handle_protect, 0,
    "Usage: protect ppl on [TYPE] | protect ppl off\n"
    "       protect keep [on|1|off|0]\n"
    "protect keep denies taskkill -f through a process DACL and blocks the "
    "close button/Alt+F4; external close requests restart a protected SCT. "
    "Use the exit command to quit.");

}
