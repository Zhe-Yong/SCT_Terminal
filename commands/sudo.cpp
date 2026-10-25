#include "global.h"

std::string sudo_usage() {
    return "Usage: sudo ad | sudo sys | sudo su | sudo exit | sudo <command>\n";
}

static int handle_sudo(CommandContext& context) {
    if (context.args.positional_count() == 0) {
        print(context.out, sudo_usage(), CLR_BrightYellow);
        return ERR_NONE;
    }

    std::string subcommand = lower_ascii(*context.args.positional(0));
    PrivilegeLevel current_level = get_process_privilege_level();

    ElevationMode mode = ElevationMode::Administrator;
    PrivilegeLevel target_level = PrivilegeLevel::Administrator;
    bool session_command = true;

    if (subcommand == "ad") {
        mode = ElevationMode::Administrator;
        target_level = PrivilegeLevel::Administrator;
    } else if (subcommand == "sys") {
        mode = ElevationMode::System;
        target_level = PrivilegeLevel::System;
    } else if (subcommand == "su") {
        mode = ElevationMode::TrustedInstaller;
        target_level = PrivilegeLevel::TrustedInstaller;
    } else {
        session_command = false;
    }

    if (session_command) {
        if (elevated_session().active()) {
            if (elevated_session().level() == target_level) {
                print(context.out,
                      "Already running as " +
                          std::string(privilege_level_name(target_level)) + ".\n",
                      privilege_color(target_level));
                return ERR_NONE;
            }
            elevated_session().stop(true);
        }

        if (current_level >= target_level) {
            print(context.out,
                  "Already running as " +
                      std::string(privilege_level_name(current_level)) + ".\n",
                  privilege_color(current_level));
            return ERR_NONE;
        }

        std::string error;
        if (!elevated_session().start(mode, error)) {
            print(context.out, "Failed to start elevated session: " + error + "\n",
                  CLR_BrightRed);
            return ERR_PERMISSION_DENIED;
        }

        print(context.out,
              std::string(privilege_level_name(target_level)) +
                  " session started. Use 'sudo exit' to leave it.\n",
              privilege_color(target_level));
        return ERR_NONE;
    }

    if (subcommand == "exit") {
        if (elevated_session().active()) {
            elevated_session().stop(true);
            print(context.out, "Elevated session ended.\n", CLR_BrightGreen);
            return ERR_NONE;
        }
        if (current_level != PrivilegeLevel::User) {
            print(context.out,
                  std::string(privilege_level_name(current_level)) +
                      " privileges cannot be dropped.\n",
                  CLR_BrightYellow);
            return ERR_NONE;
        }
        print(context.out, "No elevated session is active.\n", CLR_BrightYellow);
        return ERR_NONE;
    }

    std::string command_line = rest_after_first_token(context.raw_line);
    if (command_line.empty()) {
        print(context.out, sudo_usage(), CLR_BrightYellow);
        return ERR_NONE;
    }

    if (current_level != PrivilegeLevel::User) {
        CommandContext nested{context.out};
        return execute_command_line(command_registry(), command_line, nested);
    }

    ElevatedResponse response;
    std::string error;
    if (!elevated_session().run_once(command_line, response, error)) {
        print(context.out, "Failed to run elevated command: " + error + "\n",
              CLR_BrightRed);
        return ERR_PERMISSION_DENIED;
    }

    if (!response.output.empty()) context.out << response.output;
    return response.exit_code;
}

static CommandSchema make_sudo_schema() {
    CommandSchema schema;
    schema.max_positionals = 1;
    schema.raw_tail = true;
    return schema;
}

static const CommandRegistration register_sudo_command(
    "sudo",
    "Start an administrator, SYSTEM, or TrustedInstaller session, or run an elevated command.",
    make_sudo_schema(), handle_sudo);
