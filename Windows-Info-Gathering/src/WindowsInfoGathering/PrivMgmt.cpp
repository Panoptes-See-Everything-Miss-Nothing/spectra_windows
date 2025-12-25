#include "PrivMgmt.h"

// Enable a given privilege for the current process
bool EnablePrivilege(const std::wstring& privilegeName)
{
    HANDLE hToken = nullptr;
    TOKEN_PRIVILEGES tp = {};
    LUID luid = {};
    DWORD error = 0;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
    {
        LogError("[-] Failed to open process token for privilege elevation, error: " + std::to_string(GetLastError()));
        return false;
    }

    // Lookup privilege LUID
    if (!LookupPrivilegeValueW(nullptr, privilegeName.c_str(), &luid))
    {
        error = GetLastError();
        CloseHandle(hToken);
        LogError("[-] Failed to lookup privilege '" + WideToUtf8(privilegeName) + "', error: " + std::to_string(error));
        return false;
    }

    // After a successful lookup, set up TOKEN_PRIVILEGES structure
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid; 
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

     // Adjust token privileges
    if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), nullptr, nullptr))
    {
        error = GetLastError();
        CloseHandle(hToken);
        LogError("[-] Failed to adjust token privileges, error: " + std::to_string(error));
        return false;
    }

    // Check if privilege was actually granted
    if (GetLastError() == ERROR_NOT_ALL_ASSIGNED)
    {
        CloseHandle(hToken);
        LogError("[-] Privilege '" + WideToUtf8(privilegeName) + "' not assigned to current token");
        return false;
    }

    CloseHandle(hToken);
    LogError("[+] Successfully enabled privilege: " + WideToUtf8(privilegeName));

    return true;
}

// Disable a specific privilege for the current process
bool DisablePrivilege(const std::wstring& privilegeName)
{
    HANDLE hToken = nullptr;
    TOKEN_PRIVILEGES tp = {};
    LUID luid = {};
    DWORD error = 0;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
    {
        LogError("[-] Failed to open process token for privilege removal, error: " + std::to_string(GetLastError()));
        return false;
    }

    if (!LookupPrivilegeValueW(nullptr, privilegeName.c_str(), &luid))
    {
        error = GetLastError();
        CloseHandle(hToken);
        LogError("[-] Failed to lookup privilege for removal, error: " + std::to_string(error));
        return false;
    }

    // After a successful lookup, set up TOKEN_PRIVILEGES structure
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = 0;  // Disable

    if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), nullptr, nullptr))
    {
        error = GetLastError();
        CloseHandle(hToken);
        LogError("[-] Failed to disable privilege, error: " + std::to_string(error));
        return false;
    }

    CloseHandle(hToken);

    return true;
}
