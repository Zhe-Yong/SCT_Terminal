#pragma once

#include <string>
#include <vector>

struct CertificateRequestOptions {
    std::string subject = "CN=SuperCommandTerminal";
    std::vector<std::string> dns_names;
    int key_bits = 2048;
    std::string hash_algorithm = "sha256";
    std::string expiration;
    std::string key_usage = "digitalSignature";
    std::string enhanced_key_usage = "code-signing";
    std::string password;
    std::string parent_pfx;
    std::string parent_password;
    std::string output_path;
    bool force = false;
};

struct CertificateResult {
    std::string subject;
    std::string thumbprint_sha256;
    std::string not_after;
    std::string output_path;
};

bool generate_certificate(const CertificateRequestOptions& options,
                          CertificateResult& result,
                          std::string& error);
bool sign_file_with_pfx(const std::string& pfx_path,
                        const std::string& password,
                        const std::string& file_path,
                        std::string& signer_subject,
                        std::string& error);
