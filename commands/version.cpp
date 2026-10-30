#include "global.h"

namespace {

std::string architecture_name() {
#ifdef _WIN64
    return "x64";
#else
    return "x86";
#endif
}

void print_self_version(std::ostream& out) {
    print(out, "SuperCommandTerminal " + std::string(SCT_VERSION_TEXT) + "\n",
          CLR_BrightCyan);
    print(out, "Build:        " +
                   std::string(SCT_VERSION_STRINGIFY(SCT_BUILD_NUMBER)) + "\n",
          CLR_Gray);
    print(out, "Architecture: " + architecture_name() + "\n", CLR_Gray);
}

}

static int handle_version(CommandContext& context) {
    std::string target = context.args.positional(0).value_or("self");
    if (target.empty() || lower_ascii(target) == "self") {
        print_self_version(context.out);
        return ERR_NONE;
    }

    std::string version;
    std::string error;
    if (!plugin_manager().dll_version(target, version, error)) {
        print(context.out, error + "\n", CLR_BrightRed);
        return ERR_FILE_NOT_FOUND;
    }

    print(context.out, target + ": " + version + "\n", CLR_BrightCyan);
    return ERR_NONE;
}

static CommandSchema make_version_schema() {
    CommandSchema schema;
    schema.max_positionals = 1;
    return schema;
}

static const CommandRegistration register_version_command(
    "version",
    "Show the SCT version or a plugin DLL version.",
    make_version_schema(),
    handle_version);
