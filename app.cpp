#include "global.h"

using namespace std;

static int run_helper_mode(const vector<string>& args) {
    DWORD parent_pid = 0;
    try {
        unsigned long long value = std::stoull(args[5]);
        if (value == 0 || value > 0xFFFFFFFFull) return 1;
        parent_pid = static_cast<DWORD>(value);
    } catch (...) {
        return 1;
    }

    return run_elevated_helper(args[2], args[3], args[4], parent_pid);
}

static void print_welcome() {
    print("========================================\n"
          "|  SuperCommandShell   V0.0.1-Preview  |\n"
          "|  Developed by ZheYong                |\n"
          "========================================\n",
          CLR_White, CLR_BgBlue, CLR_Bold);
    print_rst();

    if (get_process_privilege_level() == PrivilegeLevel::User) {
        print("Running under non administrator privileges\n",
              CLR_Black, CLR_BgWhite);
        print_rst();
    }

    cout << "Type 'help' for help.\n";
}

int run_application() {
    vector<string> startup_args = get_command_line_args();
    if (startup_args.size() >= 6 &&
        startup_args[1] == "--sct-elevated-helper") {
        return run_helper_mode(startup_args);
    }

    HRESULT com_result = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    setup_console();
    print_welcome();
    run_shell();

    elevated_session().stop(true);
    print_rst();

    if (SUCCEEDED(com_result)) CoUninitialize();
    return 0;
}
