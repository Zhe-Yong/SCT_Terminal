#include "global.h"

#include <wincrypt.h>

namespace {

constexpr DWORD kMaxCertificateFileSize = 64 * 1024 * 1024;

constexpr DWORD SIGNER_SUBJECT_FILE = 1;
constexpr DWORD SIGNER_CERT_STORE = 2;
constexpr DWORD SIGNER_CERT_POLICY_STORE = 0x1;
constexpr DWORD SIGNER_NO_ATTR = 0;

struct SIGNER_CONTEXT {
    DWORD cbSize;
    DWORD cbBlob;
    BYTE* pbBlob;
};

struct SIGNER_FILE_INFO {
    DWORD cbSize;
    const WCHAR* pwszFileName;
    HANDLE hFile;
};

struct SIGNER_BLOB_INFO {
    DWORD cbSize;
    GUID* pGuidSubject;
    DWORD cbBlob;
    BYTE* pbBlob;
    const WCHAR* pwszDisplayName;
};

struct SIGNER_SUBJECT_INFO {
    DWORD cbSize;
    DWORD* pdwIndex;
    DWORD dwSubjectChoice;
    union {
        SIGNER_FILE_INFO* pSignerFileInfo;
        SIGNER_BLOB_INFO* pSignerBlobInfo;
    };
};

struct SIGNER_CERT_STORE_INFO {
    DWORD cbSize;
    const CERT_CONTEXT* pSigningCert;
    DWORD dwCertPolicy;
    HCERTSTORE hCertStore;
};

struct SIGNER_SPC_CHAIN_INFO {
    DWORD cbSize;
    const WCHAR* pwszSpcFile;
    DWORD dwCertPolicy;
    HCERTSTORE hCertStore;
};

struct SIGNER_CERT {
    DWORD cbSize;
    DWORD dwCertChoice;
    union {
        const WCHAR* pwszSpcFile;
        SIGNER_CERT_STORE_INFO* pCertStoreInfo;
        SIGNER_SPC_CHAIN_INFO* pSpcChainInfo;
    };
    HWND hwnd;
};

struct SIGNER_ATTR_AUTHCODE {
    DWORD cbSize;
    BOOL fCommercial;
    BOOL fIndividual;
    const WCHAR* pwszName;
    const WCHAR* pwszInfo;
};

struct SIGNER_SIGNATURE_INFO {
    DWORD cbSize;
    ALG_ID algidHash;
    DWORD dwAttrChoice;
    union {
        SIGNER_ATTR_AUTHCODE* pAttrAuthcode;
    };
    CRYPT_ATTRIBUTES* psAuthenticated;
    CRYPT_ATTRIBUTES* psUnauthenticated;
};

struct SIGNER_PROVIDER_INFO {
    DWORD cbSize;
    const WCHAR* pwszProviderName;
    DWORD dwProviderType;
    DWORD dwKeySpec;
    DWORD dwPvkChoice;
    union {
        WCHAR* pwszPvkFileName;
        WCHAR* pwszKeyContainer;
    };
};

struct CertStoreHolder {
    HCERTSTORE value = nullptr;

    ~CertStoreHolder() {
        if (value) CertCloseStore(value, 0);
    }

    CertStoreHolder() = default;
    CertStoreHolder(const CertStoreHolder&) = delete;
    CertStoreHolder& operator=(const CertStoreHolder&) = delete;
};

struct CertContextHolder {
    PCCERT_CONTEXT value = nullptr;

    ~CertContextHolder() {
        if (value) CertFreeCertificateContext(value);
    }

    CertContextHolder() = default;
    CertContextHolder(const CertContextHolder&) = delete;
    CertContextHolder& operator=(const CertContextHolder&) = delete;
};

struct CryptProvHolder {
    HCRYPTPROV value = 0;

    ~CryptProvHolder() {
        if (value) CryptReleaseContext(value, 0);
    }

    CryptProvHolder() = default;
    CryptProvHolder(const CryptProvHolder&) = delete;
    CryptProvHolder& operator=(const CryptProvHolder&) = delete;
};

struct CryptKeyHolder {
    HCRYPTKEY value = 0;

    ~CryptKeyHolder() {
        if (value) CryptDestroyKey(value);
    }

    CryptKeyHolder() = default;
    CryptKeyHolder(const CryptKeyHolder&) = delete;
    CryptKeyHolder& operator=(const CryptKeyHolder&) = delete;
};

struct CryptHashHolder {
    HCRYPTHASH value = 0;

    ~CryptHashHolder() {
        if (value) CryptDestroyHash(value);
    }

    CryptHashHolder() = default;
    CryptHashHolder(const CryptHashHolder&) = delete;
    CryptHashHolder& operator=(const CryptHashHolder&) = delete;
};

struct HashSpec {
    const char* name;
    ALG_ID algorithm;
    const char* oid;
};

const HashSpec kHashAlgorithms[] = {
    {"sha1", CALG_SHA1, szOID_RSA_SHA1RSA},
    {"sha256", CALG_SHA_256, szOID_RSA_SHA256RSA},
    {"sha384", CALG_SHA_384, szOID_RSA_SHA384RSA},
    {"sha512", CALG_SHA_512, szOID_RSA_SHA512RSA},
};

const HashSpec* find_hash_algorithm(const std::string& name) {
    std::string normalized = lower_ascii(name);
    for (const HashSpec& spec : kHashAlgorithms) {
        if (normalized == spec.name) return &spec;
    }
    return nullptr;
}

std::wstring full_path(const std::wstring& path) {
    DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (needed == 0) return path;

    std::wstring result(needed, L'\0');
    DWORD length = GetFullPathNameW(path.c_str(), needed, result.data(),
                                    nullptr);
    if (length == 0 || length >= needed) return path;
    result.resize(length);
    return result;
}

bool read_file(const std::wstring& path,
               std::vector<unsigned char>& data,
               std::string& error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "Cannot open '" + wide_to_utf8(path) + "': " +
                format_win32_error(GetLastError());
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) >
            kMaxCertificateFileSize) {
        DWORD code = GetLastError();
        CloseHandle(file);
        error = "Cannot read '" + wide_to_utf8(path) + "': " +
                (code ? format_win32_error(code) : "file is too large.");
        return false;
    }

    data.resize(static_cast<size_t>(size.QuadPart));
    size_t offset = 0;
    while (offset < data.size()) {
        DWORD read = 0;
        DWORD remaining = static_cast<DWORD>(
            std::min<size_t>(data.size() - offset, 0x7FFFFFFF));
        if (!ReadFile(file, data.data() + offset, remaining, &read, nullptr)) {
            DWORD code = GetLastError();
            CloseHandle(file);
            error = "Cannot read '" + wide_to_utf8(path) + "': " +
                    format_win32_error(code);
            return false;
        }
        if (read == 0) break;
        offset += read;
    }

    CloseHandle(file);
    if (offset != data.size()) {
        error = "Cannot read '" + wide_to_utf8(path) + "': unexpected EOF.";
        return false;
    }
    return true;
}

bool write_file(const std::wstring& path,
                const unsigned char* data,
                DWORD size,
                bool overwrite,
                std::string& error) {
    if (size != 0 && data == nullptr) {
        error = "Cannot write '" + wide_to_utf8(path) +
                "': output buffer is null.";
        return false;
    }
    DWORD disposition = overwrite ? CREATE_ALWAYS : CREATE_NEW;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS) {
            error = "Output already exists: " + wide_to_utf8(path) +
                    ". Use -force to overwrite.";
        } else {
            error = "Cannot write '" + wide_to_utf8(path) + "': " +
                    format_win32_error(code);
        }
        return false;
    }

    DWORD offset = 0;
    while (offset < size) {
        DWORD written = 0;
        if (!WriteFile(file, data + offset, size - offset, &written, nullptr)) {
            DWORD code = GetLastError();
            CloseHandle(file);
            error = "Cannot write '" + wide_to_utf8(path) + "': " +
                    format_win32_error(code);
            return false;
        }
        if (written == 0) {
            CloseHandle(file);
            error = "Cannot write '" + wide_to_utf8(path) + "': no progress.";
            return false;
        }
        offset += written;
    }

    CloseHandle(file);
    return true;
}

std::vector<std::string> split_csv(const std::string& text) {
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

std::string normalize_name(const std::string& text) {
    std::string result;
    for (char ch : text) {
        if (ch == '-' || ch == '_' || ch == ' ' || ch == '\t') continue;
        result.push_back(
            static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return result;
}

bool parse_key_usage(const std::string& text,
                     BYTE bytes[2],
                     DWORD& length,
                     DWORD& unused_bits,
                     bool& is_ca,
                     std::string& error) {
    bytes[0] = 0;
    bytes[1] = 0;
    length = 1;
    unused_bits = 0;
    is_ca = false;

    std::vector<std::string> names = split_csv(text);
    if (names.empty()) names.push_back("digitalSignature");

    bool has_decipher_only = false;
    bool any = false;
    for (const std::string& name : names) {
        std::string normalized = normalize_name(name);
        BYTE flag = 0;
        if (normalized == "digitalsignature") {
            flag = CERT_DIGITAL_SIGNATURE_KEY_USAGE;
        } else if (normalized == "nonrepudiation") {
            flag = CERT_NON_REPUDIATION_KEY_USAGE;
        } else if (normalized == "keyencipherment") {
            flag = CERT_KEY_ENCIPHERMENT_KEY_USAGE;
        } else if (normalized == "dataencipherment") {
            flag = CERT_DATA_ENCIPHERMENT_KEY_USAGE;
        } else if (normalized == "keyagreement") {
            flag = CERT_KEY_AGREEMENT_KEY_USAGE;
        } else if (normalized == "certsign") {
            flag = CERT_KEY_CERT_SIGN_KEY_USAGE;
            is_ca = true;
        } else if (normalized == "crlsign") {
            flag = CERT_CRL_SIGN_KEY_USAGE;
        } else if (normalized == "encipheronly") {
            flag = CERT_ENCIPHER_ONLY_KEY_USAGE;
        } else if (normalized == "decipheronly") {
            has_decipher_only = true;
        } else {
            error = "Unknown key usage: " + name + ".";
            return false;
        }
        if (flag) {
            bytes[0] = static_cast<BYTE>(bytes[0] | flag);
            any = true;
        }
        if (has_decipher_only) any = true;
    }

    if (!any) {
        error = "At least one key usage is required.";
        return false;
    }

    if (has_decipher_only) {
        bytes[1] = CERT_DECIPHER_ONLY_KEY_USAGE;
        length = 2;
        unused_bits = 7;
    }
    return true;
}

bool parse_enhanced_key_usage(const std::string& text,
                              std::vector<std::string>& oids,
                              std::string& error) {
    std::vector<std::string> names = split_csv(text);
    if (names.empty()) names.push_back("code-signing");

    for (const std::string& name : names) {
        std::string normalized = normalize_name(name);
        if (normalized == "codesigning") {
            oids.push_back(szOID_PKIX_KP_CODE_SIGNING);
        } else if (normalized == "clientauth") {
            oids.push_back(szOID_PKIX_KP_CLIENT_AUTH);
        } else if (normalized == "serverauth") {
            oids.push_back(szOID_PKIX_KP_SERVER_AUTH);
        } else if (normalized == "emailprotection" || normalized == "email") {
            oids.push_back(szOID_PKIX_KP_EMAIL_PROTECTION);
        } else if (normalized == "any") {
            oids.push_back(szOID_ANY_ENHANCED_KEY_USAGE);
        } else if (name.find('.') != std::string::npos) {
            oids.push_back(name);
        } else {
            error = "Unknown enhanced key usage: " + name + ".";
            return false;
        }
    }
    return true;
}

bool parse_expiration(const std::string& text,
                      SYSTEMTIME& end,
                      std::string& error) {
    SYSTEMTIME now{};
    GetSystemTime(&now);

    if (text.empty()) {
        FILETIME start_ft{};
        if (!SystemTimeToFileTime(&now, &start_ft)) {
            error = "Cannot calculate the default expiration time.";
            return false;
        }
        ULARGE_INTEGER value{};
        value.LowPart = start_ft.dwLowDateTime;
        value.HighPart = start_ft.dwHighDateTime;
        value.QuadPart += 365ULL * 24ULL * 60ULL * 60ULL * 10000000ULL;
        FILETIME end_ft{};
        end_ft.dwLowDateTime = value.LowPart;
        end_ft.dwHighDateTime = value.HighPart;
        if (!FileTimeToSystemTime(&end_ft, &end)) {
            error = "Cannot calculate the default expiration time.";
            return false;
        }
        return true;
    }

    if (text.size() != 8 ||
        !std::all_of(text.begin(), text.end(),
                     [](char ch) { return ch >= '0' && ch <= '9'; })) {
        error = "Expiration must use YYYYMMDD.";
        return false;
    }

    SYSTEMTIME parsed{};
    parsed.wYear = static_cast<WORD>(std::stoi(text.substr(0, 4)));
    parsed.wMonth = static_cast<WORD>(std::stoi(text.substr(4, 2)));
    parsed.wDay = static_cast<WORD>(std::stoi(text.substr(6, 2)));
    parsed.wHour = 23;
    parsed.wMinute = 59;
    parsed.wSecond = 59;
    parsed.wMilliseconds = 0;

    FILETIME file_time{};
    if (!SystemTimeToFileTime(&parsed, &file_time)) {
        error = "Invalid expiration date: " + text + ".";
        return false;
    }
    end = parsed;
    return true;
}

bool make_subject_blob(const std::string& subject,
                       std::vector<BYTE>& storage,
                       CERT_NAME_BLOB& blob,
                       std::string& error) {
    std::wstring wide = utf8_to_wide(subject);
    DWORD size = 0;
    if (!CertStrToNameW(X509_ASN_ENCODING, wide.c_str(), CERT_X500_NAME_STR,
                        nullptr, nullptr, &size, nullptr)) {
        error = "Invalid certificate subject: " + subject + ".";
        return false;
    }

    storage.resize(size);
    if (!CertStrToNameW(X509_ASN_ENCODING, wide.c_str(), CERT_X500_NAME_STR,
                        nullptr, storage.data(), &size, nullptr)) {
        error = "Invalid certificate subject: " + subject + ".";
        return false;
    }

    blob.cbData = size;
    blob.pbData = storage.data();
    return true;
}

struct ExtensionList {
    std::vector<std::vector<BYTE>> values;
    std::vector<const char*> oids;
    std::vector<bool> criticals;
    std::vector<CERT_EXTENSION> extensions;
    CERT_EXTENSIONS info{};

    void finalize() {
        info.cExtension = static_cast<DWORD>(extensions.size());
        info.rgExtension = extensions.data();
    }
};

bool add_extension(ExtensionList& list,
                   const char* oid,
                   bool critical,
                   LPCSTR structure_type,
                   const void* structure,
                   std::string& error) {
    DWORD size = 0;
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, structure_type, structure, 0,
                             nullptr, nullptr, &size)) {
        error = "Cannot encode certificate extension " + std::string(oid) +
                ": " + format_win32_error(GetLastError());
        return false;
    }

    list.values.emplace_back(size);
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, structure_type, structure, 0,
                             nullptr, list.values.back().data(), &size)) {
        error = "Cannot encode certificate extension " + std::string(oid) +
                ": " + format_win32_error(GetLastError());
        return false;
    }
    list.oids.push_back(oid);
    list.criticals.push_back(critical);
    return true;
}

bool build_extensions(const CertificateRequestOptions& options,
                      const std::vector<std::string>& eku_oids,
                      bool& is_ca,
                      ExtensionList& list,
                      std::string& error) {
    list.values.reserve(4);

    BYTE usage_bytes[2] = {};
    DWORD usage_length = 1;
    DWORD unused_bits = 0;
    bool usage_is_ca = false;
    if (!parse_key_usage(options.key_usage, usage_bytes, usage_length,
                         unused_bits, usage_is_ca, error)) {
        return false;
    }
    is_ca = is_ca || usage_is_ca;

    CRYPT_BIT_BLOB usage{};
    usage.cbData = usage_length;
    usage.pbData = usage_bytes;
    usage.cUnusedBits = unused_bits;
    if (!add_extension(list, szOID_KEY_USAGE, true, X509_KEY_USAGE, &usage,
                       error)) {
        return false;
    }

    std::vector<LPSTR> eku_pointers;
    eku_pointers.reserve(eku_oids.size());
    for (const std::string& oid : eku_oids) {
        eku_pointers.push_back(const_cast<LPSTR>(oid.c_str()));
    }
    CERT_ENHKEY_USAGE eku{};
    eku.cUsageIdentifier = static_cast<DWORD>(eku_pointers.size());
    eku.rgpszUsageIdentifier = eku_pointers.data();
    if (!add_extension(list, szOID_ENHANCED_KEY_USAGE, false,
                       X509_ENHANCED_KEY_USAGE, &eku, error)) {
        return false;
    }

    CERT_BASIC_CONSTRAINTS2_INFO constraints{};
    constraints.fCA = is_ca ? TRUE : FALSE;
    constraints.fPathLenConstraint = FALSE;
    if (!add_extension(list, szOID_BASIC_CONSTRAINTS2, is_ca,
                       X509_BASIC_CONSTRAINTS2, &constraints, error)) {
        return false;
    }

    if (!options.dns_names.empty()) {
        std::vector<std::wstring> wide_names;
        wide_names.reserve(options.dns_names.size());
        for (const std::string& name : options.dns_names) {
            wide_names.push_back(utf8_to_wide(name));
        }

        std::vector<CERT_ALT_NAME_ENTRY> entries;
        entries.reserve(wide_names.size());
        for (std::wstring& name : wide_names) {
            CERT_ALT_NAME_ENTRY entry{};
            entry.dwAltNameChoice = CERT_ALT_NAME_DNS_NAME;
            entry.pwszDNSName = const_cast<LPWSTR>(name.c_str());
            entries.push_back(entry);
        }

        CERT_ALT_NAME_INFO alt_names{};
        alt_names.cAltEntry = static_cast<DWORD>(entries.size());
        alt_names.rgAltEntry = entries.data();
        if (!add_extension(list, szOID_SUBJECT_ALT_NAME2, false,
                           X509_ALTERNATE_NAME, &alt_names, error)) {
            return false;
        }
    }

    list.extensions.reserve(list.values.size());
    for (size_t index = 0; index < list.values.size(); ++index) {
        CERT_EXTENSION extension{};
        extension.pszObjId = const_cast<LPSTR>(list.oids[index]);
        extension.fCritical = list.criticals[index] ? TRUE : FALSE;
        extension.Value.cbData =
            static_cast<DWORD>(list.values[index].size());
        extension.Value.pbData = list.values[index].data();
        list.extensions.push_back(extension);
    }
    list.finalize();
    return true;
}

bool create_key_provider(const std::wstring& container,
                         int key_bits,
                         CryptProvHolder& provider,
                         CryptKeyHolder& key,
                         CRYPT_KEY_PROV_INFO& key_info,
                         std::string& error) {
    if (!CryptAcquireContextW(&provider.value, container.c_str(),
                              MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES,
                              CRYPT_NEWKEYSET)) {
        error = "Cannot create certificate key container: " +
                format_win32_error(GetLastError());
        return false;
    }

    if (!CryptGenKey(provider.value, AT_SIGNATURE,
                     (static_cast<DWORD>(key_bits) << 16) | CRYPT_EXPORTABLE,
                     &key.value)) {
        error = "Cannot create certificate key: " +
                format_win32_error(GetLastError());
        return false;
    }

    key_info = {};
    key_info.pwszContainerName = const_cast<LPWSTR>(container.c_str());
    key_info.pwszProvName = const_cast<LPWSTR>(MS_ENH_RSA_AES_PROV_W);
    key_info.dwProvType = PROV_RSA_AES;
    key_info.dwKeySpec = AT_SIGNATURE;
    key_info.dwFlags = 0;
    return true;
}

void delete_key_container(const std::wstring& container) {
    HCRYPTPROV provider = 0;
    CryptAcquireContextW(&provider, container.c_str(), MS_ENH_RSA_AES_PROV_W,
                         PROV_RSA_AES, CRYPT_DELETEKEYSET);
}

bool acquire_certificate_key(PCCERT_CONTEXT certificate,
                             HCRYPTPROV& provider,
                             DWORD& key_spec,
                             bool& provider_owned,
                             std::string& error) {
    provider = 0;
    key_spec = 0;
    provider_owned = false;

    DWORD size = 0;
    if (CertGetCertificateContextProperty(
            certificate, CERT_KEY_PROV_INFO_PROP_ID, nullptr, &size) &&
        size >= sizeof(CRYPT_KEY_PROV_INFO)) {
        std::vector<BYTE> buffer(size);
        if (CertGetCertificateContextProperty(
                certificate, CERT_KEY_PROV_INFO_PROP_ID, buffer.data(),
                &size)) {
            auto* info = reinterpret_cast<CRYPT_KEY_PROV_INFO*>(buffer.data());
            HCRYPTPROV acquired = 0;
            if (CryptAcquireContextW(
                    &acquired, info->pwszContainerName, info->pwszProvName,
                    info->dwProvType, 0)) {
                provider = acquired;
                key_spec = info->dwKeySpec;
                provider_owned = true;
                return true;
            }
        }
    }

    size = 0;
    if (CertGetCertificateContextProperty(
            certificate, CERT_KEY_CONTEXT_PROP_ID, nullptr, &size) &&
        size >= sizeof(CERT_KEY_CONTEXT)) {
        std::vector<BYTE> buffer(size);
        if (CertGetCertificateContextProperty(
                certificate, CERT_KEY_CONTEXT_PROP_ID, buffer.data(), &size)) {
            auto* context = reinterpret_cast<CERT_KEY_CONTEXT*>(buffer.data());
            if (context->hCryptProv &&
                context->dwKeySpec != CERT_NCRYPT_KEY_SPEC) {
                provider = context->hCryptProv;
                key_spec = context->dwKeySpec;
                return true;
            }
        }
    }

    error = "The certificate does not have an accessible private key.";
    return false;
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

std::string certificate_sha256(PCCERT_CONTEXT certificate) {
    DWORD size = 0;
    if (!CertGetCertificateContextProperty(
            certificate, CERT_SHA256_HASH_PROP_ID, nullptr, &size) ||
        size == 0) {
        return {};
    }

    std::vector<BYTE> hash(size);
    if (!CertGetCertificateContextProperty(
            certificate, CERT_SHA256_HASH_PROP_ID, hash.data(), &size)) {
        return {};
    }

    static const char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (DWORD index = 0; index < size; ++index) {
        result.push_back(digits[hash[index] >> 4]);
        result.push_back(digits[hash[index] & 0x0F]);
    }
    return result;
}

std::string certificate_not_after(PCCERT_CONTEXT certificate) {
    SYSTEMTIME time{};
    if (!FileTimeToSystemTime(&certificate->pCertInfo->NotAfter, &time)) {
        return {};
    }
    std::ostringstream text;
    text << std::setfill('0') << std::setw(4) << time.wYear << '-'
         << std::setw(2) << time.wMonth << '-' << std::setw(2) << time.wDay
         << ' ' << std::setw(2) << time.wHour << ':' << std::setw(2)
         << time.wMinute << ':' << std::setw(2) << time.wSecond << " UTC";
    return text.str();
}

bool is_pfx_path(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return true;
    std::string extension = lower_ascii(wide_to_utf8(path.substr(dot)));
    return extension == ".pfx" || extension == ".p12";
}

bool export_certificate_store(HCERTSTORE store,
                              const std::wstring& path,
                              const std::wstring& password,
                              bool overwrite,
                              std::string& error) {
    CRYPT_DATA_BLOB pfx{};
    DWORD flags = EXPORT_PRIVATE_KEYS;
    if (!PFXExportCertStoreEx(store, &pfx, password.c_str(), nullptr, flags)) {
        error = "Cannot export PFX: " + format_win32_error(GetLastError());
        return false;
    }

    std::vector<BYTE> fallback;
    bool allocated_by_us = false;
    if (!pfx.pbData && pfx.cbData != 0) {
        fallback.resize(pfx.cbData);
        pfx.pbData = fallback.data();
        DWORD capacity = pfx.cbData;
        if (!PFXExportCertStoreEx(store, &pfx, password.c_str(), nullptr,
                                  flags)) {
            error = "Cannot export PFX: " + format_win32_error(GetLastError());
            return false;
        }
        if (pfx.cbData > capacity) {
            error = "PFX export buffer is too small.";
            return false;
        }
        allocated_by_us = pfx.pbData == fallback.data();
    }

    if (!pfx.pbData) {
        error = "PFX export returned no data (cbData=" +
                std::to_string(pfx.cbData) + ", lastError=" +
                std::to_string(GetLastError()) + ").";
        return false;
    }

    bool ok = write_file(path, pfx.pbData, pfx.cbData, overwrite, error);
    if (pfx.pbData && !allocated_by_us) {
        SecureZeroMemory(pfx.pbData, pfx.cbData);
        LocalFree(pfx.pbData);
    }
    return ok;
}

bool export_certificate_der(PCCERT_CONTEXT certificate,
                            const std::wstring& path,
                            bool overwrite,
                            std::string& error) {
    DWORD size = 0;
    if (!CertSerializeCertificateStoreElement(certificate, 0, nullptr, &size)) {
        error = "Cannot serialize certificate: " +
                format_win32_error(GetLastError());
        return false;
    }

    std::vector<BYTE> data(size);
    if (!CertSerializeCertificateStoreElement(certificate, 0, data.data(),
                                              &size)) {
        error = "Cannot serialize certificate: " +
                format_win32_error(GetLastError());
        return false;
    }
    return write_file(path, data.data(), size, overwrite, error);
}

bool add_public_certificates(HCERTSTORE source, HCERTSTORE destination) {
    PCCERT_CONTEXT current = nullptr;
    while ((current = CertEnumCertificatesInStore(source, current)) != nullptr) {
        PCCERT_CONTEXT public_cert = CertCreateCertificateContext(
            X509_ASN_ENCODING, current->pbCertEncoded, current->cbCertEncoded);
        if (public_cert) {
            CertAddCertificateContextToStore(
                destination, public_cert, CERT_STORE_ADD_REPLACE_EXISTING,
                nullptr);
            CertFreeCertificateContext(public_cert);
        }
    }
    return true;
}

bool create_self_signed_certificate(
    const CertificateRequestOptions& options,
    const std::wstring& container,
    CertContextHolder& certificate,
    ExtensionList& extensions,
    const HashSpec& hash,
    SYSTEMTIME start,
    SYSTEMTIME end,
    std::string& error) {
    std::vector<BYTE> subject_storage;
    CERT_NAME_BLOB subject{};
    if (!make_subject_blob(options.subject, subject_storage, subject, error)) {
        return false;
    }

    CryptProvHolder provider;
    CryptKeyHolder key;
    CRYPT_KEY_PROV_INFO key_info{};
    if (!create_key_provider(container, options.key_bits, provider, key,
                             key_info, error)) {
        return false;
    }

    CRYPT_ALGORITHM_IDENTIFIER signature_algorithm{};
    signature_algorithm.pszObjId = const_cast<LPSTR>(hash.oid);

    certificate.value = CertCreateSelfSignCertificate(
        provider.value, &subject, 0, &key_info, &signature_algorithm, &start,
        &end, &extensions.info);
    if (!certificate.value) {
        error = "Cannot create certificate: " +
                format_win32_error(GetLastError());
        return false;
    }

    if (!CertSetCertificateContextProperty(
            certificate.value, CERT_KEY_PROV_INFO_PROP_ID, 0, &key_info)) {
        error = "Cannot attach the private key to the certificate: " +
                format_win32_error(GetLastError());
        return false;
    }
    return true;
}

bool sign_certificate_info(HCRYPTPROV provider,
                           DWORD key_spec,
                           const CERT_INFO& info,
                           const HashSpec& hash,
                           std::vector<BYTE>& encoded,
                           std::string& error) {
    DWORD to_be_signed_size = 0;
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, X509_CERT_TO_BE_SIGNED,
                             &info, 0, nullptr, nullptr,
                             &to_be_signed_size)) {
        DWORD code = GetLastError();
        error = "Cannot encode the certificate: " +
                format_win32_error(code) + " (" + std::to_string(code) + ")";
        return false;
    }

    std::vector<BYTE> to_be_signed(to_be_signed_size);
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, X509_CERT_TO_BE_SIGNED,
                             &info, 0, nullptr, to_be_signed.data(),
                             &to_be_signed_size)) {
        DWORD code = GetLastError();
        error = "Cannot encode the certificate: " +
                format_win32_error(code) + " (" + std::to_string(code) + ")";
        return false;
    }

    CryptHashHolder hash_handle;
    if (!CryptCreateHash(provider, hash.algorithm, 0, 0, &hash_handle.value)) {
        DWORD code = GetLastError();
        error = "Cannot create the certificate hash: " +
                format_win32_error(code) + " (" + std::to_string(code) + ")";
        return false;
    }
    if (!CryptHashData(hash_handle.value, to_be_signed.data(),
                       to_be_signed_size, 0)) {
        DWORD code = GetLastError();
        error = "Cannot hash the certificate: " +
                format_win32_error(code) + " (" + std::to_string(code) + ")";
        return false;
    }

    DWORD signature_size = 0;
    if (!CryptSignHashW(hash_handle.value, key_spec, nullptr, 0, nullptr,
                        &signature_size)) {
        DWORD code = GetLastError();
        error = "Cannot sign the certificate: " +
                format_win32_error(code) + " (" + std::to_string(code) + ")";
        return false;
    }

    std::vector<BYTE> signature(signature_size);
    if (!CryptSignHashW(hash_handle.value, key_spec, nullptr, 0,
                        signature.data(), &signature_size)) {
        DWORD code = GetLastError();
        error = "Cannot sign the certificate: " +
                format_win32_error(code) + " (" + std::to_string(code) + ")";
        return false;
    }
    std::reverse(signature.begin(), signature.end());

    CERT_SIGNED_CONTENT_INFO signed_info{};
    signed_info.ToBeSigned.cbData = to_be_signed_size;
    signed_info.ToBeSigned.pbData = to_be_signed.data();
    signed_info.SignatureAlgorithm.pszObjId = const_cast<LPSTR>(hash.oid);
    signed_info.Signature.cbData = signature_size;
    signed_info.Signature.pbData = signature.data();
    signed_info.Signature.cUnusedBits = 0;

    DWORD encoded_size = 0;
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, X509_CERT, &signed_info, 0,
                             nullptr, nullptr, &encoded_size)) {
        DWORD code = GetLastError();
        error = "Cannot encode the signed certificate: " +
                format_win32_error(code) + " (" + std::to_string(code) + ")";
        return false;
    }

    encoded.resize(encoded_size);
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, X509_CERT, &signed_info, 0,
                             nullptr, encoded.data(), &encoded_size)) {
        DWORD code = GetLastError();
        error = "Cannot encode the signed certificate: " +
                format_win32_error(code) + " (" + std::to_string(code) + ")";
        return false;
    }
    return true;
}

bool create_child_certificate(
    const CertificateRequestOptions& options,
    const std::wstring& container,
    const std::wstring& parent_path,
    const std::wstring& parent_password,
    CertContextHolder& certificate,
    CertStoreHolder& output_store,
    ExtensionList& extensions,
    const HashSpec& hash,
    SYSTEMTIME start,
    SYSTEMTIME end,
    std::string& error) {
    std::vector<unsigned char> parent_data;
    if (!read_file(parent_path, parent_data, error)) return false;

    CRYPT_DATA_BLOB parent_blob{};
    parent_blob.cbData = static_cast<DWORD>(parent_data.size());
    parent_blob.pbData = parent_data.data();
    CertStoreHolder parent_store;
    parent_store.value = PFXImportCertStore(
        &parent_blob, parent_password.c_str(),
        PKCS12_NO_PERSIST_KEY | PKCS12_IMPORT_SILENT);
    if (!parent_store.value) {
        error = "Cannot open parent PFX: " +
                format_win32_error(GetLastError());
        return false;
    }

    CertContextHolder parent;
    parent.value = CertFindCertificateInStore(
        parent_store.value, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
        CERT_FIND_HAS_PRIVATE_KEY, nullptr, nullptr);
    if (!parent.value) {
        error = "The parent PFX does not contain a certificate with a private "
                "key.";
        return false;
    }

    HCRYPTPROV parent_provider = 0;
    DWORD parent_key_spec = 0;
    bool parent_provider_owned = false;
    if (!acquire_certificate_key(parent.value, parent_provider, parent_key_spec,
                                 parent_provider_owned, error)) {
        return false;
    }
    CryptProvHolder owned_parent_provider;
    if (parent_provider_owned) owned_parent_provider.value = parent_provider;

    std::vector<BYTE> subject_storage;
    CERT_NAME_BLOB subject{};
    if (!make_subject_blob(options.subject, subject_storage, subject, error)) {
        return false;
    }

    CryptProvHolder child_provider;
    CryptKeyHolder child_key;
    CRYPT_KEY_PROV_INFO child_key_info{};
    if (!create_key_provider(container, options.key_bits, child_provider,
                             child_key, child_key_info, error)) {
        return false;
    }

    DWORD public_key_size = 0;
    if (!CryptExportPublicKeyInfo(child_provider.value, AT_SIGNATURE,
                                  X509_ASN_ENCODING, nullptr,
                                  &public_key_size)) {
        error = "Cannot export the child public key: " +
                format_win32_error(GetLastError());
        return false;
    }
    std::vector<BYTE> public_key_storage(public_key_size);
    auto* public_key = reinterpret_cast<CERT_PUBLIC_KEY_INFO*>(
        public_key_storage.data());
    if (!CryptExportPublicKeyInfo(child_provider.value, AT_SIGNATURE,
                                  X509_ASN_ENCODING, public_key,
                                  &public_key_size)) {
        error = "Cannot export the child public key: " +
                format_win32_error(GetLastError());
        return false;
    }

    BYTE serial_bytes[16] = {};
    if (!CryptGenRandom(child_provider.value, sizeof(serial_bytes),
                        serial_bytes)) {
        error = "Cannot generate the certificate serial number: " +
                format_win32_error(GetLastError());
        return false;
    }
    serial_bytes[0] = static_cast<BYTE>(serial_bytes[0] & 0x7F);
    if (serial_bytes[0] == 0) serial_bytes[0] = 1;

    FILETIME not_before{};
    FILETIME not_after{};
    if (!SystemTimeToFileTime(&start, &not_before) ||
        !SystemTimeToFileTime(&end, &not_after)) {
        error = "Cannot convert the certificate validity period.";
        return false;
    }

    CERT_INFO info{};
    info.dwVersion = CERT_V3;
    info.SerialNumber.cbData = sizeof(serial_bytes);
    info.SerialNumber.pbData = serial_bytes;
    info.SignatureAlgorithm.pszObjId = const_cast<LPSTR>(hash.oid);
    info.Issuer = parent.value->pCertInfo->Subject;
    info.NotBefore = not_before;
    info.NotAfter = not_after;
    info.Subject = subject;
    info.SubjectPublicKeyInfo = *public_key;
    info.cExtension = extensions.info.cExtension;
    info.rgExtension = extensions.info.rgExtension;

    std::vector<BYTE> encoded;
    if (!sign_certificate_info(parent_provider, parent_key_spec, info, hash,
                               encoded, error)) {
        return false;
    }

    certificate.value = CertCreateCertificateContext(
        X509_ASN_ENCODING, encoded.data(),
        static_cast<DWORD>(encoded.size()));
    if (!certificate.value) {
        error = "Cannot parse the generated child certificate: " +
                format_win32_error(GetLastError());
        return false;
    }

    if (!CertSetCertificateContextProperty(
            certificate.value, CERT_KEY_PROV_INFO_PROP_ID, 0,
            &child_key_info)) {
        error = "Cannot attach the private key to the child certificate: " +
                format_win32_error(GetLastError());
        return false;
    }

    output_store.value = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0,
                                       nullptr);
    if (!output_store.value) {
        error = "Cannot create the output certificate store: " +
                format_win32_error(GetLastError());
        return false;
    }

    add_public_certificates(parent_store.value, output_store.value);
    if (!CertAddCertificateContextToStore(
            output_store.value, certificate.value,
            CERT_STORE_ADD_REPLACE_EXISTING, nullptr)) {
        error = "Cannot add the child certificate to the output store: " +
                format_win32_error(GetLastError());
        return false;
    }
    return true;
}

}  // namespace

bool generate_certificate(const CertificateRequestOptions& options,
                          CertificateResult& result,
                          std::string& error) {
    result = {};
    error.clear();

    const HashSpec* hash = find_hash_algorithm(options.hash_algorithm);
    if (!hash) {
        error = "Unsupported hash algorithm: " + options.hash_algorithm + ".";
        return false;
    }
    if (options.key_bits < 1024 || options.key_bits > 16384) {
        error = "Key length must be between 1024 and 16384.";
        return false;
    }

    std::vector<std::string> eku_oids;
    if (!parse_enhanced_key_usage(options.enhanced_key_usage, eku_oids,
                                  error)) {
        return false;
    }

    bool is_ca = false;
    ExtensionList extensions;
    if (!build_extensions(options, eku_oids, is_ca, extensions, error)) {
        return false;
    }

    SYSTEMTIME start{};
    GetSystemTime(&start);
    FILETIME start_file_time{};
    if (SystemTimeToFileTime(&start, &start_file_time)) {
        ULARGE_INTEGER value{};
        value.LowPart = start_file_time.dwLowDateTime;
        value.HighPart = start_file_time.dwHighDateTime;
        value.QuadPart -= 5ULL * 60ULL * 10000000ULL;
        start_file_time.dwLowDateTime = value.LowPart;
        start_file_time.dwHighDateTime = value.HighPart;
        FileTimeToSystemTime(&start_file_time, &start);
    }

    SYSTEMTIME end{};
    if (!parse_expiration(options.expiration, end, error)) return false;

    std::wstring container = L"SCTCert-" + utf8_to_wide(get_uuid_v4());
    std::wstring output_path = options.output_path.empty()
                                   ? L"cert.pfx"
                                   : utf8_to_wide(options.output_path);
    output_path = full_path(output_path);

    CertContextHolder certificate;
    CertStoreHolder store;
    bool created = false;

    if (options.parent_pfx.empty()) {
        if (!create_self_signed_certificate(options, container, certificate,
                                            extensions, *hash, start, end,
                                            error)) {
            delete_key_container(container);
            return false;
        }

        store.value = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr);
        if (!store.value) {
            error = "Cannot create the output certificate store: " +
                    format_win32_error(GetLastError());
            delete_key_container(container);
            return false;
        }
        if (!CertAddCertificateContextToStore(
                store.value, certificate.value,
                CERT_STORE_ADD_REPLACE_EXISTING, nullptr)) {
            error = "Cannot add the certificate to the output store: " +
                    format_win32_error(GetLastError());
            delete_key_container(container);
            return false;
        }
        created = true;
    } else {
        std::wstring parent_path = full_path(utf8_to_wide(options.parent_pfx));
        if (!create_child_certificate(
                options, container, parent_path,
                utf8_to_wide(options.parent_password), certificate, store,
                extensions, *hash, start, end, error)) {
            delete_key_container(container);
            return false;
        }
        created = true;
    }

    if (!created) {
        error = "Certificate generation did not complete.";
        return false;
    }

    bool pfx = is_pfx_path(output_path);
    bool exported = pfx
                        ? export_certificate_store(
                              store.value, output_path,
                              utf8_to_wide(options.password), options.force,
                              error)
                        : export_certificate_der(certificate.value, output_path,
                                                 options.force, error);

    result.subject = certificate_subject(certificate.value);
    result.thumbprint_sha256 = certificate_sha256(certificate.value);
    result.not_after = certificate_not_after(certificate.value);
    result.output_path = wide_to_utf8(output_path);

    delete_key_container(container);
    return exported;
}

struct KeyContainerInfo {
    std::wstring container;
    std::wstring provider;
    DWORD provider_type = 0;
    DWORD key_spec = 0;
    bool valid = false;
};

bool read_key_container_info(PCCERT_CONTEXT certificate,
                             KeyContainerInfo& info) {
    info = {};
    DWORD size = 0;
    if (!CertGetCertificateContextProperty(
            certificate, CERT_KEY_PROV_INFO_PROP_ID, nullptr, &size) ||
        size < sizeof(CRYPT_KEY_PROV_INFO)) {
        return false;
    }

    std::vector<BYTE> buffer(size);
    if (!CertGetCertificateContextProperty(
            certificate, CERT_KEY_PROV_INFO_PROP_ID, buffer.data(), &size)) {
        return false;
    }

    auto* key_info = reinterpret_cast<CRYPT_KEY_PROV_INFO*>(buffer.data());
    if (!key_info->pwszContainerName || !key_info->pwszProvName) return false;

    info.container = key_info->pwszContainerName;
    info.provider = key_info->pwszProvName;
    info.provider_type = key_info->dwProvType;
    info.key_spec = key_info->dwKeySpec;
    info.valid = true;
    return true;
}

bool key_container_exists(const KeyContainerInfo& info) {
    if (!info.valid) return true;

    HCRYPTPROV provider = 0;
    if (CryptAcquireContextW(&provider, info.container.c_str(),
                             info.provider.c_str(), info.provider_type,
                             CRYPT_SILENT)) {
        CryptReleaseContext(provider, 0);
        return true;
    }

    DWORD code = GetLastError();
    return code != static_cast<DWORD>(NTE_BAD_KEYSET) &&
           code != static_cast<DWORD>(NTE_NO_KEY);
}

void delete_key_container_info(const KeyContainerInfo& info) {
    if (!info.valid) return;

    HCRYPTPROV provider = 0;
    CryptAcquireContextW(&provider, info.container.c_str(),
                         info.provider.c_str(), info.provider_type,
                         CRYPT_DELETEKEYSET);
}

bool sign_file_with_pfx(const std::string& pfx_path,
                        const std::string& password,
                        const std::string& file_path,
                        std::string& signer_subject,
                        std::string& error) {
    signer_subject.clear();
    error.clear();

    std::wstring pfx_wide = full_path(utf8_to_wide(pfx_path));
    std::wstring file_wide = full_path(utf8_to_wide(file_path));

    DWORD attributes = GetFileAttributesW(file_wide.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        error = "Target file not found: " + wide_to_utf8(file_wide);
        return false;
    }

    std::vector<unsigned char> pfx_data;
    if (!read_file(pfx_wide, pfx_data, error)) return false;

    CRYPT_DATA_BLOB pfx_blob{};
    pfx_blob.cbData = static_cast<DWORD>(pfx_data.size());
    pfx_blob.pbData = pfx_data.data();
    std::wstring password_wide = utf8_to_wide(password);

    KeyContainerInfo original_info;
    bool container_existed = true;
    {
        CertStoreHolder inspect_store;
        inspect_store.value = PFXImportCertStore(
            &pfx_blob, password_wide.c_str(),
            PKCS12_NO_PERSIST_KEY | PKCS12_IMPORT_SILENT);
        if (!inspect_store.value) {
            error = "Cannot open PFX: " + format_win32_error(GetLastError());
            return false;
        }

        CertContextHolder inspect_certificate;
        inspect_certificate.value = CertFindCertificateInStore(
            inspect_store.value, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
            CERT_FIND_HAS_PRIVATE_KEY, nullptr, nullptr);
        if (!inspect_certificate.value) {
            error =
                "The PFX does not contain a certificate with a private key.";
            return false;
        }

        if (read_key_container_info(inspect_certificate.value, original_info)) {
            container_existed = key_container_exists(original_info);
        }
    }

    CertStoreHolder store;
    store.value = PFXImportCertStore(
        &pfx_blob, password_wide.c_str(), PKCS12_IMPORT_SILENT);
    if (!store.value) {
        error = "Cannot open PFX: " + format_win32_error(GetLastError());
        return false;
    }

    CertContextHolder certificate;
    certificate.value = CertFindCertificateInStore(
        store.value, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
        CERT_FIND_HAS_PRIVATE_KEY, nullptr, nullptr);
    if (!certificate.value) {
        error = "The PFX does not contain a certificate with a private key.";
        return false;
    }

    KeyContainerInfo imported_info;
    read_key_container_info(certificate.value, imported_info);
    signer_subject = certificate_subject(certificate.value);

    bool should_delete = original_info.valid && imported_info.valid &&
                         !container_existed &&
                         imported_info.container == original_info.container;
    auto cleanup_store = [&]() {
        if (certificate.value) {
            CertFreeCertificateContext(certificate.value);
            certificate.value = nullptr;
        }
        if (store.value) {
            CertCloseStore(store.value, 0);
            store.value = nullptr;
        }
        if (should_delete) delete_key_container_info(imported_info);
    };

    HMODULE module = LoadLibraryW(L"mssign32.dll");
    if (!module) {
        cleanup_store();
        error = "Cannot load mssign32.dll: " +
                format_win32_error(GetLastError());
        return false;
    }

    using SignerSignFn = HRESULT(WINAPI*)(SIGNER_SUBJECT_INFO*,
                                         SIGNER_CERT*,
                                         SIGNER_SIGNATURE_INFO*,
                                         SIGNER_PROVIDER_INFO*,
                                         LPCWSTR,
                                         PCRYPT_ATTRIBUTES,
                                         LPVOID);
    using SignerSignEx2Fn = HRESULT(WINAPI*)(
        DWORD, SIGNER_SUBJECT_INFO*, SIGNER_CERT*, SIGNER_SIGNATURE_INFO*,
        SIGNER_PROVIDER_INFO*, DWORD, PCSTR, PCWSTR, PCRYPT_ATTRIBUTES,
        PVOID, SIGNER_CONTEXT**, PVOID, PVOID);
    using SignerFreeSignerContextFn = HRESULT(WINAPI*)(SIGNER_CONTEXT*);

    FARPROC procedure = GetProcAddress(module, "SignerSignEx2");
    SignerSignEx2Fn signer_sign_ex2 = nullptr;
    if (procedure) {
        static_assert(sizeof(signer_sign_ex2) == sizeof(procedure));
        std::memcpy(&signer_sign_ex2, &procedure, sizeof(signer_sign_ex2));
    }
    procedure = GetProcAddress(module, "SignerSign");
    if (!procedure && !signer_sign_ex2) {
        FreeLibrary(module);
        cleanup_store();
        error = "mssign32.dll does not export SignerSign.";
        return false;
    }
    SignerSignFn signer_sign = nullptr;
    if (procedure) {
        static_assert(sizeof(signer_sign) == sizeof(procedure));
        std::memcpy(&signer_sign, &procedure, sizeof(signer_sign));
    }

    SIGNER_FILE_INFO file_info{};
    file_info.cbSize = sizeof(file_info);
    file_info.pwszFileName = file_wide.c_str();
    file_info.hFile = INVALID_HANDLE_VALUE;

    DWORD subject_index = 0;
    SIGNER_SUBJECT_INFO subject_info{};
    subject_info.cbSize = sizeof(subject_info);
    subject_info.pdwIndex = &subject_index;
    subject_info.dwSubjectChoice = SIGNER_SUBJECT_FILE;
    subject_info.pSignerFileInfo = &file_info;

    SIGNER_CERT_STORE_INFO cert_store_info{};
    cert_store_info.cbSize = sizeof(cert_store_info);
    cert_store_info.pSigningCert = certificate.value;
    cert_store_info.dwCertPolicy = SIGNER_CERT_POLICY_STORE;
    cert_store_info.hCertStore = store.value;

    SIGNER_CERT signer_cert{};
    signer_cert.cbSize = sizeof(signer_cert);
    signer_cert.dwCertChoice = SIGNER_CERT_STORE;
    signer_cert.pCertStoreInfo = &cert_store_info;

    SIGNER_SIGNATURE_INFO signature_info{};
    signature_info.cbSize = sizeof(signature_info);
    signature_info.algidHash = CALG_SHA_256;
    signature_info.dwAttrChoice = SIGNER_NO_ATTR;

    HRESULT result = E_FAIL;
    SIGNER_CONTEXT* signer_context = nullptr;
    if (signer_sign_ex2) {
        result = signer_sign_ex2(
            0, &subject_info, &signer_cert, &signature_info, nullptr, 0,
            nullptr, nullptr, nullptr, nullptr, &signer_context, nullptr,
            nullptr);
        if (signer_context) {
            procedure = GetProcAddress(module, "SignerFreeSignerContext");
            if (procedure) {
                SignerFreeSignerContextFn free_context = nullptr;
                static_assert(sizeof(free_context) == sizeof(procedure));
                std::memcpy(&free_context, &procedure, sizeof(free_context));
                free_context(signer_context);
            }
        }
    } else if (signer_sign) {
        result = signer_sign(
            &subject_info, &signer_cert, &signature_info, nullptr, nullptr,
            nullptr, nullptr);
    }
    FreeLibrary(module);
    cleanup_store();

    if (FAILED(result)) {
        std::ostringstream code;
        code << "0x" << std::hex << std::uppercase
             << static_cast<unsigned long>(result);
        error = "Cannot sign '" + wide_to_utf8(file_wide) + "': " +
                format_win32_error(static_cast<DWORD>(result)) + " (" +
                code.str() + ")";
        return false;
    }
    return true;
}
