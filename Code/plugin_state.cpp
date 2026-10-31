#include "global.h"

namespace {

constexpr unsigned char kStateKey[32] = {
    0x78, 0xb8, 0x91, 0xa7, 0x42, 0x49, 0x61, 0xd8,
    0x4f, 0x74, 0x0d, 0x2e, 0x75, 0x8a, 0x36, 0x0e,
    0xe3, 0x83, 0xd8, 0x0c, 0xcb, 0xfa, 0x3d, 0xf6,
    0x71, 0x51, 0xa5, 0x23, 0x23, 0x2b, 0x48, 0xfb
};

constexpr unsigned char kStateMagic[4] = {'S', 'C', 'T', 'P'};
constexpr uint32_t kStateVersion = 1;
constexpr size_t kNonceSize = 12;
constexpr size_t kTagSize = 16;
constexpr size_t kHeaderSize = 8;
constexpr size_t kEnvelopeOverhead = kHeaderSize + kNonceSize + 4 + kTagSize;

bool is_hex_hash(const std::string& text) {
    if (text.size() != 64) return false;
    for (char ch : text) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
              (ch >= 'A' && ch <= 'F'))) {
            return false;
        }
    }
    return true;
}

std::string normalize_hash(const std::string& text) {
    return lower_ascii(text);
}

bool read_binary_file(const std::wstring& path,
                      std::vector<unsigned char>& data,
                      DWORD& error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        size.QuadPart > 4ll * 1024ll * 1024ll) {
        error = GetLastError();
        if (error == ERROR_SUCCESS) error = ERROR_NOT_ENOUGH_MEMORY;
        CloseHandle(file);
        return false;
    }

    data.assign(static_cast<size_t>(size.QuadPart), 0);
    size_t offset = 0;
    while (offset < data.size()) {
        DWORD read = 0;
        DWORD remaining = static_cast<DWORD>(data.size() - offset);
        if (!ReadFile(file, data.data() + offset, remaining, &read, nullptr) ||
            read == 0) {
            error = GetLastError();
            if (error == ERROR_SUCCESS) error = ERROR_READ_FAULT;
            CloseHandle(file);
            return false;
        }
        offset += read;
    }

    CloseHandle(file);
    error = ERROR_SUCCESS;
    return true;
}

bool write_binary_file(const std::wstring& path,
                       const std::vector<unsigned char>& data,
                       DWORD& error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return false;
    }

    size_t offset = 0;
    while (offset < data.size()) {
        DWORD written = 0;
        DWORD remaining = static_cast<DWORD>(data.size() - offset);
        if (!WriteFile(file, data.data() + offset, remaining, &written,
                       nullptr) ||
            written == 0) {
            error = GetLastError();
            if (error == ERROR_SUCCESS) error = ERROR_WRITE_FAULT;
            CloseHandle(file);
            return false;
        }
        offset += written;
    }

    if (!FlushFileBuffers(file)) {
        error = GetLastError();
        CloseHandle(file);
        return false;
    }

    CloseHandle(file);
    error = ERROR_SUCCESS;
    return true;
}

void append_u32(std::vector<unsigned char>& data, uint32_t value) {
    data.push_back(static_cast<unsigned char>(value & 0xff));
    data.push_back(static_cast<unsigned char>((value >> 8) & 0xff));
    data.push_back(static_cast<unsigned char>((value >> 16) & 0xff));
    data.push_back(static_cast<unsigned char>((value >> 24) & 0xff));
}

uint32_t read_u32(const unsigned char* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

bool aes_gcm_encrypt(const std::string& plaintext,
                     const std::vector<unsigned char>& header,
                     std::vector<unsigned char>& envelope,
                     std::string& error) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status)) {
        error = "Failed to open AES provider.";
        return false;
    }

    status = BCryptSetProperty(
        algorithm, BCRYPT_CHAINING_MODE,
        reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
        sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
    if (!BCRYPT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        error = "Failed to configure AES-GCM.";
        return false;
    }

    status = BCryptGenerateSymmetricKey(algorithm, &key, nullptr, 0,
                                        const_cast<PUCHAR>(kStateKey),
                                        sizeof(kStateKey), 0);
    if (!BCRYPT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        error = "Failed to create plugin state key.";
        return false;
    }

    std::vector<unsigned char> nonce(kNonceSize, 0);
    status = BCryptGenRandom(nullptr, nonce.data(),
                             static_cast<ULONG>(nonce.size()),
                             BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(status)) {
        BCryptDestroyKey(key);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        error = "Failed to generate plugin state nonce.";
        return false;
    }

    std::vector<unsigned char> ciphertext(
        std::max<size_t>(plaintext.size(), 1), 0);
    std::vector<unsigned char> tag(kTagSize, 0);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth;
    BCRYPT_INIT_AUTH_MODE_INFO(auth);
    auth.pbNonce = nonce.data();
    auth.cbNonce = static_cast<ULONG>(nonce.size());
    auth.pbAuthData = const_cast<PUCHAR>(header.data());
    auth.cbAuthData = static_cast<ULONG>(header.size());
    auth.pbTag = tag.data();
    auth.cbTag = static_cast<ULONG>(tag.size());

    ULONG ciphertext_size = 0;
    status = BCryptEncrypt(
        key, reinterpret_cast<PUCHAR>(const_cast<char*>(plaintext.data())),
        static_cast<ULONG>(plaintext.size()), &auth, nullptr, 0,
        ciphertext.data(), static_cast<ULONG>(ciphertext.size()),
        &ciphertext_size, 0);
    if (!BCRYPT_SUCCESS(status)) {
        BCryptDestroyKey(key);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        error = "Failed to encrypt plugin state.";
        return false;
    }

    envelope.clear();
    envelope.reserve(kEnvelopeOverhead + ciphertext_size);
    envelope.insert(envelope.end(), header.begin(), header.end());
    envelope.insert(envelope.end(), nonce.begin(), nonce.end());
    append_u32(envelope, ciphertext_size);
    envelope.insert(envelope.end(), ciphertext.begin(),
                    ciphertext.begin() + ciphertext_size);
    envelope.insert(envelope.end(), tag.begin(), tag.end());

    BCryptDestroyKey(key);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return true;
}

bool aes_gcm_decrypt(const std::vector<unsigned char>& envelope,
                     std::string& plaintext,
                     std::string& error) {
    if (envelope.size() < kEnvelopeOverhead) {
        error = "Plugin state file is too short.";
        return false;
    }

    std::vector<unsigned char> header(envelope.begin(),
                                      envelope.begin() + kHeaderSize);
    const unsigned char* nonce = envelope.data() + kHeaderSize;
    uint32_t ciphertext_size =
        read_u32(envelope.data() + kHeaderSize + kNonceSize);
    size_t ciphertext_offset = kHeaderSize + kNonceSize + 4;
    if (ciphertext_size > envelope.size() - ciphertext_offset - kTagSize ||
        envelope.size() != ciphertext_offset + ciphertext_size + kTagSize) {
        error = "Plugin state file has an invalid length.";
        return false;
    }

    const unsigned char* ciphertext = envelope.data() + ciphertext_offset;
    const unsigned char* tag = ciphertext + ciphertext_size;

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status)) {
        error = "Failed to open AES provider.";
        return false;
    }

    status = BCryptSetProperty(
        algorithm, BCRYPT_CHAINING_MODE,
        reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
        sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
    if (!BCRYPT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        error = "Failed to configure AES-GCM.";
        return false;
    }

    status = BCryptGenerateSymmetricKey(algorithm, &key, nullptr, 0,
                                        const_cast<PUCHAR>(kStateKey),
                                        sizeof(kStateKey), 0);
    if (!BCRYPT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        error = "Failed to create plugin state key.";
        return false;
    }

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth;
    BCRYPT_INIT_AUTH_MODE_INFO(auth);
    auth.pbNonce = const_cast<PUCHAR>(nonce);
    auth.cbNonce = static_cast<ULONG>(kNonceSize);
    auth.pbAuthData = header.data();
    auth.cbAuthData = static_cast<ULONG>(header.size());
    auth.pbTag = const_cast<PUCHAR>(tag);
    auth.cbTag = static_cast<ULONG>(kTagSize);

    std::vector<unsigned char> output(
        std::max<uint32_t>(ciphertext_size, 1), 0);
    ULONG output_size = 0;
    status = BCryptDecrypt(
        key, const_cast<PUCHAR>(ciphertext), ciphertext_size, &auth, nullptr,
        0, output.data(), static_cast<ULONG>(output.size()), &output_size, 0);

    BCryptDestroyKey(key);
    BCryptCloseAlgorithmProvider(algorithm, 0);

    if (!BCRYPT_SUCCESS(status)) {
        error = "Plugin state authentication failed.";
        return false;
    }

    plaintext.assign(reinterpret_cast<char*>(output.data()), output_size);
    return true;
}

std::string serialize_state(const PluginState& state) {
    std::string text = "SCTP1\n";
    for (const std::string& hash : state.enabled) {
        text += "E " + hash + "\n";
    }
    for (const std::string& hash : state.disabled) {
        text += "D " + hash + "\n";
    }
    return text;
}

bool parse_state(const std::string& text, PluginState& state,
                 std::string& error) {
    PluginState parsed;
    std::istringstream input(text);
    std::string line;
    if (!std::getline(input, line)) {
        error = "Plugin state is empty.";
        return false;
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line != "SCTP1") {
        error = "Plugin state header is invalid.";
        return false;
    }

    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line.size() < 3 || line[1] != ' ') {
            error = "Plugin state contains an invalid line.";
            return false;
        }
        std::string hash = normalize_hash(line.substr(2));
        if (!is_hex_hash(hash)) {
            error = "Plugin state contains an invalid plugin hash.";
            return false;
        }
        if (line[0] == 'E') {
            parsed.enabled.insert(hash);
        } else if (line[0] == 'D') {
            parsed.disabled.insert(hash);
        } else {
            error = "Plugin state contains an unknown entry.";
            return false;
        }
    }

    state = std::move(parsed);
    return true;
}

}

bool load_plugin_state(const std::wstring& path,
                       PluginState& state,
                       std::string& error) {
    state = PluginState{};
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return true;
        }
        error = "Failed to access plugin state: " + format_win32_error(code);
        return false;
    }

    std::vector<unsigned char> envelope;
    DWORD read_error = ERROR_SUCCESS;
    if (!read_binary_file(path, envelope, read_error)) {
        error = "Failed to read plugin state: " +
                format_win32_error(read_error);
        return false;
    }

    if (envelope.size() < kHeaderSize ||
        std::memcmp(envelope.data(), kStateMagic, sizeof(kStateMagic)) != 0 ||
        read_u32(envelope.data() + 4) != kStateVersion) {
        error = "Plugin state file is invalid or unsupported.";
        return false;
    }

    std::string plaintext;
    if (!aes_gcm_decrypt(envelope, plaintext, error)) return false;
    return parse_state(plaintext, state, error);
}

bool save_plugin_state(const std::wstring& path,
                       const PluginState& state,
                       std::string& error) {
    std::vector<unsigned char> header;
    header.insert(header.end(), std::begin(kStateMagic),
                  std::end(kStateMagic));
    append_u32(header, kStateVersion);

    std::vector<unsigned char> envelope;
    if (!aes_gcm_encrypt(serialize_state(state), header, envelope, error)) {
        return false;
    }

    std::wstring temporary = path + L".tmp." +
                             std::to_wstring(GetCurrentProcessId()) + L"." +
                             std::to_wstring(GetTickCount64());
    DWORD write_error = ERROR_SUCCESS;
    if (!write_binary_file(temporary, envelope, write_error)) {
        DeleteFileW(temporary.c_str());
        error = "Failed to write plugin state: " +
                format_win32_error(write_error);
        return false;
    }

    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD move_error = GetLastError();
        DeleteFileW(temporary.c_str());
        error = "Failed to replace plugin state: " +
                format_win32_error(move_error);
        return false;
    }

    return true;
}
