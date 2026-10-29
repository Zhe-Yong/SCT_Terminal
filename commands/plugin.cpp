#include "global.h"

namespace {

void print_usage() {
    print("Usage:\n"
          "  plugin -list\n"
          "  plugin -enable <sha256>\n"
          "  plugin -disable <sha256>\n",
          CLR_BrightYellow);
}

void print_plugin(const PluginInfo& info) {
    std::string status = "disabled";
    if (info.loaded) {
        status = "loaded";
    } else if (info.enabled) {
        status = "enabled";
    }

    print("  " + info.file_name + " [" + status + "]\n",
          info.loaded ? CLR_BrightGreen : CLR_White);
    print("    sha256: " + info.sha256 + "\n", CLR_Gray);
    if (info.signed_file) {
        print("    signer: " + info.signer_subject + "\n", CLR_Gray);
        print("    spki:   " + info.signer_spki_sha256 + "\n", CLR_Gray);
        if (info.trusted) {
            print("    trust:  pinned\n", CLR_BrightGreen);
        } else if (info.signature_valid) {
            print("    trust:  valid signature, not pinned\n", CLR_BrightYellow);
        } else {
            print("    trust:  invalid signature\n", CLR_BrightRed);
        }
    } else {
        print("    trust:  unsigned\n", CLR_BrightYellow);
    }
    if (!info.error.empty()) {
        print("    error:  " + info.error + "\n", CLR_BrightRed);
    }
    print_rst();
}

int handle_plugin(CommandContext& context) {
    if (context.args.has("list")) {
        std::vector<PluginInfo> plugins = plugin_manager().list();
        print("Plugin directory: " + plugin_manager().plugin_directory() + "\n");
        print("State file:       " + plugin_manager().state_path() + "\n",
              CLR_Gray);
        if (plugins.empty()) {
            print("No plugins found.\n", CLR_BrightYellow);
            print_rst();
            return ERR_NONE;
        }
        for (const PluginInfo& plugin : plugins) print_plugin(plugin);
        return ERR_NONE;
    }

    if (context.args.has("enable")) {
        if (context.origin == CommandOrigin::Autoexec) {
            print("plugin -enable is not allowed in autoexec.sct.\n",
                  CLR_BrightRed);
            print_rst();
            return ERR_PERMISSION_DENIED;
        }
        std::string hash = context.args.value("enable").value_or("");
        std::string error;
        if (!plugin_manager().enable(hash, error)) {
            print("Failed to enable plugin: " + error + "\n", CLR_BrightRed);
            print_rst();
            return ERR_PERMISSION_DENIED;
        }
        print("Plugin enabled. Restart or run 'plugin -list' to inspect it.\n",
              CLR_BrightGreen);
        print_rst();
        return ERR_NONE;
    }

    if (context.args.has("disable")) {
        std::string hash = context.args.value("disable").value_or("");
        std::string error;
        if (!plugin_manager().disable(hash, error)) {
            print("Failed to disable plugin: " + error + "\n", CLR_BrightRed);
            print_rst();
            return ERR_PERMISSION_DENIED;
        }
        print("Plugin disabled.\n", CLR_BrightGreen);
        print_rst();
        return ERR_NONE;
    }

    print_usage();
    print_rst();
    return ERR_INVALID_OPTION;
}

CommandRegistration registration(
    "plugin",
    "List, enable, or disable SCT DLL plugins.",
    plugin_command_schema(),
    handle_plugin,
    0);

}
