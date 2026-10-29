#pragma once

#include "sdk/sct_plugin.h"

struct PluginInfo {
    std::string file_name;
    std::string sha256;
    std::string signer_subject;
    std::string signer_spki_sha256;
    bool signed_file = false;
    bool signature_valid = false;
    bool trusted = false;
    bool enabled = false;
    bool loaded = false;
    std::string error;
};

class PluginManager {
public:
    struct Impl;

    PluginManager();
    ~PluginManager();

    bool initialize(const std::string& executable_directory,
                    std::string& error,
                    bool emit_warnings);
    bool enable(const std::string& sha256, std::string& error);
    bool disable(const std::string& sha256, std::string& error);
    std::vector<PluginInfo> list() const;
    bool initialized() const;
    const std::string& state_path() const;
    const std::string& plugin_directory() const;

    int register_plugin_commands(const SctCommandDescriptor* commands,
                                 uint32_t command_count,
                                 std::string& error);

private:
    std::unique_ptr<Impl> impl_;
};

PluginManager& plugin_manager();
CommandSchema plugin_command_schema();
