#include "global.h"

namespace {

std::string cert_usage() {
    return "Usage:\n"
           "  cert -gen [options] [output.pfx|output.cer]\n"
           "  cert -sign <pfx> [-pwd <password>] <file>\n"
           "Options for -gen:\n"
           "  -n, --name <subject>       Certificate subject\n"
           "  -d, --dns <names>          DNS names, comma separated\n"
           "  -l, --len <bits>           RSA key length (default 2048)\n"
           "  -ha, --hash <algorithm>    sha256, sha384, or sha512\n"
           "  -e, --exp <YYYYMMDD>       Expiration date (default 1 year)\n"
           "  -ku, --usage <names>       Key usage, comma separated\n"
           "  -eku <names>               Enhanced key usage, comma separated\n"
           "  -ca <pfx>                  Parent certificate PFX\n"
           "  -capwd <password>          Parent certificate password\n"
           "  -p, --pwd <password>       Output PFX password\n"
           "  -f, --force                Overwrite an existing output file\n";
}

std::optional<std::string> first_value(
    const CommandArguments& args,
    std::initializer_list<const char*> names) {
    for (const char* name : names) {
        if (args.has(name)) return args.value(name);
    }
    return std::nullopt;
}

bool has_any(const CommandArguments& args,
             std::initializer_list<const char*> names) {
    for (const char* name : names) {
        if (args.has(name)) return true;
    }
    return false;
}

bool parse_integer(const std::string& text, int& value) {
    try {
        size_t consumed = 0;
        int parsed = std::stoi(text, &consumed);
        if (consumed != text.size()) return false;
        value = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<std::string> split_names(const std::string& text) {
    std::vector<std::string> result;
    size_t start = 0;
    while (start <= text.size()) {
        size_t comma = text.find(',', start);
        std::string part =
            text.substr(start, comma == std::string::npos
                                   ? std::string::npos
                                   : comma - start);
        size_t begin = 0;
        while (begin < part.size() &&
               (part[begin] == ' ' || part[begin] == '\t')) {
            ++begin;
        }
        size_t end = part.size();
        while (end > begin &&
               (part[end - 1] == ' ' || part[end - 1] == '\t')) {
            --end;
        }
        if (end > begin) result.push_back(part.substr(begin, end - begin));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return result;
}

int error_code_for(const std::string& error) {
    std::string lower = lower_ascii(error);
    if (lower.find("not found") != std::string::npos) {
        return ERR_FILE_NOT_FOUND;
    }
    if (lower.find("access is denied") != std::string::npos ||
        lower.find("permission") != std::string::npos) {
        return ERR_PERMISSION_DENIED;
    }
    return ERR_UNKNOWN;
}

void print_error(CommandContext& context, const std::string& error) {
    print(context.out, error + "\n", CLR_BrightRed);
    print(context.out, cert_usage(), CLR_BrightYellow);
    print_rst(context.out);
}

int handle_generate(CommandContext& context) {
    CertificateRequestOptions options;

    if (auto value = first_value(context.args, {"n", "name", "subject"})) {
        options.subject = *value;
    }
    if (auto value = first_value(context.args, {"d", "dns"})) {
        options.dns_names = split_names(*value);
    }
    if (auto value = first_value(context.args, {"l", "len", "length"})) {
        if (!parse_integer(*value, options.key_bits)) {
            print_error(context, "Invalid key length: " + *value + ".");
            return ERR_INVALID_OPTION;
        }
    }
    if (auto value = first_value(context.args, {"ha", "hash"})) {
        options.hash_algorithm = *value;
    }
    if (auto value = first_value(context.args, {"e", "exp"})) {
        options.expiration = *value;
    }
    if (auto value = first_value(context.args, {"ku", "usage"})) {
        options.key_usage = *value;
    }
    if (auto value = first_value(context.args, {"eku"})) {
        options.enhanced_key_usage = *value;
    }
    if (auto value = first_value(context.args, {"p", "pwd"})) {
        options.password = *value;
    }
    if (auto value = first_value(context.args, {"ca", "parent"})) {
        options.parent_pfx = *value;
    }
    if (auto value = first_value(
            context.args, {"capwd", "ca-pwd", "parent-pwd"})) {
        options.parent_password = *value;
    }
    options.force = has_any(context.args, {"f", "force"});
    options.output_path = context.args.positional(0).value_or("");

    CertificateResult result;
    std::string error;
    if (!generate_certificate(options, result, error)) {
        print(context.out, error + "\n", CLR_BrightRed);
        print_rst(context.out);
        return error_code_for(error);
    }

    print(context.out,
          "Certificate generated: " + result.output_path + "\n",
          CLR_BrightGreen);
    print(context.out, "Subject: " + result.subject + "\n", CLR_White);
    print(context.out,
          "SHA-256: " + result.thumbprint_sha256 + "\n", CLR_Gray);
    print(context.out, "NotAfter: " + result.not_after + "\n", CLR_Gray);
    print_rst(context.out);
    return ERR_NONE;
}

int handle_sign(CommandContext& context) {
    std::optional<std::string> pfx = first_value(context.args, {"s", "sign"});
    if (!pfx || pfx->empty()) {
        print_error(context, "Missing certificate PFX for -sign.");
        return ERR_TOO_FEW_ARGUMENTS;
    }

    std::optional<std::string> target = context.args.positional(0);
    if (!target || target->empty()) {
        print_error(context, "Missing target file for -sign.");
        return ERR_TOO_FEW_ARGUMENTS;
    }

    std::string password =
        first_value(context.args, {"p", "pwd"}).value_or("");
    std::string signer_subject;
    std::string error;
    if (!sign_file_with_pfx(*pfx, password, *target, signer_subject, error)) {
        print(context.out, error + "\n", CLR_BrightRed);
        print_rst(context.out);
        return error_code_for(error);
    }

    print(context.out, "Signed: " + *target + "\n", CLR_BrightGreen);
    if (!signer_subject.empty()) {
        print(context.out, "Signer: " + signer_subject + "\n", CLR_Gray);
    }
    print_rst(context.out);
    return ERR_NONE;
}

int handle_cert(CommandContext& context) {
    bool generate = has_any(context.args, {"g", "gen"});
    bool sign = has_any(context.args, {"s", "sign"});

    if (generate && sign) {
        print_error(context, "-gen and -sign cannot be used together.");
        return ERR_INVALID_OPTION;
    }
    if (!generate && !sign) {
        print_error(context, "Specify -gen or -sign.");
        return ERR_TOO_FEW_ARGUMENTS;
    }
    return generate ? handle_generate(context) : handle_sign(context);
}

CommandSchema make_cert_schema() {
    CommandSchema schema;
    schema.options.push_back(CommandOption{"g", OptionValueMode::Flag});
    schema.options.push_back(CommandOption{"gen", OptionValueMode::Flag});
    schema.options.push_back(CommandOption{"s", OptionValueMode::Required});
    schema.options.push_back(CommandOption{"sign", OptionValueMode::Required});
    schema.options.push_back(CommandOption{"p", OptionValueMode::Required});
    schema.options.push_back(CommandOption{"pwd", OptionValueMode::Required});
    schema.options.push_back(CommandOption{"e", OptionValueMode::Required});
    schema.options.push_back(CommandOption{"exp", OptionValueMode::Required});
    schema.options.push_back(CommandOption{"n", OptionValueMode::Required});
    schema.options.push_back(CommandOption{"name", OptionValueMode::Required});
    schema.options.push_back(
        CommandOption{"subject", OptionValueMode::Required});
    schema.options.push_back(CommandOption{"d", OptionValueMode::Required});
    schema.options.push_back(CommandOption{"dns", OptionValueMode::Required});
    schema.options.push_back(CommandOption{"l", OptionValueMode::Required});
    schema.options.push_back(CommandOption{"len", OptionValueMode::Required});
    schema.options.push_back(
        CommandOption{"length", OptionValueMode::Required});
    schema.options.push_back(CommandOption{"ha", OptionValueMode::Required});
    schema.options.push_back(CommandOption{"hash", OptionValueMode::Required});
    schema.options.push_back(CommandOption{"ku", OptionValueMode::Required});
    schema.options.push_back(
        CommandOption{"usage", OptionValueMode::Required});
    schema.options.push_back(CommandOption{"eku", OptionValueMode::Required});
    schema.options.push_back(CommandOption{"ca", OptionValueMode::Required});
    schema.options.push_back(
        CommandOption{"parent", OptionValueMode::Required});
    schema.options.push_back(
        CommandOption{"capwd", OptionValueMode::Required});
    schema.options.push_back(
        CommandOption{"ca-pwd", OptionValueMode::Required});
    schema.options.push_back(
        CommandOption{"parent-pwd", OptionValueMode::Required});
    schema.options.push_back(CommandOption{"f", OptionValueMode::Flag});
    schema.options.push_back(CommandOption{"force", OptionValueMode::Flag});
    schema.max_positionals = 1;
    return schema;
}

const CommandRegistration register_cert_command(
    "cert", "Generate certificates or sign files.", make_cert_schema(),
    handle_cert, 0,
    "Usage:\n"
    "  cert -gen [options] [output.pfx|output.cer]\n"
    "  cert -sign <pfx> [-pwd <password>] <file>\n"
    "Generate options:\n"
    "  -n, --name <subject>     default CN=SuperCommandTerminal\n"
    "  -d, --dns <names>        comma-separated DNS names\n"
    "  -l, --len <bits>         RSA key length, default 2048\n"
    "  -ha, --hash <algorithm>  sha256, sha384, or sha512\n"
    "  -e, --exp <YYYYMMDD>     expiration date, default 1 year\n"
    "  -ku, --usage <names>     key usage, default digitalSignature\n"
    "  -eku <names>             enhanced key usage, default code-signing\n"
    "  -ca <pfx>                parent PFX for certificate issuance\n"
    "  -capwd <password>        parent PFX password\n"
    "  -p, --pwd <password>     output PFX password\n"
    "  -f, --force              overwrite an existing output file\n"
    "Sign options:\n"
    "  -s, --sign <pfx>         certificate PFX used for signing\n"
    "  -p, --pwd <password>     PFX password\n"
    "Output defaults to cert.pfx. .cer/.crt exports the public certificate "
    "only.");

}
