#pragma once

enum class PplProtectionState {
    Unknown,
    Unprotected,
    Protected
};

std::string ppl_default_type();
std::string ppl_dll_path();
void ppl_set_target_pid(DWORD pid);
PplProtectionState ppl_target_protection(std::string& error);
DWORD ppl_target_pid();
int ppl_run_command(const std::string& command, std::string& error);
bool ppl_install_driver(std::string& error);
bool ppl_uninstall_driver(std::string& error);
bool ppl_protect_target(const std::string& type, std::string& error);
bool ppl_unprotect_target(std::string& error);
bool ppl_run_isolated(DWORD target_pid,
                      const std::string& action,
                      const std::string& type,
                      std::string& error);
