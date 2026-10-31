#pragma once

inline const std::vector<std::string>& trusted_plugin_signer_pins() {
    static const std::vector<std::string> pins = {
        // Add lowercase SHA-256 hashes of trusted signer SubjectPublicKeyInfo.
        // Example: "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
    };
    return pins;
}
