#include "global.h"

namespace {

struct EchoOptions {
    int foreground = CLR_NONE;
    int background = CLR_NONE;
    int style = CLR_NONE;
    std::string message;
};

std::string normalize_name(const std::string& text) {
    std::string result;
    for (char ch : text) {
        if (ch == '-' || ch == '_' || ch == ' ' || ch == '\t') continue;
        result.push_back(
            static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return result;
}

bool parse_color(const std::string& text, bool background, int& value) {
    std::string normalized = normalize_name(text);
    if (normalized.empty() || normalized == "default" ||
        normalized == "none") {
        value = CLR_NONE;
        return true;
    }

    struct ColorName {
        const char* name;
        int foreground;
        int background;
    };
    static const ColorName colors[] = {
        {"black", CLR_Black, CLR_BgBlack},
        {"red", CLR_Red, CLR_BgRed},
        {"green", CLR_Green, CLR_BgGreen},
        {"yellow", CLR_Yellow, CLR_BgYellow},
        {"blue", CLR_Blue, CLR_BgBlue},
        {"magenta", CLR_Magenta, CLR_BgMagenta},
        {"cyan", CLR_Cyan, CLR_BgCyan},
        {"white", CLR_White, CLR_BgWhite},
        {"gray", CLR_Gray, CLR_BgGray},
        {"grey", CLR_Gray, CLR_BgGray},
        {"brightblack", CLR_Gray, CLR_BgGray},
        {"brightred", CLR_BrightRed, CLR_BgBrightRed},
        {"brightgreen", CLR_BrightGreen, CLR_BgBrightGreen},
        {"brightyellow", CLR_BrightYellow, CLR_BgBrightYellow},
        {"brightblue", CLR_BrightBlue, CLR_BgBrightBlue},
        {"brightmagenta", CLR_BrightMagenta, CLR_BgBrightMagenta},
        {"brightcyan", CLR_BrightCyan, CLR_BgBrightCyan},
        {"brightwhite", CLR_BrightWhite, CLR_BgBrightWhite},
    };

    for (const ColorName& color : colors) {
        if (normalized == color.name) {
            value = background ? color.background : color.foreground;
            return true;
        }
    }

    try {
        size_t consumed = 0;
        int number = std::stoi(text, &consumed);
        if (consumed != text.size()) return false;
        if (background) {
            if ((number >= 40 && number <= 47) ||
                (number >= 100 && number <= 107)) {
                value = number;
                return true;
            }
        } else {
            if ((number >= 30 && number <= 37) ||
                (number >= 90 && number <= 97)) {
                value = number;
                return true;
            }
        }
    } catch (...) {
    }

    return false;
}

bool parse_style(const std::string& text, int& value) {
    std::string normalized = normalize_name(text);
    if (normalized.empty() || normalized == "none") {
        value = CLR_NONE;
        return true;
    }
    if (normalized == "normal" || normalized == "reset") {
        value = CLR_Reset;
        return true;
    }

    struct StyleName {
        const char* name;
        int value;
    };
    static const StyleName styles[] = {
        {"bold", CLR_Bold},
        {"dim", CLR_Dim},
        {"italic", CLR_Italic},
        {"underline", CLR_Underline},
        {"blink", CLR_Blink},
        {"reverse", CLR_Reverse},
        {"hidden", CLR_Hidden},
        {"strike", CLR_Strike},
    };

    for (const StyleName& style : styles) {
        if (normalized == style.name) {
            value = style.value;
            return true;
        }
    }
    return false;
}

bool split_option(const std::string& token,
                  std::string& name,
                  std::optional<std::string>& inline_value) {
    if (token.size() <= 1 || (token[0] != '-' && token[0] != '/')) {
        return false;
    }

    size_t prefix = 1;
    if (token.size() >= 2 && token[0] == '-' && token[1] == '-') {
        prefix = 2;
    }

    std::string rest = token.substr(prefix);
    size_t equals = rest.find('=');
    if (equals == std::string::npos) {
        name = normalize_name(rest);
        inline_value.reset();
    } else {
        name = normalize_name(rest.substr(0, equals));
        inline_value = rest.substr(equals + 1);
    }
    return !name.empty();
}

std::string canonical_option(const std::string& name) {
    if (name == "f" || name == "frontcolor") return "f";
    if (name == "b" || name == "backcolor") return "b";
    if (name == "t" || name == "type") return "t";
    return {};
}

std::string echo_usage() {
    return "Usage: echo [--] <message...> "
           "[-f|--frontcolor <color>] [-b|--backcolor <color>] "
           "[-t|--type <style>]\n";
}

int parse_echo(CommandContext& context, EchoOptions& options) {
    std::vector<std::string> tokens =
        split_command_line(rest_after_first_token(context.raw_line));
    std::vector<std::string> message_tokens;
    std::set<std::string> seen_options;
    bool options_ended = false;

    for (size_t index = 0; index < tokens.size(); ++index) {
        const std::string& token = tokens[index];

        if (!options_ended && token == "--") {
            options_ended = true;
            continue;
        }

        std::string name;
        std::optional<std::string> inline_value;
        std::string canonical;
        if (!options_ended && split_option(token, name, inline_value)) {
            canonical = canonical_option(name);
        }

        if (canonical.empty()) {
            message_tokens.push_back(token);
            continue;
        }

        std::string value;
        if (inline_value.has_value()) {
            value = *inline_value;
        } else if (index + 1 < tokens.size() && tokens[index + 1] != "--") {
            value = tokens[++index];
        } else {
            print(context.out, echo_usage(), CLR_BrightYellow);
            return ERR_TOO_FEW_ARGUMENTS;
        }

        if (value.empty()) {
            print(context.out,
                  "Option '-" + canonical + "' requires a value.\n",
                  CLR_BrightRed);
            return ERR_TOO_FEW_ARGUMENTS;
        }

        if (!seen_options.insert(canonical).second) {
            print(context.out,
                  "Option '-" + canonical +
                      "' specified more than once.\n",
                  CLR_BrightRed);
            return ERR_DUPLICATE_OPTION;
        }

        if (canonical == "f") {
            if (!parse_color(value, false, options.foreground)) {
                print(context.out,
                      "Invalid foreground color: " + value + ".\n",
                      CLR_BrightRed);
                return ERR_INVALID_OPTION;
            }
        } else if (canonical == "b") {
            if (!parse_color(value, true, options.background)) {
                print(context.out,
                      "Invalid background color: " + value + ".\n",
                      CLR_BrightRed);
                return ERR_INVALID_OPTION;
            }
        } else if (!parse_style(value, options.style)) {
            print(context.out, "Invalid text style: " + value + ".\n",
                  CLR_BrightRed);
            return ERR_INVALID_OPTION;
        }
    }

    for (size_t index = 0; index < message_tokens.size(); ++index) {
        if (index != 0) options.message.push_back(' ');
        options.message += message_tokens[index];
    }
    return ERR_NONE;
}

int handle_echo(CommandContext& context) {
    EchoOptions options;
    int error = parse_echo(context, options);
    if (error != ERR_NONE) return error;

    print(context.out, options.message + "\n", options.foreground,
          options.background, options.style);
    print_rst(context.out);
    return ERR_NONE;
}

CommandSchema make_echo_schema() {
    CommandSchema schema;
    schema.raw_tail = true;
    schema.min_positionals = 0;
    schema.max_positionals = 1;
    return schema;
}

const CommandRegistration register_echo_command(
    "echo", "Print text with optional color and style.",
    make_echo_schema(), handle_echo, 0,
    "Usage: echo [--] <message...> [-f|--frontcolor <color>] "
    "[-b|--backcolor <color>] [-t|--type <style>]\n"
    "Colors: black, red, green, yellow, blue, magenta, cyan, white, gray,\n"
    "        brightred, brightgreen, brightyellow, brightblue,\n"
    "        brightmagenta, brightcyan, brightwhite,\n"
    "        or ANSI numbers 30-37/90-97 for foreground and\n"
    "        40-47/100-107 for background.\n"
    "Styles: normal, bold, dim, italic, underline, blink, reverse,\n"
    "        hidden, strike.\n"
    "Use '--' to treat the remaining tokens as message text.");

}
