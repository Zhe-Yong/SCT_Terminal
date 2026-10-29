#include "global.h"

#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>

namespace {

std::string hex_encode(const unsigned char* data, size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        result.push_back(digits[data[i] >> 4]);
        result.push_back(digits[data[i] & 0x0f]);
    }
    return result;
}

std::string certificate_sha256(PCCERT_CONTEXT certificate) {
    DWORD size = 0;
    if (!CertGetCertificateContextProperty(
            certificate, CERT_SHA256_HASH_PROP_ID, nullptr, &size) ||
        size == 0) {
        return {};
    }
    std::vector<unsigned char> hash(size, 0);
    if (!CertGetCertificateContextProperty(
            certificate, CERT_SHA256_HASH_PROP_ID, hash.data(), &size)) {
        return {};
    }
    return hex_encode(hash.data(), size);
}

std::string certificate_spki_sha256(PCCERT_CONTEXT certificate) {
    CERT_INFO* info = nullptr;
    DWORD info_size = 0;
    if (!CryptDecodeObjectEx(X509_ASN_ENCODING, X509_CERT_TO_BE_SIGNED,
                             certificate->pbCertEncoded,
                             certificate->cbCertEncoded,
                             CRYPT_DECODE_ALLOC_FLAG, nullptr,
                             &info, &info_size)) {
        return {};
    }

    DWORD hash_size = 0;
    if (!CryptHashPublicKeyInfo(0, CALG_SHA_256, 0, X509_ASN_ENCODING,
                                &info->SubjectPublicKeyInfo, nullptr,
                                &hash_size) ||
        hash_size == 0) {
        LocalFree(info);
        return {};
    }

    std::vector<unsigned char> hash(hash_size, 0);
    if (!CryptHashPublicKeyInfo(0, CALG_SHA_256, 0, X509_ASN_ENCODING,
                                &info->SubjectPublicKeyInfo, hash.data(),
                                &hash_size)) {
        LocalFree(info);
        return {};
    }

    LocalFree(info);
    return hex_encode(hash.data(), hash_size);
}

std::string certificate_subject(PCCERT_CONTEXT certificate) {
    DWORD size = CertGetNameStringW(certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE,
                                    0, nullptr, nullptr, 0);
    if (size <= 1) return {};
    std::wstring name(size, L'\0');
    CertGetNameStringW(certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr,
                       name.data(), size);
    if (!name.empty() && name.back() == L'\0') name.pop_back();
    return wide_to_utf8(name);
}

}

bool compute_file_sha256(const std::wstring& path,
                         std::string& sha256,
                         std::string& error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "Failed to open plugin: " +
                format_win32_error(GetLastError());
        return false;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status)) {
        CloseHandle(file);
        error = "Failed to open SHA-256 provider.";
        return false;
    }

    DWORD object_size = 0;
    DWORD result_size = 0;
    status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                               reinterpret_cast<PUCHAR>(&object_size),
                               sizeof(object_size), &result_size, 0);
    if (!BCRYPT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        CloseHandle(file);
        error = "Failed to query SHA-256 object size.";
        return false;
    }

    std::vector<unsigned char> object(object_size, 0);
    status = BCryptCreateHash(algorithm, &hash, object.data(), object_size,
                              nullptr, 0, 0);
    if (!BCRYPT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        CloseHandle(file);
        error = "Failed to create SHA-256 hash.";
        return false;
    }

    std::vector<unsigned char> buffer(64 * 1024, 0);
    while (BCRYPT_SUCCESS(status)) {
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                      &read, nullptr)) {
            status = static_cast<NTSTATUS>(0xC0000001L);
            break;
        }
        if (read == 0) break;
        status = BCryptHashData(hash, buffer.data(), read, 0);
    }

    unsigned char output[32] = {};
    if (BCRYPT_SUCCESS(status)) {
        status = BCryptFinishHash(hash, output, sizeof(output), 0);
    }

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);

    if (!BCRYPT_SUCCESS(status)) {
        error = "Failed to calculate plugin SHA-256.";
        return false;
    }

    sha256 = hex_encode(output, sizeof(output));
    return true;
}

bool verify_plugin_signature(const std::wstring& path,
                             PluginSignatureInfo& info,
                             std::string& error) {
    info = PluginSignatureInfo{};
    error.clear();

    WINTRUST_FILE_INFO file_info{};
    file_info.cbStruct = sizeof(file_info);
    file_info.pcwszFilePath = path.c_str();

    WINTRUST_DATA trust_data{};
    trust_data.cbStruct = sizeof(trust_data);
    trust_data.dwUIChoice = WTD_UI_NONE;
    trust_data.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust_data.dwUnionChoice = WTD_CHOICE_FILE;
    trust_data.pFile = &file_info;
    trust_data.dwStateAction = WTD_STATEACTION_VERIFY;
    trust_data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG status = WinVerifyTrust(nullptr, &action, &trust_data);
    info.signature_valid = status == ERROR_SUCCESS;
    trust_data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &trust_data);

    HCERTSTORE store = nullptr;
    HCRYPTMSG message = nullptr;
    DWORD encoding = 0;
    DWORD content = 0;
    DWORD format = 0;
    BOOL queried = CryptQueryObject(
        CERT_QUERY_OBJECT_FILE, path.c_str(),
        CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
        CERT_QUERY_FORMAT_FLAG_BINARY, 0, &encoding, &content, &format,
        &store, &message, nullptr);
    if (!queried) {
        return true;
    }

    DWORD signer_size = 0;
    if (!CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, nullptr,
                          &signer_size) ||
        signer_size == 0) {
        if (store) CertCloseStore(store, 0);
        if (message) CryptMsgClose(message);
        return true;
    }

    std::vector<unsigned char> signer_buffer(signer_size, 0);
    if (!CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0,
                          signer_buffer.data(), &signer_size)) {
        if (store) CertCloseStore(store, 0);
        if (message) CryptMsgClose(message);
        return true;
    }

    auto* signer =
        reinterpret_cast<CMSG_SIGNER_INFO*>(signer_buffer.data());
    CERT_INFO certificate_info{};
    certificate_info.Issuer = signer->Issuer;
    certificate_info.SerialNumber = signer->SerialNumber;
    PCCERT_CONTEXT certificate = CertFindCertificateInStore(
        store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
        CERT_FIND_SUBJECT_CERT, &certificate_info, nullptr);
    if (!certificate) {
        if (store) CertCloseStore(store, 0);
        if (message) CryptMsgClose(message);
        return true;
    }

    info.signed_file = true;
    info.signer_subject = certificate_subject(certificate);
    info.certificate_sha256 = certificate_sha256(certificate);
    info.signer_spki_sha256 = certificate_spki_sha256(certificate);
    info.trusted = info.signature_valid &&
                   is_trusted_plugin_signer(info.signer_spki_sha256);

    CertFreeCertificateContext(certificate);
    CertCloseStore(store, 0);
    CryptMsgClose(message);
    return true;
}

bool is_trusted_plugin_signer(const std::string& spki_sha256) {
    if (spki_sha256.size() != 64) return false;
    std::string normalized = lower_ascii(spki_sha256);
    for (const std::string& pin : trusted_plugin_signer_pins()) {
        if (lower_ascii(pin) == normalized) return true;
    }
    return false;
}
