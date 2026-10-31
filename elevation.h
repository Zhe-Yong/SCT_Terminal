#pragma once

enum class PrivilegeLevel {
    User,
    Administrator,
    System,
    TrustedInstaller
};

enum class ElevationMode {
    Administrator,
    System,
    TrustedInstaller
};

bool is_process_elevated();
std::wstring get_process_user_sid();
PrivilegeLevel get_process_privilege_level();
const char* privilege_level_name(PrivilegeLevel level);
char privilege_level_tag(PrivilegeLevel level);
int privilege_color(PrivilegeLevel level);

bool launch_elevated_self(ElevationMode mode,
                          const std::string& command_line,
                          std::string& error);
bool relaunch_as_user(std::string& error);
std::vector<DWORD> sct_window_process_ids();
bool wait_for_new_sct_window(const std::vector<DWORD>& existing,
                             DWORD timeout_ms,
                             std::string& error);
