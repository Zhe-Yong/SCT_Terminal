/* Useful tools */
/* Copyright (c) 2026 ZheYong. All rights reserved. */
/* File Version: 0.0.1 */
#pragma once

std::string unix2time(time_t unix_time);
std::string get_uuid_v4();
const std::wstring& get_app_title();

std::string wide_to_utf8(const std::wstring& text);
std::wstring utf8_to_wide(const std::string& text);
std::wstring quote_wide_arg(const std::wstring& arg);

std::string get_current_directory();
bool set_current_directory(const std::string& path);
std::string get_module_path();
std::string get_module_directory();
std::string get_current_user_name();
std::string format_win32_error(DWORD code);

enum class ReadLineResult {
    Line,
    Interrupted,
    EndOfFile,
    Error
};

std::vector<std::string> get_command_line_args();
ReadLineResult read_line_utf8(std::string& line);
void ensure_console_attached();
void setup_console();