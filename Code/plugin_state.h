#pragma once

struct PluginState {
    std::set<std::string> enabled;
    std::set<std::string> disabled;
};

bool load_plugin_state(const std::wstring& path,
                       PluginState& state,
                       std::string& error);
bool save_plugin_state(const std::wstring& path,
                       const PluginState& state,
                       std::string& error);
