#pragma once

struct AutoexecResult {
    bool exit_requested = false;
    bool rejected = false;
    size_t error_line = 0;
    std::string message;
    int exit_code = 0;
};

AutoexecResult run_autoexec_file();
