#pragma once

struct PluginSignatureInfo {
    bool signed_file = false;
    bool signature_valid = false;
    bool trusted = false;
    std::string signer_subject;
    std::string signer_spki_sha256;
    std::string certificate_sha256;
};

bool compute_file_sha256(const std::wstring& path,
                         std::string& sha256,
                         std::string& error);
bool verify_plugin_signature(const std::wstring& path,
                             PluginSignatureInfo& info,
                             std::string& error);
bool is_trusted_plugin_signer(const std::string& spki_sha256);
