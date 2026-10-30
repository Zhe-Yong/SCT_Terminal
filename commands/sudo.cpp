#include "global.h"

std::string sudo_usage() {
    return "Usage: sudo ad | sudo sys | sudo su | sudo exit | sudo <command>\n";
}

static bool elevation_target(const std::string& subcommand,
                             ElevationMode& mode,
                             PrivilegeLevel& target) {
    if (subcommand == "ad") {
        mode = ElevationMode::Administrator;
        target = PrivilegeLevel::Administrator;
        return true;
    }
    if (subcommand == "sys") {
        mode = ElevationMode::System;
        target = PrivilegeLevel::System;
        return true;
    }
    if (subcommand == "su") {
        mode = ElevationMode::TrustedInstaller;
        target = PrivilegeLevel::TrustedInstaller;
        return true;
    }
    return false;
}

static int relaunch_or_report(CommandContext& context,
                              ElevationMode mode,
                              const std::string& command_line) {
    std::string error;
    if (!launch_elevated_self(mode, command_line, error)) {
        if (error == "Command cancelled.") return ERR_CANCELLED;
        print(context.out, "Failed to relaunch elevated process: " + error + "\n",
              CLR_BrightRed);
        return ERR_PERMISSION_DENIED;
    }

    ExitProcess(0);
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
    if (elevation_target(subcommand, mode, target_level)) {
        if (current_level >= target_level) {
            print(context.out,
                  "Already running as " +
                      std::string(privilege_level_name(current_level)) + ".\n",
                  privilege_color(current_level));
            return ERR_NONE;
        }

        return relaunch_or_report(context, mode, "");
    }

    if (subcommand == "exit") {
        if (current_level == PrivilegeLevel::User) {
            print(context.out, "No elevated privileges to drop.\n",
                  CLR_BrightYellow);
            return ERR_NONE;
        }

        std::string error;
        if (!relaunch_as_user(error)) {
            print(context.out, "Failed to relaunch as User: " + error + "\n",
                  CLR_BrightRed);
            return ERR_PERMISSION_DENIED;
        }

        ExitProcess(0);
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

    return relaunch_or_report(context, ElevationMode::Administrator, command_line);
}

static CommandSchema make_sudo_schema() {
    CommandSchema schema;
    schema.max_positionals = 1;
    schema.raw_tail = true;
    return schema;
}

static const CommandRegistration register_sudo_command(
    "sudo",
    "Relaunch SCT as Administrator, SYSTEM, or TrustedInstaller.",
    make_sudo_schema(), handle_sudo);
