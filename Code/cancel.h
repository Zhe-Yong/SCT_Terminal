#pragma once

bool initialize_command_cancellation(std::string& error);
bool open_command_cancellation_event(const std::string& event_name,
                                     std::string& error);
void shutdown_command_cancellation();
const std::string& command_cancellation_event_name();

bool install_console_cancel_handler();

void begin_command_execution();
void end_command_execution();
bool command_running();
bool command_cancelled();
void request_command_cancel();
void clear_command_cancel();
void request_prompt_interrupt();
bool consume_prompt_interrupt();

class CommandExecutionGuard {
public:
    CommandExecutionGuard();
    ~CommandExecutionGuard();

    CommandExecutionGuard(const CommandExecutionGuard&) = delete;
    CommandExecutionGuard& operator=(const CommandExecutionGuard&) = delete;
};
