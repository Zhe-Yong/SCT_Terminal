#include "global.h"

namespace {

struct PluginRecord {
    std::wstring path;
    std::string file_name;
    std::string sha256;
    PluginSignatureInfo signature;
    bool enabled = false;
    bool loaded = false;
    HMODULE module = nullptr;
    SctHostApi* api = nullptr;
    void* host = nullptr;
    std::string error;
};

thread_local CommandContext* plugin_command_context = nullptr;

std::string architecture_name() {
#ifdef _WIN64
    return "x64";
#else
    return "x86";
#endif
}

bool is_hex_hash(const std::string& text) {
    if (text.size() != 64) return false;
    for (char ch : text) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
              (ch >= 'A' && ch <= 'F'))) {
            return false;
        }
    }
    return true;
}

std::string normalize_hash(const std::string& text) {
    return lower_ascii(text);
}

std::wstring join_path(const std::wstring& left, const std::wstring& right) {
    if (left.empty()) return right;
    if (left.back() == L'\\' || left.back() == L'/') return left + right;
    return left + L"\\" + right;
}

bool is_directory(const std::wstring& path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::vector<std::wstring> list_plugin_files(const std::wstring& directory) {
    std::vector<std::wstring> result;
    if (directory.empty() || !is_directory(directory)) return result;

    WIN32_FIND_DATAW data{};
    std::wstring pattern = join_path(directory, L"*.dll");
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return result;

    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        result.push_back(join_path(directory, data.cFileName));
    } while (FindNextFileW(find, &data));

    FindClose(find);
    std::sort(result.begin(), result.end());
    return result;
}

FARPROC get_plugin_export(HMODULE module, const char* name) {
    FARPROC address = GetProcAddress(module, name);
    if (address) return address;
    std::string decorated = std::string("_") + name;
    address = GetProcAddress(module, decorated.c_str());
    if (address) return address;
    std::string stdcall = decorated + "@0";
    return GetProcAddress(module, stdcall.c_str());
}

template <typename Function>
Function get_typed_plugin_export(HMODULE module, const char* name) {
    FARPROC address = get_plugin_export(module, name);
    Function function = nullptr;
    static_assert(sizeof(function) == sizeof(address));
    std::memcpy(&function, &address, sizeof(function));
    return function;
}

const ParsedOption* find_option(const CommandArguments& arguments,
                                const std::string& name) {
    std::string normalized = lower_ascii(name);
    for (const ParsedOption& option : arguments.options()) {
        if (option.name == normalized) return &option;
    }
    return nullptr;
}

int register_commands_for_host(void* host,
                               const SctCommandDescriptor* commands,
                               uint32_t command_count,
                               std::string& error);

int SCT_PLUGIN_CALL host_register_commands(void* host,
                                           const SctCommandDescriptor* commands,
                                           uint32_t command_count) {
    std::string error;
    return register_commands_for_host(host, commands, command_count, error);
}

void SCT_PLUGIN_CALL host_write_utf8(void* host,
                                     const char* text,
                                     uint32_t length) {
    (void)host;
    if (!plugin_command_context || !text || length == 0) return;
    plugin_command_context->out.write(text, static_cast<std::streamsize>(length));
}

void SCT_PLUGIN_CALL host_set_color(void* host,
                                    int32_t fg,
                                    int32_t bg,
                                    int32_t style) {
    (void)host;
    if (!plugin_command_context) return;
    color_set(plugin_command_context->out, fg, bg, style);
}

void SCT_PLUGIN_CALL host_reset_color(void* host) {
    (void)host;
    if (!plugin_command_context) return;
    print_rst(plugin_command_context->out);
}

int SCT_PLUGIN_CALL host_has_option(void* host, const char* name) {
    (void)host;
    if (!plugin_command_context || !name) return 0;
    return plugin_command_context->args.has(name) ? 1 : 0;
}

const char* SCT_PLUGIN_CALL host_option_value(void* host, const char* name) {
    (void)host;
    if (!plugin_command_context || !name) return nullptr;
    const ParsedOption* option =
        find_option(plugin_command_context->args, name);
    if (!option || !option->value.has_value()) return nullptr;
    return option->value->c_str();
}

uint32_t SCT_PLUGIN_CALL host_positional_count(void* host) {
    (void)host;
    if (!plugin_command_context) return 0;
    return static_cast<uint32_t>(
        plugin_command_context->args.positional_count());
}

const char* SCT_PLUGIN_CALL host_positional(void* host, uint32_t index) {
    (void)host;
    if (!plugin_command_context) return nullptr;
    const std::vector<std::string>& positionals =
        plugin_command_context->args.positionals();
    if (index >= positionals.size()) return nullptr;
    return positionals[index].c_str();
}

const char* SCT_PLUGIN_CALL host_raw_line(void* host) {
    (void)host;
    if (!plugin_command_context) return "";
    return plugin_command_context->raw_line.c_str();
}

int SCT_PLUGIN_CALL host_is_cancelled(void* host) {
    (void)host;
    return command_cancelled() ? 1 : 0;
}

void SCT_PLUGIN_CALL host_request_exit(void* host) {
    (void)host;
    if (plugin_command_context) {
        plugin_command_context->exit_requested = true;
    }
}

int invoke_plugin_command(const std::shared_ptr<PluginRecord>& record,
                          SctCommandHandler handler,
                          CommandContext& context) {
    CommandContext* previous = plugin_command_context;
    plugin_command_context = &context;
    SctCommandContext plugin_context{record->api, record->host};
    int result = handler(&plugin_context);
    plugin_command_context = previous;
    return result;
}

}

struct PluginManager::Impl {
    PluginManager* owner = nullptr;
    std::wstring executable_directory;
    std::wstring plugin_directory;
    std::wstring state_path;
    std::string plugin_directory_utf8;
    std::string state_path_utf8;
    PluginState state;
    bool initialized = false;
    bool state_ok = false;
    std::string state_error;
    std::map<std::string, std::shared_ptr<PluginRecord>> records;
    std::vector<std::string> order;
    std::shared_ptr<PluginRecord> loading;
    SctHostApi api{};
    std::string registration_error;

    std::shared_ptr<PluginRecord> inspect(const std::wstring& path) {
        auto record = std::make_shared<PluginRecord>();
        record->path = path;
        size_t separator = path.find_last_of(L"\\/");
        record->file_name = wide_to_utf8(
            separator == std::wstring::npos ? path : path.substr(separator + 1));

        std::string error;
        if (!compute_file_sha256(path, record->sha256, error)) {
            record->error = error;
            return record;
        }
        verify_plugin_signature(path, record->signature, error);
        if (!error.empty()) record->error = error;
        return record;
    }

    bool load_plugin(const std::shared_ptr<PluginRecord>& record,
                     std::string& error) {
        if (record->loaded) return true;
        if (!record->error.empty()) {
            error = record->error;
            return false;
        }

        HMODULE module = LoadLibraryExW(
            record->path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module) {
            error = "Failed to load " + record->file_name + ": " +
                    format_win32_error(GetLastError());
            return false;
        }

        auto abi_version = get_typed_plugin_export<SctPluginAbiVersionFn>(
            module, "sct_plugin_abi_version");
        auto register_plugin = get_typed_plugin_export<SctPluginRegisterFn>(
            module, "sct_plugin_register");
        if (!abi_version || !register_plugin) {
            FreeLibrary(module);
            error = record->file_name + " does not export the SCT plugin ABI.";
            return false;
        }

        if (abi_version() != SCT_PLUGIN_ABI_VERSION) {
            FreeLibrary(module);
            error = record->file_name + " uses an unsupported plugin ABI.";
            return false;
        }

        record->api = &api;
        record->host = this;
        loading = record;
        registration_error.clear();
        int result = register_plugin(&api);
        loading.reset();

        if (result != ERR_NONE) {
            command_registry().remove_owner(record->sha256);
            FreeLibrary(module);
            error = registration_error.empty()
                        ? record->file_name + " failed to register commands."
                        : registration_error;
            return false;
        }

        record->module = module;
        record->loaded = true;
        return true;
    }

    void unload_plugin(const std::shared_ptr<PluginRecord>& record) {
        command_registry().remove_owner(record->sha256);
        if (record->module) {
            FreeLibrary(record->module);
            record->module = nullptr;
        }
        record->loaded = false;
        record->api = nullptr;
        record->host = nullptr;
    }

    std::shared_ptr<PluginRecord> find_or_inspect(const std::string& hash,
                                                  std::string& error) {
        auto it = records.find(hash);
        if (it != records.end()) return it->second;

        for (const std::wstring& path : list_plugin_files(plugin_directory)) {
            auto record = inspect(path);
            if (record->sha256 == hash) {
                records[hash] = record;
                order.push_back(hash);
                return record;
            }
        }

        error = "No plugin with SHA-256 " + hash + " was found.";
        return nullptr;
    }
};

namespace {

int register_commands_for_host(void* host,
                               const SctCommandDescriptor* commands,
                               uint32_t command_count,
                               std::string& error) {
    auto* impl = static_cast<PluginManager::Impl*>(host);
    if (!impl) {
        error = "Invalid plugin host.";
        return ERR_INVALID_COMMAND;
    }
    int result =
        impl->owner->register_plugin_commands(commands, command_count, error);
    if (result != ERR_NONE) impl->registration_error = error;
    return result;
}

}

PluginManager::PluginManager() : impl_(std::make_unique<Impl>()) {
    impl_->owner = this;
    impl_->api.abi_version = SCT_PLUGIN_ABI_VERSION;
    impl_->api.struct_size = sizeof(SctHostApi);
    impl_->api.host = impl_.get();
    impl_->api.register_commands = &host_register_commands;
    impl_->api.write_utf8 = &host_write_utf8;
    impl_->api.set_color = &host_set_color;
    impl_->api.reset_color = &host_reset_color;
    impl_->api.has_option = &host_has_option;
    impl_->api.option_value = &host_option_value;
    impl_->api.positional_count = &host_positional_count;
    impl_->api.positional = &host_positional;
    impl_->api.raw_line = &host_raw_line;
    impl_->api.is_cancelled = &host_is_cancelled;
    impl_->api.request_exit = &host_request_exit;
}

PluginManager::~PluginManager() = default;

bool PluginManager::initialize(const std::string& executable_directory,
                               std::string& error,
                               bool emit_warnings) {
    if (impl_->initialized) return impl_->state_ok;

    impl_->executable_directory = utf8_to_wide(executable_directory);
    while (impl_->executable_directory.size() > 1 &&
           (impl_->executable_directory.back() == L'\\' ||
            impl_->executable_directory.back() == L'/')) {
        impl_->executable_directory.pop_back();
    }

    impl_->plugin_directory = join_path(
        join_path(impl_->executable_directory, L"plugin"),
        utf8_to_wide(architecture_name()));
    impl_->state_path =
        join_path(impl_->executable_directory, L"plugin-state.dat");
    impl_->plugin_directory_utf8 = wide_to_utf8(impl_->plugin_directory);
    impl_->state_path_utf8 = wide_to_utf8(impl_->state_path);

    std::string state_error;
    if (!load_plugin_state(impl_->state_path, impl_->state, state_error)) {
        impl_->state_error = state_error;
        impl_->state_ok = false;
        impl_->initialized = true;
        error = state_error;
        return false;
    }
    impl_->state_ok = true;

    for (const std::wstring& path : list_plugin_files(impl_->plugin_directory)) {
        auto record = impl_->inspect(path);
        if (record->sha256.empty()) {
            if (emit_warnings && !record->error.empty()) {
                print("Plugin " + record->file_name + ": " + record->error +
                          "\n",
                      CLR_BrightYellow);
            }
            continue;
        }

        std::string hash = record->sha256;
        bool explicitly_enabled = impl_->state.enabled.find(hash) !=
                                  impl_->state.enabled.end();
        bool explicitly_disabled = impl_->state.disabled.find(hash) !=
                                   impl_->state.disabled.end();
        record->enabled = !explicitly_disabled &&
                          (explicitly_enabled || record->signature.trusted);

        if (record->enabled) {
            std::string load_error;
            if (!impl_->load_plugin(record, load_error)) {
                record->error = load_error;
            }
        }

        if (emit_warnings && !record->error.empty()) {
            print("Plugin " + record->file_name + ": " + record->error + "\n",
                  CLR_BrightYellow);
        }

        impl_->records[hash] = record;
        impl_->order.push_back(hash);
    }

    impl_->initialized = true;
    return true;
}

bool PluginManager::enable(const std::string& sha256, std::string& error) {
    if (!impl_->initialized) {
        error = "Plugin manager is not initialized.";
        return false;
    }
    if (!impl_->state_ok) {
        error = impl_->state_error.empty() ? "Plugin state is unavailable."
                                           : impl_->state_error;
        return false;
    }

    std::string hash = normalize_hash(sha256);
    if (!is_hex_hash(hash)) {
        error = "Plugin SHA-256 must be 64 hexadecimal characters.";
        return false;
    }

    auto record = impl_->find_or_inspect(hash, error);
    if (!record) return false;
    if (!record->error.empty() && !record->loaded) {
        error = record->error;
        return false;
    }

    PluginState previous = impl_->state;
    impl_->state.disabled.erase(hash);
    impl_->state.enabled.insert(hash);
    if (!save_plugin_state(impl_->state_path, impl_->state, error)) {
        impl_->state = std::move(previous);
        return false;
    }

    record->enabled = true;
    if (!record->loaded) {
        std::string load_error;
        if (!impl_->load_plugin(record, load_error)) {
            impl_->state = std::move(previous);
            std::string ignored;
            save_plugin_state(impl_->state_path, impl_->state, ignored);
            record->enabled = false;
            error = load_error;
            return false;
        }
    }

    return true;
}

bool PluginManager::disable(const std::string& sha256, std::string& error) {
    if (!impl_->initialized) {
        error = "Plugin manager is not initialized.";
        return false;
    }
    if (!impl_->state_ok) {
        error = impl_->state_error.empty() ? "Plugin state is unavailable."
                                           : impl_->state_error;
        return false;
    }

    std::string hash = normalize_hash(sha256);
    if (!is_hex_hash(hash)) {
        error = "Plugin SHA-256 must be 64 hexadecimal characters.";
        return false;
    }

    auto it = impl_->records.find(hash);
    bool known = it != impl_->records.end();
    if (!known) {
        known = impl_->state.enabled.find(hash) != impl_->state.enabled.end() ||
                impl_->state.disabled.find(hash) != impl_->state.disabled.end();
    }
    if (!known) {
        error = "No plugin with SHA-256 " + hash + " was found.";
        return false;
    }

    PluginState previous = impl_->state;
    impl_->state.enabled.erase(hash);
    impl_->state.disabled.insert(hash);
    if (!save_plugin_state(impl_->state_path, impl_->state, error)) {
        impl_->state = std::move(previous);
        return false;
    }

    if (it != impl_->records.end()) {
        it->second->enabled = false;
        if (it->second->loaded) impl_->unload_plugin(it->second);
    }
    return true;
}

std::vector<PluginInfo> PluginManager::list() const {
    std::vector<PluginInfo> result;
    for (const std::string& hash : impl_->order) {
        auto it = impl_->records.find(hash);
        if (it == impl_->records.end()) continue;
        const PluginRecord& record = *it->second;
        PluginInfo info;
        info.file_name = record.file_name;
        info.sha256 = record.sha256;
        info.signer_subject = record.signature.signer_subject;
        info.signer_spki_sha256 = record.signature.signer_spki_sha256;
        info.signed_file = record.signature.signed_file;
        info.signature_valid = record.signature.signature_valid;
        info.trusted = record.signature.trusted;
        info.enabled = record.enabled;
        info.loaded = record.loaded;
        info.error = record.error;
        result.push_back(std::move(info));
    }
    return result;
}

bool PluginManager::initialized() const {
    return impl_->initialized;
}

const std::string& PluginManager::state_path() const {
    return impl_->state_path_utf8;
}

const std::string& PluginManager::plugin_directory() const {
    return impl_->plugin_directory_utf8;
}

int PluginManager::register_plugin_commands(
    const SctCommandDescriptor* commands,
    uint32_t command_count,
    std::string& error) {
    if (!impl_->loading) {
        error = "Commands may only be registered during plugin startup.";
        return ERR_INVALID_COMMAND;
    }
    if (!commands || command_count == 0) {
        error = "Plugin registered no commands.";
        return ERR_INVALID_COMMAND;
    }
    if (command_count > 256) {
        error = "Plugin registered too many commands.";
        return ERR_INVALID_COMMAND;
    }

    auto record = impl_->loading;
    CommandRegistry& registry = command_registry();

    for (uint32_t index = 0; index < command_count; ++index) {
        const SctCommandDescriptor& descriptor = commands[index];
        if (!descriptor.name || descriptor.name[0] == '\0' ||
            !descriptor.handler) {
            registry.remove_owner(record->sha256);
            error = "Plugin registered an invalid command.";
            return ERR_INVALID_COMMAND;
        }
        if (descriptor.schema.option_count > 128 ||
            (descriptor.schema.option_count > 0 &&
             !descriptor.schema.options)) {
            registry.remove_owner(record->sha256);
            error = "Plugin command '" + std::string(descriptor.name) +
                    "' has an invalid option table.";
            return ERR_INVALID_COMMAND;
        }

        CommandSchema schema;
        schema.min_positionals = descriptor.schema.min_positionals;
        schema.max_positionals = descriptor.schema.max_positionals;
        schema.raw_tail = descriptor.schema.raw_tail != 0;
        for (uint32_t option_index = 0;
             option_index < descriptor.schema.option_count; ++option_index) {
            const SctCommandOption& source =
                descriptor.schema.options[option_index];
            if (!source.name || source.name[0] == '\0') {
                registry.remove_owner(record->sha256);
                error = "Plugin command '" + std::string(descriptor.name) +
                        "' has an unnamed option.";
                return ERR_INVALID_COMMAND;
            }
            CommandOption option;
            option.name = source.name;
            switch (source.value_mode) {
                case SCT_OPTION_FLAG:
                    option.value_mode = OptionValueMode::Flag;
                    break;
                case SCT_OPTION_REQUIRED:
                    option.value_mode = OptionValueMode::Required;
                    break;
                case SCT_OPTION_OPTIONAL:
                    option.value_mode = OptionValueMode::Optional;
                    break;
                default:
                    registry.remove_owner(record->sha256);
                    error = "Plugin command '" + std::string(descriptor.name) +
                            "' has an invalid option mode.";
                    return ERR_INVALID_COMMAND;
            }
            schema.options.push_back(std::move(option));
        }

        SctCommandHandler plugin_handler = descriptor.handler;
        CommandHandler handler =
            [record, plugin_handler](CommandContext& context) {
                return invoke_plugin_command(record, plugin_handler, context);
            };
        if (!registry.try_add(
                descriptor.name,
                descriptor.description ? descriptor.description : "",
                std::move(schema), std::move(handler),
                descriptor.require_elevation, record->sha256, error)) {
            registry.remove_owner(record->sha256);
            return ERR_INVALID_COMMAND;
        }
    }

    return ERR_NONE;
}

PluginManager& plugin_manager() {
    static PluginManager manager;
    return manager;
}

CommandSchema plugin_command_schema() {
    CommandSchema schema;
    schema.options.push_back(CommandOption{"list", OptionValueMode::Flag});
    schema.options.push_back(CommandOption{"enable", OptionValueMode::Required});
    schema.options.push_back(CommandOption{"disable", OptionValueMode::Required});
    schema.max_positionals = 0;
    return schema;
}
