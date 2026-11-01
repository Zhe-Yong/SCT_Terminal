#include "global.h"

using namespace std;

static int run_ppl_mode(const vector<string>& args) {
    if (args.size() < 4) return 1;

    DWORD target_pid = 0;
    try {
        unsigned long long value = std::stoull(args[2]);
        if (value == 0 || value > 0xFFFFFFFFull) return 1;
        target_pid = static_cast<DWORD>(value);
    } catch (...) {
        return 1;
    }

    std::string action = lower_ascii(args[3]);
    std::string type = args.size() >= 5 && !args[4].empty()
                           ? args[4]
                           : ppl_default_type();
    ppl_set_target_pid(target_pid);

    std::string error;
    if (action == "install") {
        return ppl_install_driver(error) ? 0 : 2;
    }
    if (action == "protect") {
        return ppl_protect_target(type, error) ? 0 : 2;
    }
    if (action == "unprotect") {
        return ppl_unprotect_target(error) ? 0 : 2;
    }
    if (action == "uninstall") {
        if (ppl_uninstall_driver(error)) return 0;
        if (error.find("reboot") != std::string::npos) return 6;
        return 2;
    }
    return 1;
}

static std::string startup_value(const vector<string>& args,
                                 const std::string& name) {
    for (size_t index = 1; index + 1 < args.size(); ++index) {
        if (lower_ascii(args[index]) == name) {
            return args[index + 1];
        }
    }
    return {};
}

static void print_welcome() {
    std::string title = "SuperCommandTerminal  " + std::string(SCT_DISPLAY_VERSION);
    std::string author = "Developed by ZheYong";
    size_t content_width = std::max(title.size(), author.size());
    std::string border(content_width + 6, '=');

    auto line = [content_width](const std::string& text) {
        return "|  " + text + std::string(content_width - text.size(), ' ') +
               "  |\n";
    };

    print(border + "\n" + line(title) + line(author) + border + "\n",
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
    std::string resource_directory =
        startup_value(startup_args, "--sct-resource-dir");
    if (!resource_directory.empty()) {
        SetEnvironmentVariableW(
            L"SCT_RESOURCE_DIR",
            utf8_to_wide(resource_directory).c_str());
    }

    if (startup_args.size() >= 2 &&
        startup_args[1] == "--sct-ppl-helper") {
        return run_ppl_mode(startup_args);
    }

    std::string bootstrap_error;
    RuntimeBootstrapResult bootstrap =
        bootstrap_random_runtime(startup_args, bootstrap_error);
    if (bootstrap == RuntimeBootstrapResult::Launched) {
        return 0;
    }

    std::string startup_directory =
        startup_value(startup_args, "--sct-cwd");
    if (!startup_directory.empty()) {
        set_current_directory(startup_directory);
    }

    bool force_console = false;
    for (size_t index = 1; index < startup_args.size(); ++index) {
        if (lower_ascii(startup_args[index]) == "--console") {
            force_console = true;
        }
    }
    if (force_console) ensure_console_attached();

    std::signal(SIGINT, SIG_IGN);
#ifdef SIGBREAK
    std::signal(SIGBREAK, SIG_IGN);
#endif
    install_console_cancel_handler();

    std::string cancel_error;
    bool cancel_ready = initialize_command_cancellation(cancel_error);

    HRESULT com_result = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    std::string terminal_error;
    bool gui_terminal = false;
    if (!force_console) {
        gui_terminal = terminal().start_gui(terminal_error);
    }
    if (!gui_terminal) {
        setup_console();
        if (!force_console && !terminal_error.empty()) {
            print("Warning: custom terminal unavailable, using console: " +
                      terminal_error + "\n",
                  CLR_BrightYellow);
        }
    }

    print_welcome();
    if (!cancel_ready) {
        print("Warning: Ctrl+C command cancellation is unavailable: " +
                  cancel_error + "\n",
              CLR_BrightYellow);
    }

    std::string plugin_error;
    if (!plugin_manager().initialize(get_resource_directory(), plugin_error,
                                     true)) {
        print("Warning: plugin state is unavailable: " + plugin_error + "\n",
              CLR_BrightYellow);
        print_rst();
    }

    AutoexecResult autoexec = run_autoexec_file();
    if (autoexec.rejected) {
        print(autoexec.message + "\n", CLR_BrightRed);
        print_rst();
    }
    if (autoexec.exit_code != 0) {
        shutdown_command_cancellation();
        print_rst();
        terminal().stop_gui();
        if (SUCCEEDED(com_result)) CoUninitialize();
        return autoexec.exit_code;
    }

    std::string initial_command =
        startup_value(startup_args, "--sct-run-command");
    if (!autoexec.exit_requested && !initial_command.empty()) {
        CommandContext context{std::cout};
        execute_command_line(command_registry(), initial_command, context);
        print_rst();
        if (context.exit_requested) autoexec.exit_requested = true;
    }

    if (!autoexec.exit_requested) {
        run_shell();
    }

    shutdown_command_cancellation();
    print_rst();
    terminal().stop_gui();

    if (SUCCEEDED(com_result)) CoUninitialize();
    return 0;
}
