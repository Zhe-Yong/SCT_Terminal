#include "global.h"

static HANDLE cancel_event = nullptr;
static std::string cancel_event_name;
static std::atomic<int> command_depth{0};
static std::atomic<bool> prompt_interrupted{false};

static PSECURITY_DESCRIPTOR create_cancel_security_descriptor() {
    std::wstring current_sid = get_process_user_sid();
    if (current_sid.empty()) current_sid = L"OW";

    std::wstring sddl =
        L"D:(A;;GA;;;SY)"
        L"(A;;GA;;;BA)"
        L"(A;;GA;;;S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464)"
        L"(A;;GA;;;" + current_sid + L")";

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
        return nullptr;
    }
    return descriptor;
}

bool initialize_command_cancellation(std::string& error) {
    if (cancel_event) return true;

    PSECURITY_DESCRIPTOR descriptor = create_cancel_security_descriptor();
    if (!descriptor) {
        error = "Failed to create cancellation security descriptor: " +
                format_win32_error(GetLastError());
        return false;
    }

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.lpSecurityDescriptor = descriptor;
    security.bInheritHandle = FALSE;

    std::string name = "Local\\SCT_" + get_uuid_v4() + "_cancel";
    std::wstring wide_name = utf8_to_wide(name);

    HANDLE event = CreateEventW(&security, TRUE, FALSE, wide_name.c_str());
    LocalFree(descriptor);

    if (!event) {
        error = "Failed to create cancellation event: " +
                format_win32_error(GetLastError());
        return false;
    }

    cancel_event = event;
    cancel_event_name = std::move(name);
    return true;
}

bool open_command_cancellation_event(const std::string& event_name,
                                     std::string& error) {
    if (cancel_event) return true;

    std::wstring wide_name = utf8_to_wide(event_name);
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE,
                              wide_name.c_str());
    if (!event) {
        error = "Failed to open cancellation event: " +
                format_win32_error(GetLastError());
        return false;
    }

    cancel_event = event;
    cancel_event_name = event_name;
    return true;
}

void shutdown_command_cancellation() {
    if (cancel_event) {
        CloseHandle(cancel_event);
        cancel_event = nullptr;
    }
    cancel_event_name.clear();
    command_depth = 0;
    prompt_interrupted = false;
}

const std::string& command_cancellation_event_name() {
    return cancel_event_name;
}

static void inject_enter_key() {
    HANDLE input = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
    if (input == INVALID_HANDLE_VALUE) return;

    INPUT_RECORD record{};
    record.EventType = KEY_EVENT;
    record.Event.KeyEvent.bKeyDown = TRUE;
    record.Event.KeyEvent.wRepeatCount = 1;
    record.Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
    record.Event.KeyEvent.wVirtualScanCode =
        static_cast<WORD>(MapVirtualKeyW(VK_RETURN, MAPVK_VK_TO_VSC));
    record.Event.KeyEvent.uChar.UnicodeChar = L'\r';

    DWORD written = 0;
    WriteConsoleInputW(input, &record, 1, &written);
    CloseHandle(input);
}

static BOOL WINAPI fallback_control_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) return TRUE;
    return FALSE;
}

static BOOL WINAPI console_control_handler(DWORD type) {
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) return FALSE;

    if (command_running()) {
        request_command_cancel();
    } else {
        request_prompt_interrupt();
        inject_enter_key();
    }
    return TRUE;
}

bool install_console_cancel_handler() {
    SetConsoleCtrlHandler(fallback_control_handler, TRUE);
    bool installed =
        SetConsoleCtrlHandler(console_control_handler, TRUE) != FALSE;
    if (!installed) {
        SetConsoleCtrlHandler(nullptr, TRUE);
    }
    return installed;
}

void begin_command_execution() {
    command_depth.fetch_add(1);
}

void end_command_execution() {
    int previous = command_depth.fetch_sub(1);
    if (previous <= 1) {
        command_depth = 0;
        if (cancel_event) ResetEvent(cancel_event);
    }
}

bool command_running() {
    return command_depth.load() > 0;
}

bool command_cancelled() {
    return cancel_event &&
           WaitForSingleObject(cancel_event, 0) == WAIT_OBJECT_0;
}

void request_command_cancel() {
    if (cancel_event) SetEvent(cancel_event);
}

void clear_command_cancel() {
    if (cancel_event) ResetEvent(cancel_event);
}

void request_prompt_interrupt() {
    prompt_interrupted = true;
}

bool consume_prompt_interrupt() {
    return prompt_interrupted.exchange(false);
}

CommandExecutionGuard::CommandExecutionGuard() {
    begin_command_execution();
}

CommandExecutionGuard::~CommandExecutionGuard() {
    end_command_execution();
}
