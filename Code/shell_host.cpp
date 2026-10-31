#include "global.h"

using namespace std;

enum class ShellAction {
    Continue,
    Exit
};

static bool execute_local_line(const string& line) {
    CommandContext context{std::cout};
    execute_command_line(command_registry(), line, context);
    print_rst();
    return context.exit_requested;
}

static ShellAction process_line(const string& line) {
    if (line.empty()) return ShellAction::Continue;
    return execute_local_line(line) ? ShellAction::Exit
                                    : ShellAction::Continue;
}

static void print_prompt() {
    PrivilegeLevel level = get_process_privilege_level();
    string cwd = get_current_directory();
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
