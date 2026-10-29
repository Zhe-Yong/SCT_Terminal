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

struct ElevatedResponse {
    int exit_code = ERR_UNKNOWN;
    PrivilegeLevel level = PrivilegeLevel::User;
    std::string cwd;
    std::string output;
};

class ElevatedSession {
public:
    ~ElevatedSession();

    bool start(ElevationMode mode, std::string& error);
    bool send(const std::string& command_line,
              ElevatedResponse& response,
              std::string& error);
    bool run_once(const std::string& command_line,
                  ElevatedResponse& response,
                  std::string& error);
    void stop(bool adopt_cwd = true);

    bool active() const;
    std::string cwd() const;
    PrivilegeLevel level() const;

private:
    void cleanup();

    HANDLE cmd_pipe_ = INVALID_HANDLE_VALUE;
    HANDLE out_pipe_ = INVALID_HANDLE_VALUE;
    HANDLE process_ = nullptr;
    HANDLE job_ = nullptr;
    bool active_ = false;
    std::string cwd_;
    PrivilegeLevel level_ = PrivilegeLevel::User;
    std::vector<BYTE> session_key_;
    uint64_t command_sequence_ = 1;
};

ElevatedSession& elevated_session();

int run_elevated_helper(const std::string& cmd_pipe_name,
                        const std::string& out_pipe_name,
                        const std::string& cwd,
                        DWORD parent_pid);
