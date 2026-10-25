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
                          CommandHandler handler) {
    Command command;
    command.name = name;
    command.description = description;
    command.schema = std::move(schema);
    command.handler = std::move(handler);
    commands_[lower_ascii(name)] = std::move(command);
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
                                         CommandHandler handler) {
    command_registry().add(name, description, std::move(schema),
                           std::move(handler));
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

    std::vector<std::string> arguments(tokens.begin() + 1, tokens.end());
    int error_code = ERR_NONE;
    std::string error;
    if (!CommandArguments::parse(command->schema, arguments, context.args,
                                 error_code, error)) {
        print(context.out, error + "\n", CLR_BrightRed);
        return error_code;
    }

    try {
        return command->handler(context);
    } catch (const std::exception& error) {
        print(context.out, std::string("Command failed: ") + error.what() + "\n",
              CLR_BrightRed);
        return ERR_UNKNOWN;
    } catch (...) {
        print(context.out, "Command failed.\n", CLR_BrightRed);
        return ERR_UNKNOWN;
    }
}
