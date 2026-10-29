#include "global.h"

using namespace std;

enum class ShellAction {
    Continue,
    Exit
};

static void dispatch_elevated(const string& line) {
    CommandExecutionGuard guard;
    if (command_cancelled()) {
        print("Command cancelled.\n", CLR_BrightYellow);
        print_rst();
        return;
    }

    ElevatedResponse response;
    string error;
    if (!elevated_session().send(line, response, error)) {
        if (error == "Command cancelled.") {
            print("Command cancelled.\n", CLR_BrightYellow);
        } else {
            print("Elevated command failed: " + error + "\n", CLR_BrightRed);
            elevated_session().stop(true);
        }
        print_rst();
        return;
    }

    if (!response.output.empty()) {
        cout << response.output;
        cout.flush();
    }
    print_rst();
}

static bool execute_local_line(const string& line) {
    CommandContext context{std::cout};
    execute_command_line(command_registry(), line, context);
    print_rst();
    return context.exit_requested;
}

static bool is_elevation_mode_command(const string& subcommand) {
    return subcommand == "ad" || subcommand == "sys" || subcommand == "su";
}

static void print_sudo_usage() {
    print(sudo_usage(), CLR_BrightYellow);
    print_rst();
}

static ShellAction handle_active_session_line(
    const string& line,
    const vector<string>& args,
    const string& name) {
    if (name == "exit") {
        elevated_session().stop(true);
        return ShellAction::Exit;
    }

    if (name != "sudo") {
        dispatch_elevated(line);
        return ShellAction::Continue;
    }

    if (args.size() >= 2) {
        string subcommand = lower_ascii(args[1]);
        if (subcommand == "exit") {
            elevated_session().stop(true);
            print("Elevated session ended.\n", CLR_BrightGreen);
            print_rst();
            return ShellAction::Continue;
        }
        if (is_elevation_mode_command(subcommand)) {
            execute_local_line(line);
            return ShellAction::Continue;
        }

        string command_line = rest_after_first_token(line);
        if (!command_line.empty()) {
            dispatch_elevated(command_line);
            return ShellAction::Continue;
        }
    }

    print_sudo_usage();
    return ShellAction::Continue;
}

static ShellAction process_line(const string& line) {
    if (line.empty()) return ShellAction::Continue;

    if (elevated_session().active()) {
        vector<string> args = split_command_line(line);
        if (args.empty()) return ShellAction::Continue;
        return handle_active_session_line(line, args, lower_ascii(args[0]));
    }

    return execute_local_line(line) ? ShellAction::Exit
                                    : ShellAction::Continue;
}

static void print_prompt() {
    PrivilegeLevel level = elevated_session().active()
                               ? elevated_session().level()
                               : get_process_privilege_level();
    string cwd = elevated_session().active()
                     ? elevated_session().cwd()
                     : get_current_directory();
    if (cwd.empty()) cwd = "?";

    cout << '[' << privilege_level_tag(level) << ']' << cwd << ">";
    cout.flush();
}

void run_shell() {
    while (true) {
        print_prompt();

        string line;
        ReadLineResult result = read_line_utf8(line);

        if (result == ReadLineResult::Line) {
            if (consume_prompt_interrupt()) {
                clear_command_cancel();
                cout << "\n";
                print_rst();
                continue;
            }
            clear_command_cancel();
            if (process_line(line) == ShellAction::Exit) break;
            continue;
        }

        if (result == ReadLineResult::Interrupted) {
            Sleep(50);
            consume_prompt_interrupt();
            clear_command_cancel();
            cout << "\n";
            print_rst();
            continue;
        }

        Sleep(50);
        if (consume_prompt_interrupt() || command_cancelled()) {
            clear_command_cancel();
            cout << "\n";
            print_rst();
            continue;
        }

        cout << "\n";
        break;
    }
}
