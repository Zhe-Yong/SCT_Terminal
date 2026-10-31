#include "global.h"

std::string lower_ascii(const std::string& text) {
    std::string result = text;
    for (char& ch : result) {
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
    }
    return result;
}

std::vector<std::string> split_command_line(const std::string& line) {
    std::vector<std::string> result;
    std::string current;
    bool in_quotes = false;
    bool has_token = false;

    for (char ch : line) {
        if (ch == '"') {
            in_quotes = !in_quotes;
            has_token = true;
            continue;
        }
        if (!in_quotes && (ch == ' ' || ch == '\t')) {
            if (has_token) {
                result.push_back(current);
                current.clear();
                has_token = false;
            }
            continue;
        }
        current.push_back(ch);
        has_token = true;
    }

    if (has_token) result.push_back(current);
    return result;
}

std::string rest_after_first_token(const std::string& line) {
    size_t pos = 0;
    while (pos < line.size() && line[pos] != ' ' && line[pos] != '\t') ++pos;
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    return line.substr(pos);
}

namespace {

class CommandAborted : public std::exception {
public:
    const char* what() const noexcept override {
        return "Command cancelled.";
    }
};

class PipeStreamBuf : public std::streambuf {
public:
    explicit PipeStreamBuf(HANDLE handle) : handle_(handle) {}

protected:
    int_type overflow(int_type ch) override {
        if (traits_type::eq_int_type(ch, traits_type::eof())) {
            return traits_type::not_eof(ch);
        }
        if (command_cancelled()) throw CommandAborted();
        char value = traits_type::to_char_type(ch);
        return write_bytes(&value, 1) ? ch : traits_type::eof();
    }

    std::streamsize xsputn(const char* data, std::streamsize size) override {
        if (size <= 0) return 0;
        if (command_cancelled()) throw CommandAborted();
        DWORD length = static_cast<DWORD>(size);
        return write_bytes(data, length) ? size : 0;
    }

    int sync() override {
        if (command_cancelled()) throw CommandAborted();
        return 0;
    }

private:
    bool write_bytes(const char* data, DWORD length) {
        while (length > 0) {
            DWORD written = 0;
            if (!WriteFile(handle_, data, length, &written, nullptr) ||
                written == 0) {
                return false;
            }
            data += written;
            length -= written;
        }
        return true;
    }

    HANDLE handle_;
};

struct CommandRunState {
    const Command* command = nullptr;
    std::string raw_line;
    CommandArguments args;
    CommandOrigin origin = CommandOrigin::Interactive;
    std::string error;
    std::atomic<int> exit_code{ERR_UNKNOWN};
    std::atomic<bool> exit_requested{false};
};

struct CommandWorkerParameter {
    CommandRunState* state = nullptr;
    HANDLE output = INVALID_HANDLE_VALUE;
};

thread_local bool inside_command_worker = false;

DWORD WINAPI command_worker(void* parameter) {
    auto* worker = static_cast<CommandWorkerParameter*>(parameter);
    PipeStreamBuf buffer(worker->output);
    std::ostream output(&buffer);
    output.exceptions(std::ios::badbit);

    inside_command_worker = true;

    try {
        CommandContext context{output};
        context.raw_line = worker->state->raw_line;
        context.args = worker->state->args;
        context.origin = worker->state->origin;
        int code = worker->state->command->handler(context);
        worker->state->exit_requested = context.exit_requested;
        worker->state->exit_code = code;
    } catch (const CommandAborted&) {
        worker->state->exit_code = ERR_CANCELLED;
    } catch (const std::exception& exception) {
        if (command_cancelled()) {
            worker->state->exit_code = ERR_CANCELLED;
        } else {
            worker->state->error =
                std::string("Command failed: ") + exception.what() + "\n";
            worker->state->exit_code = ERR_UNKNOWN;
        }
    } catch (...) {
        worker->state->error = "Command failed.\n";
        worker->state->exit_code = ERR_UNKNOWN;
    }

    return 0;
}

void read_pipe_output(HANDLE pipe, std::ostream& output) {
    char buffer[4096];
    while (true) {
        DWORD read = 0;
        if (!ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) ||
            read == 0) {
            break;
        }
        output.write(buffer, read);
        output.flush();
    }
}

int privilege_rank(PrivilegeLevel level) {
    switch (level) {
        case PrivilegeLevel::User:
            return 1;
        case PrivilegeLevel::Administrator:
            return 2;
        case PrivilegeLevel::System:
            return 3;
        case PrivilegeLevel::TrustedInstaller:
            return 4;
    }
    return 0;
}

bool check_command_elevation(CommandContext& context, const Command& command) {
    if (command.require_elevation <= 0) return true;

    PrivilegeLevel current = get_process_privilege_level();
    if (privilege_rank(current) >= command.require_elevation) return true;

    PrivilegeLevel required =
        static_cast<PrivilegeLevel>(command.require_elevation - 1);
    print(context.out,
          "Command '" + command.name + "' requires " +
              std::string(privilege_level_name(required)) +
              " privileges. Current privilege level: " +
              std::string(privilege_level_name(current)) + ".\n",
          CLR_BrightRed);

    const char* hint = nullptr;
    switch (required) {
        case PrivilegeLevel::Administrator:
            hint = "Run 'sudo ad' first or use 'sudo <command>'.\n";
            break;
        case PrivilegeLevel::System:
            hint = "Run 'sudo sys' first or use 'sudo <command>'.\n";
            break;
        case PrivilegeLevel::TrustedInstaller:
            hint = "Run 'sudo su' first or use 'sudo <command>'.\n";
            break;
        default:
            break;
    }
    if (hint) print(context.out, hint, CLR_BrightYellow);

    return false;
}

}

static bool is_option_token(const std::string& token) {
    return token.size() > 1 && (token[0] == '-' || token[0] == '/');
}

static bool split_option_token(const std::string& token,
                               std::string& name,
                               std::optional<std::string>& inline_value) {
    size_t prefix = 1;
    if (token.size() >= 2 && token[0] == '-' && token[1] == '-') prefix = 2;

    std::string rest = token.substr(prefix);
    size_t equals = rest.find('=');
    if (equals == std::string::npos) {
        name = lower_ascii(rest);
        inline_value.reset();
    } else {
        name = lower_ascii(rest.substr(0, equals));
        inline_value = rest.substr(equals + 1);
    }
    return !name.empty();
}

static const CommandOption* find_schema_option(const CommandSchema& schema,
                                               const std::string& name) {
    for (const CommandOption& option : schema.options) {
        if (lower_ascii(option.name) == name) return &option;
    }
    return nullptr;
}

static bool is_known_option_token(const CommandSchema& schema,
                                  const std::string& token) {
    if (token == "--" || !is_option_token(token)) return false;

    std::string name;
    std::optional<std::string> inline_value;
    if (!split_option_token(token, name, inline_value)) return false;
    return find_schema_option(schema, name) != nullptr;
}

static bool parse_failure(int code,
                          const std::string& message,
                          int& error_code,
                          std::string& error) {
    error_code = code;
    error = message;
    return false;
}

bool CommandArguments::parse(const CommandSchema& schema,
                             const std::vector<std::string>& tokens,
                             CommandArguments& arguments,
                             int& error_code,
                             std::string& error) {
    arguments.options_.clear();
    arguments.positionals_.clear();
    error_code = ERR_NONE;
    error.clear();

    if (schema.raw_tail) {
        if (!tokens.empty()) arguments.positionals_.push_back(tokens.front());
        if (arguments.positionals_.size() < schema.min_positionals) {
            return parse_failure(ERR_TOO_FEW_ARGUMENTS,
                                 "Too few arguments.", error_code, error);
        }
        return true;
    }

    bool options_ended = false;

    for (size_t index = 0; index < tokens.size(); ++index) {
        const std::string& token = tokens[index];

        if (!options_ended && token == "--") {
            options_ended = true;
            continue;
        }

        if (options_ended || !is_option_token(token)) {
            arguments.positionals_.push_back(token);
            continue;
        }

        std::string name;
        std::optional<std::string> inline_value;
        if (!split_option_token(token, name, inline_value)) {
            return parse_failure(ERR_INVALID_OPTION,
                                 "Invalid option: " + token + ".",
                                 error_code, error);
        }

        const CommandOption* schema_option = find_schema_option(schema, name);
        if (!schema_option) {
            return parse_failure(ERR_INVALID_OPTION,
                                 "Unknown option: " + token + ".",
                                 error_code, error);
        }

        if (arguments.has(name)) {
            return parse_failure(ERR_DUPLICATE_OPTION,
                                 "Option '" + name +
                                     "' specified more than once.",
                                 error_code, error);
        }

        ParsedOption parsed;
        parsed.name = name;
        parsed.token = token;

        switch (schema_option->value_mode) {
            case OptionValueMode::Flag:
                if (inline_value.has_value()) {
                    return parse_failure(
                        ERR_TOO_MANY_ARGUMENTS,
                        "Too many arguments: option '" + name +
                            "' does not take a value.",
                        error_code, error);
                }
                break;

            case OptionValueMode::Required:
                if (inline_value.has_value()) {
                    if (inline_value->empty()) {
                        return parse_failure(
                            ERR_TOO_FEW_ARGUMENTS,
                            "Too few arguments: option '" + name +
                                "' requires a value.",
                            error_code, error);
                    }
                    parsed.value = inline_value;
                } else if (index + 1 < tokens.size() &&
                           tokens[index + 1] != "--" &&
                           !is_known_option_token(schema, tokens[index + 1])) {
                    parsed.value = tokens[++index];
                } else {
                    return parse_failure(
                        ERR_TOO_FEW_ARGUMENTS,
                        "Too few arguments: option '" + name +
                            "' requires a value.",
                        error_code, error);
                }
                break;

            case OptionValueMode::Optional:
                if (inline_value.has_value()) {
                    parsed.value = inline_value;
                } else if (index + 1 < tokens.size() &&
                           tokens[index + 1] != "--" &&
                           !is_known_option_token(schema, tokens[index + 1])) {
                    parsed.value = tokens[++index];
                }
                break;
        }

        arguments.options_.push_back(std::move(parsed));
    }

    if (arguments.positionals_.size() < schema.min_positionals) {
        return parse_failure(ERR_TOO_FEW_ARGUMENTS,
                             "Too few arguments.", error_code, error);
    }
    if (arguments.positionals_.size() > schema.max_positionals) {
        return parse_failure(ERR_TOO_MANY_ARGUMENTS,
                             "Too many arguments.", error_code, error);
    }

    return true;
}

bool CommandArguments::has(const std::string& option) const {
    std::string name = lower_ascii(option);
    for (const ParsedOption& parsed : options_) {
        if (parsed.name == name) return true;
    }
    return false;
}

std::optional<std::string> CommandArguments::value(
    const std::string& option) const {
    std::string name = lower_ascii(option);
    for (const ParsedOption& parsed : options_) {
        if (parsed.name == name) return parsed.value;
    }
    return std::nullopt;
}

const std::vector<ParsedOption>& CommandArguments::options() const {
    return options_;
}

std::optional<std::string> CommandArguments::positional(size_t index) const {
    if (index >= positionals_.size()) return std::nullopt;
    return positionals_[index];
}

const std::vector<std::string>& CommandArguments::positionals() const {
    return positionals_;
}

size_t CommandArguments::positional_count() const {
    return positionals_.size();
}

void CommandRegistry::add(const std::string& name,
                          const std::string& description,
                          CommandSchema schema,
                          CommandHandler handler,
                          int require_elevation,
                          const std::string& help) {
    Command command;
    command.name = name;
    command.description = description;
    command.help = help;
    command.schema = std::move(schema);
    command.handler = std::move(handler);
    command.require_elevation = std::clamp(require_elevation, 0, 4);
    commands_[lower_ascii(name)] = std::move(command);
}

bool CommandRegistry::try_add(const std::string& name,
                              const std::string& description,
                              CommandSchema schema,
                              CommandHandler handler,
                              int require_elevation,
                              const std::string& owner,
                              std::string& error) {
    std::string key = lower_ascii(name);
    if (key.empty()) {
        error = "Plugin command name is empty.";
        return false;
    }
    if (commands_.find(key) != commands_.end()) {
        error = "Command '" + name + "' is already registered.";
        return false;
    }

    Command command;
    command.name = name;
    command.description = description;
    command.schema = std::move(schema);
    command.handler = std::move(handler);
    command.require_elevation = std::clamp(require_elevation, 0, 4);
    command.owner = owner;
    commands_[key] = std::move(command);
    return true;
}

void CommandRegistry::remove_owner(const std::string& owner) {
    if (owner.empty()) return;
    for (auto it = commands_.begin(); it != commands_.end();) {
        if (it->second.owner == owner) {
            it = commands_.erase(it);
        } else {
            ++it;
        }
    }
}

const Command* CommandRegistry::find(const std::string& name) const {
    auto it = commands_.find(lower_ascii(name));
    if (it == commands_.end()) return nullptr;
    return &it->second;
}

std::vector<const Command*> CommandRegistry::list() const {
    std::vector<const Command*> result;
    result.reserve(commands_.size());
    for (const auto& entry : commands_) result.push_back(&entry.second);
    return result;
}

CommandRegistry& command_registry() {
    static CommandRegistry registry;
    return registry;
}

CommandRegistration::CommandRegistration(const std::string& name,
                                         const std::string& description,
                                         CommandSchema schema,
                                         CommandHandler handler,
                                         int require_elevation,
                                         const std::string& help) {
    command_registry().add(name, description, std::move(schema),
                           std::move(handler), require_elevation, help);
}

int execute_command_line(CommandRegistry& registry,
                         const std::string& line,
                         CommandContext& context) {
    context.raw_line = line;

    std::vector<std::string> tokens = split_command_line(line);
    if (tokens.empty()) return ERR_NONE;

    const Command* command = registry.find(tokens[0]);
    if (!command) {
        print(context.out, "'" + tokens[0] + "' is not recognized as a SCT command.\n",
              CLR_BrightRed);
        return ERR_INVALID_COMMAND;
    }

    if (!check_command_elevation(context, *command)) {
        return ERR_PERMISSION_DENIED;
    }

    std::vector<std::string> arguments(tokens.begin() + 1, tokens.end());
    int error_code = ERR_NONE;
    std::string error;
    if (!CommandArguments::parse(command->schema, arguments, context.args,
                                 error_code, error)) {
        print(context.out, error + "\n", CLR_BrightRed);
        return error_code;
    }

    if (inside_command_worker) {
        try {
            if (command_cancelled()) throw CommandAborted();
            return command->handler(context);
        } catch (const CommandAborted&) {
            throw;
        } catch (const std::exception& error) {
            print(context.out,
                  std::string("Command failed: ") + error.what() + "\n",
                  CLR_BrightRed);
            return ERR_UNKNOWN;
        } catch (...) {
            print(context.out, "Command failed.\n", CLR_BrightRed);
            return ERR_UNKNOWN;
        }
    }

    CommandExecutionGuard guard;
    if (command_cancelled()) {
        print(context.out, "Command cancelled.\n", CLR_BrightYellow);
        return ERR_CANCELLED;
    }

    HANDLE output_read = INVALID_HANDLE_VALUE;
    HANDLE output_write = INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = FALSE;
    if (!CreatePipe(&output_read, &output_write, &attributes, 0)) {
        print(context.out,
              "Failed to create command output pipe: " +
                  format_win32_error(GetLastError()) + "\n",
              CLR_BrightRed);
        return ERR_UNKNOWN;
    }

    std::thread reader;
    try {
        reader = std::thread([output_read, &context]() {
            read_pipe_output(output_read, context.out);
        });
    } catch (const std::exception& error) {
        CloseHandle(output_write);
        CloseHandle(output_read);
        print(context.out,
              std::string("Failed to start command output reader: ") +
                  error.what() + "\n",
              CLR_BrightRed);
        return ERR_UNKNOWN;
    }

    CommandRunState state;
    state.command = command;
    state.raw_line = context.raw_line;
    state.args = context.args;
    state.origin = context.origin;

    CommandWorkerParameter parameter{&state, output_write};
    HANDLE thread =
        CreateThread(nullptr, 0, command_worker, &parameter, 0, nullptr);
    if (!thread) {
        CloseHandle(output_write);
        reader.join();
        CloseHandle(output_read);
        print(context.out,
              "Failed to start command worker: " +
                  format_win32_error(GetLastError()) + "\n",
              CLR_BrightRed);
        return ERR_UNKNOWN;
    }

    bool aborted = false;
    while (true) {
        DWORD wait = WaitForSingleObject(thread, 50);
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_FAILED) {
            aborted = true;
            TerminateThread(thread, 0);
            WaitForSingleObject(thread, INFINITE);
            break;
        }
        if (command_cancelled()) {
            DWORD cooperative = WaitForSingleObject(thread, 250);
            if (cooperative == WAIT_TIMEOUT) {
                TerminateThread(thread, 0);
                WaitForSingleObject(thread, INFINITE);
            }
            aborted = true;
            break;
        }
    }

    CloseHandle(output_write);
    reader.join();
    CloseHandle(output_read);
    CloseHandle(thread);

    int result = state.exit_code.load();
    if (!aborted && result != ERR_CANCELLED) {
        context.exit_requested = state.exit_requested.load();
    }

    if (aborted || result == ERR_CANCELLED) {
        print(context.out, "\nCommand cancelled.\n", CLR_BrightYellow);
        return ERR_CANCELLED;
    }

    if (!state.error.empty()) {
        print(context.out, state.error, CLR_BrightRed);
    }

    return result;
}
