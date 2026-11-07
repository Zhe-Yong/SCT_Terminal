#pragma once

#include <stdint.h>

#define SCT_PLUGIN_ABI_VERSION 1u

#define SCT_PLUGIN_SHUTDOWN_MESSAGE_NAME L"SCT.Plugin.Shutdown"
#define SCT_PLUGIN_SHUTDOWN_EXIT 0u
#define SCT_PLUGIN_SHUTDOWN_RESTART 1u
#define SCT_PLUGIN_SHUTDOWN_SYSTEM 2u

#if defined(_WIN32)
#  if defined(SCT_PLUGIN_BUILD)
#    define SCT_PLUGIN_EXPORT __declspec(dllexport)
#  else
#    define SCT_PLUGIN_EXPORT __declspec(dllimport)
#  endif
#  if defined(_M_IX86) || defined(__i386__)
#    define SCT_PLUGIN_CALL __cdecl
#  else
#    define SCT_PLUGIN_CALL
#  endif
#else
#  define SCT_PLUGIN_EXPORT
#  define SCT_PLUGIN_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SctOptionValueMode {
    SCT_OPTION_FLAG = 0,
    SCT_OPTION_REQUIRED = 1,
    SCT_OPTION_OPTIONAL = 2
} SctOptionValueMode;

typedef enum SctErrorCode {
    SCT_ERR_NONE = 0x0000,
    SCT_ERR_UNKNOWN = 0x0001,
    SCT_ERR_EXPIRED = 0x0002,
    SCT_ERR_INVALID_COMMAND = 0x0003,
    SCT_ERR_PERMISSION_DENIED = 0x0004,
    SCT_ERR_FILE_NOT_FOUND = 0x0005,
    SCT_ERR_OUT_OF_MEMORY = 0x0006,
    SCT_ERR_SELFCHECK_FAILED = 0x0007,
    SCT_ERR_TOO_MANY_ARGUMENTS = 0x0008,
    SCT_ERR_AUTH_FAILED = 0x0009,
    SCT_ERR_TOO_FEW_ARGUMENTS = 0x000A,
    SCT_ERR_INVALID_OPTION = 0x000B,
    SCT_ERR_DUPLICATE_OPTION = 0x000C,
    SCT_ERR_CANCELLED = 0x000D,
    SCT_ERR_DANGEROUS_OPERATION = 0x000E,
    SCT_ERR_IS_A_DIRECTORY = 0x000F
} SctErrorCode;

#define SCT_CLR_NONE  (-1)
#define SCT_CLR_RESET 0
#define SCT_CLR_BOLD 1
#define SCT_CLR_DIM 2
#define SCT_CLR_ITALIC 3
#define SCT_CLR_UNDERLINE 4
#define SCT_CLR_BLINK 5
#define SCT_CLR_REVERSE 7
#define SCT_CLR_HIDDEN 8
#define SCT_CLR_STRIKE 9

#define SCT_CLR_BLACK 30
#define SCT_CLR_RED 31
#define SCT_CLR_GREEN 32
#define SCT_CLR_YELLOW 33
#define SCT_CLR_BLUE 34
#define SCT_CLR_MAGENTA 35
#define SCT_CLR_CYAN 36
#define SCT_CLR_WHITE 37
#define SCT_CLR_GRAY 90
#define SCT_CLR_BRIGHT_RED 91
#define SCT_CLR_BRIGHT_GREEN 92
#define SCT_CLR_BRIGHT_YELLOW 93
#define SCT_CLR_BRIGHT_BLUE 94
#define SCT_CLR_BRIGHT_MAGENTA 95
#define SCT_CLR_BRIGHT_CYAN 96
#define SCT_CLR_BRIGHT_WHITE 97

#define SCT_CLR_BG_BLACK 40
#define SCT_CLR_BG_RED 41
#define SCT_CLR_BG_GREEN 42
#define SCT_CLR_BG_YELLOW 43
#define SCT_CLR_BG_BLUE 44
#define SCT_CLR_BG_MAGENTA 45
#define SCT_CLR_BG_CYAN 46
#define SCT_CLR_BG_WHITE 47
#define SCT_CLR_BG_GRAY 100
#define SCT_CLR_BG_BRIGHT_RED 101
#define SCT_CLR_BG_BRIGHT_GREEN 102
#define SCT_CLR_BG_BRIGHT_YELLOW 103
#define SCT_CLR_BG_BRIGHT_BLUE 104
#define SCT_CLR_BG_BRIGHT_MAGENTA 105
#define SCT_CLR_BG_BRIGHT_CYAN 106
#define SCT_CLR_BG_BRIGHT_WHITE 107

typedef struct SctCommandOption {
    const char* name;
    uint32_t value_mode;
} SctCommandOption;

typedef struct SctCommandSchema {
    const SctCommandOption* options;
    uint32_t option_count;
    uint32_t min_positionals;
    uint32_t max_positionals;
    uint32_t raw_tail;
} SctCommandSchema;

typedef struct SctCommandContext SctCommandContext;
typedef int (SCT_PLUGIN_CALL *SctCommandHandler)(SctCommandContext* context);

typedef struct SctCommandDescriptor {
    const char* name;
    const char* description;
    SctCommandSchema schema;
    int32_t require_elevation;
    SctCommandHandler handler;
} SctCommandDescriptor;

typedef struct SctHostApi {
    uint32_t abi_version;
    uint32_t struct_size;
    void* host;

    int (SCT_PLUGIN_CALL *register_commands)(
        void* host,
        const SctCommandDescriptor* commands,
        uint32_t command_count);

    void (SCT_PLUGIN_CALL *write_utf8)(
        void* host,
        const char* text,
        uint32_t length);

    void (SCT_PLUGIN_CALL *set_color)(
        void* host,
        int32_t fg,
        int32_t bg,
        int32_t style);

    void (SCT_PLUGIN_CALL *reset_color)(void* host);

    int (SCT_PLUGIN_CALL *has_option)(void* host, const char* name);
    const char* (SCT_PLUGIN_CALL *option_value)(void* host, const char* name);
    uint32_t (SCT_PLUGIN_CALL *positional_count)(void* host);
    const char* (SCT_PLUGIN_CALL *positional)(void* host, uint32_t index);
    const char* (SCT_PLUGIN_CALL *raw_line)(void* host);
    int (SCT_PLUGIN_CALL *is_cancelled)(void* host);
    void (SCT_PLUGIN_CALL *request_exit)(void* host);
} SctHostApi;

struct SctCommandContext {
    const SctHostApi* api;
    void* host;
};

typedef uint32_t (SCT_PLUGIN_CALL *SctPluginAbiVersionFn)(void);
typedef const char* (SCT_PLUGIN_CALL *SctPluginDllVersionFn)(void);
typedef int (SCT_PLUGIN_CALL *SctPluginRegisterFn)(const SctHostApi* api);

SCT_PLUGIN_EXPORT uint32_t SCT_PLUGIN_CALL sct_plugin_abi_version(void);
SCT_PLUGIN_EXPORT const char* SCT_PLUGIN_CALL sct_plugin_dll_ver(void);
SCT_PLUGIN_EXPORT int SCT_PLUGIN_CALL sct_plugin_register(const SctHostApi* api);

#ifdef __cplusplus
}
#endif
