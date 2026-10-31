#include "global.h"

#include <aclapi.h>

namespace {

std::atomic<bool> keep_enabled{false};
std::mutex keep_mutex;
PSECURITY_DESCRIPTOR original_descriptor = nullptr;

std::wstring current_user_sid() {
    std::wstring sid = get_process_user_sid();
    if (sid.empty()) sid = L"OW";
    return sid;
}

}

bool keep_protection_enabled() {
    return keep_enabled.load();
}

bool enable_keep_protection(std::string& error) {
    std::lock_guard<std::mutex> lock(keep_mutex);
    if (keep_enabled.load()) return true;

    PSECURITY_DESCRIPTOR original = nullptr;
    DWORD result = GetSecurityInfo(
        GetCurrentProcess(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr, &original);
    if (result != ERROR_SUCCESS) {
        error = "Cannot read the process DACL: " +
                format_win32_error(result);
        return false;
    }

    std::wstring sddl =
        L"D:(D;;0x00000001;;;WD)"
        L"(A;;GA;;;SY)"
        L"(A;;GA;;;BA)"
        L"(A;;GA;;;S-1-5-80-956008885-3418522649-1831038044-"
        L"1853292631-2271478464)"
        L"(A;;GA;;;OW)"
        L"(A;;GA;;;" + current_user_sid() + L")";

    PSECURITY_DESCRIPTOR protected_descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &protected_descriptor, nullptr)) {
        result = GetLastError();
        LocalFree(original);
        error = "Cannot build the protected process DACL: " +
                format_win32_error(result);
        return false;
    }

    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL dacl = nullptr;
    if (!GetSecurityDescriptorDacl(protected_descriptor, &present, &dacl,
                                   &defaulted) ||
        !present) {
        result = GetLastError();
        LocalFree(protected_descriptor);
        LocalFree(original);
        error = "Cannot extract the protected process DACL: " +
                format_win32_error(result);
        return false;
    }

    result = SetSecurityInfo(GetCurrentProcess(), SE_KERNEL_OBJECT,
                             DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl,
                             nullptr);
    LocalFree(protected_descriptor);
    if (result != ERROR_SUCCESS) {
        LocalFree(original);
        error = "Cannot apply the protected process DACL: " +
                format_win32_error(result);
        return false;
    }

    original_descriptor = original;
    keep_enabled.store(true);
    return true;
}

bool disable_keep_protection(std::string& error) {
    std::lock_guard<std::mutex> lock(keep_mutex);
    if (!keep_enabled.load()) return true;
    if (!original_descriptor) {
        error = "The original process DACL is unavailable.";
        return false;
    }

    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL dacl = nullptr;
    if (!GetSecurityDescriptorDacl(original_descriptor, &present, &dacl,
                                   &defaulted) ||
        !present) {
        DWORD result = GetLastError();
        error = "Cannot read the original process DACL: " +
                format_win32_error(result);
        return false;
    }

    DWORD result = SetSecurityInfo(GetCurrentProcess(), SE_KERNEL_OBJECT,
                                   DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                   dacl, nullptr);
    if (result != ERROR_SUCCESS) {
        error = "Cannot restore the original process DACL: " +
                format_win32_error(result);
        return false;
    }

    LocalFree(original_descriptor);
    original_descriptor = nullptr;
    keep_enabled.store(false);
    return true;
}
