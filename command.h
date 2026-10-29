#pragma once

enum class OptionValueMode {
    Flag,
    Required,
    Optional
};

struct CommandOption {
    std::string name;
    OptionValueMode value_mode = OptionValueMode::Flag;
};

struct CommandSchema {
    std::vector<CommandOption> options;
    size_t min_positionals = 0;
    size_t max_positionals = 0;
    bool raw_tail = false;
};

struct ParsedOption {
    std::string name;
    std::string token;
    std::optional<std::string> value;
};

class CommandArguments {
public:
    static bool parse(const CommandSchema& schema,
                      const std::vector<std::string>& tokens,
                      CommandArguments& arguments,
                      int& error_code,
                      std::string& error);

    bool has(const std::string& option) const;
    std::optional<std::string> value(const std::string& option) const;
    const std::vector<ParsedOption>& options() const;
    std::optional<std::string> positional(size_t index) const;
    const std::vector<std::string>& positionals() const;
    size_t positional_count() const;

private:
    std::vector<ParsedOption> options_;
    std::vector<std::string> positionals_;
};

enum class CommandOrigin {
    Interactive,
    Autoexec
};

struct CommandContext {
    explicit CommandContext(std::ostream& output) : out(output) {}

    std::ostream& out;
    bool exit_requested = false;
    std::string raw_line;
    CommandArguments args;
    CommandOrigin origin = CommandOrigin::Interactive;
};

using CommandHandler = std::function<int(CommandContext&)>;

struct Command {
    std::string name;
    std::string description;
    CommandSchema schema;
    CommandHandler handler;
    int require_elevation = 0;
    std::string owner;
};

class CommandRegistry {
public:
    void add(const std::string& name,
             const std::string& description,
             CommandSchema schema,
             CommandHandler handler,
             int require_elevation = 0);
    bool try_add(const std::string& name,
                 const std::string& description,
                 CommandSchema schema,
                 CommandHandler handler,
                 int require_elevation,
                 const std::string& owner,
                 std::string& error);
    void remove_owner(const std::string& owner);
    const Command* find(const std::string& name) const;
    std::vector<const Command*> list() const;

private:
    std::map<std::string, Command> commands_;
};

class CommandRegistration {
public:
    CommandRegistration(const std::string& name,
                        const std::string& description,
                        CommandSchema schema,
                        CommandHandler handler,
                        int require_elevation = 0);
};

std::string lower_ascii(const std::string& text);
std::vector<std::string> split_command_line(const std::string& line);
std::string rest_after_first_token(const std::string& line);
std::string sudo_usage();

CommandRegistry& command_registry();
int execute_command_line(CommandRegistry& registry,
                         const std::string& line,
                         CommandContext& context);
