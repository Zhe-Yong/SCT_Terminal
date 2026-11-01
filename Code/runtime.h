#pragma once

enum class RuntimeBootstrapResult {
    NotNeeded,
    Launched,
    Failed
};

RuntimeBootstrapResult bootstrap_random_runtime(
    const std::vector<std::string>& args, std::string& error);
