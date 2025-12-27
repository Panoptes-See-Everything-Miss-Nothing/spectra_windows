#include "Utils.h"
#include <aclapi.h>
#include <sddl.h>

// Static mutex for thread-safe logging
static std::mutex g_logMutex;

// Helper: Check if IP is loopback
bool IsLoopbackIP(const std::string& ip)
{
    return (ip == "127.0.0.1" || ip == "::1");
}

void LogError(const std::string& message)
{
    struct tm timeinfo;
    // Lock for thread safety
    std::lock_guard<std::mutex> lock(g_logMutex);
    fs::path logPath = fs::current_path() / "spectra_log.txt";
    
    try {
        std::ofstream logFile(logPath, std::ios::app);
        
        if (!logFile.is_open()) {
            // Fail gracefully - write to stderr if we can't open log
            std::cerr << "ERROR: Could not open log file: " << logPath << std::endl;
            return;
        }

        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        
        if (localtime_s(&timeinfo, &time) != 0) {
            logFile << "[TIMESTAMP_ERROR] " << message << std::endl;
        } else {
            logFile << "[" << std::put_time(&timeinfo, "%Y-%m-%d %H:%M:%S") << "] " << message << std::endl;
        }
        
        logFile.close();
    }
    catch (const std::exception& e) {
        std::cerr << "ERROR: Exception in LogError(): " << e.what() << std::endl;
    }
    catch (...) {
        std::cerr << "ERROR: Unknown exception in LogError()" << std::endl;
    }
}

void LogWideStringAsUtf8(const std::string& prefix, const std::wstring& value)
{
    // Convert wstring to UTF-8 for logging
    if (value.empty()) {
        LogError(prefix + "<empty>");
        return;
    }

    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (sizeNeeded <= 0) {
        LogError(prefix + "<conversion failed>");
        return;
    }

    std::string utf8(static_cast<size_t>(sizeNeeded - 1), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, utf8.data(), sizeNeeded, nullptr, nullptr) <= 0) {
        LogError(prefix + "<conversion failed>");
        return;
    }

    LogError(prefix + utf8);
}

// UTF-8 JSON ESCAPE
std::string JsonEscape(const std::wstring& input)
{
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, input.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (sizeNeeded <= 0) return "\"\"";

    std::string utf8(sizeNeeded - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.c_str(), -1, utf8.data(), sizeNeeded, nullptr, nullptr);

    std::ostringstream out;
    out << "\"";

    for (char c : utf8)
    {
        switch (c)
        {
        case '\"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default: out << c; break;
        }
    }

    out << "\"";
    return out.str();
}

void WriteJSONToFile(const std::string& jsonData, const std::wstring& filename)
{
    std::filesystem::path outputPath = std::filesystem::current_path() / filename;

    std::ofstream outFile(outputPath, std::ios::out | std::ios::trunc);
    if (outFile.is_open()) {
        outFile << jsonData;
        outFile.close();
        std::wcout << L"JSON written to: " << outputPath << std::endl;
    }
    else {
        std::cerr << "Failed to open file for writing." << std::endl;
    }
}

// Helper: Convert wstring to UTF-8 string safely
std::string WideToUtf8(const std::wstring& wstr)
{
    if (wstr.empty()) return {};

    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (sizeNeeded <= 0) return {};

    std::string utf8(static_cast<size_t>(sizeNeeded - 1), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, utf8.data(), sizeNeeded, nullptr, nullptr) <= 0) {
        return {};
    }

    return utf8;
}

// Helper: Check if username is a system account
bool IsSystemAccount(const std::wstring& username)
{
    static constexpr std::array<std::wstring_view, 10> systemAccounts = {
        L"Default", L"Public", L"All Users", L"Default User",
        L"SYSTEM", L"LOCAL SERVICE", L"NETWORK SERVICE",
        L"systemprofile", L"LocalService", L"NetworkService"
    };

    for (const auto& sysAccount : systemAccounts)
    {
        if (_wcsicmp(username.c_str(), sysAccount.data()) == 0)
            return true;
    }

    return false;
}

// Helper: Translate Windows error code to message
std::string GetWindowsErrorMessage(LONG errorCode)
{
    LPWSTR messageBuffer = nullptr;
    FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errorCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPWSTR)&messageBuffer,
        0,
        nullptr
    );

    std::wstring errorMsg = messageBuffer ? messageBuffer : L"Unknown error";
    LocalFree(messageBuffer);

    // Remove trailing newline/carriage return
    while (!errorMsg.empty() && (errorMsg.back() == L'\n' || errorMsg.back() == L'\r'))
        errorMsg.pop_back();

    return WideToUtf8(errorMsg);
}

// Directory Security Validation Functions

bool IsDirectory(const std::wstring& path)
{
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return false;
    
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool IsReparsePoint(const std::wstring& path)
{
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return false;
    
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

PSID GetCurrentUserSid()
{
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
        return nullptr;

    DWORD dwBufferSize = 0;
    GetTokenInformation(hToken, TokenUser, nullptr, 0, &dwBufferSize);
    
    std::vector<BYTE> buffer(dwBufferSize);
    TOKEN_USER* pTokenUser = reinterpret_cast<TOKEN_USER*>(buffer.data());
    
    if (!GetTokenInformation(hToken, TokenUser, pTokenUser, dwBufferSize, &dwBufferSize))
    {
        CloseHandle(hToken);
        return nullptr;
    }

    // Allocate and copy the SID
    DWORD sidLength = GetLengthSid(pTokenUser->User.Sid);
    PSID pSidCopy = LocalAlloc(LPTR, sidLength);
    if (pSidCopy == nullptr)
    {
        CloseHandle(hToken);
        return nullptr;
    }

    if (!CopySid(sidLength, pSidCopy, pTokenUser->User.Sid))
    {
        LocalFree(pSidCopy);
        CloseHandle(hToken);
        return nullptr;
    }

    CloseHandle(hToken);
    return pSidCopy;
}

void FreeUserSid(PSID pSid)
{
    if (pSid)
        LocalFree(pSid);
}

bool IsDirectoryOwnedByTrustedUser(const std::wstring& path)
{
    PSID pOwnerSid = nullptr;
    PSECURITY_DESCRIPTOR pSD = nullptr;
    
    // Get the owner of the directory
    DWORD dwResult = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT,
                                           OWNER_SECURITY_INFORMATION,
                                           &pOwnerSid, nullptr, nullptr, nullptr, &pSD);
    
    if (dwResult != ERROR_SUCCESS)
    {
        LogError("[-] Failed to get directory owner for: " + WideToUtf8(path) + 
                 ", error: " + std::to_string(dwResult));
        return false;
    }

    // Get current user's SID
    PSID pCurrentUserSid = GetCurrentUserSid();
    if (pCurrentUserSid == nullptr)
    {
        LocalFree(pSD);
        LogError("[-] Failed to get current user SID for ownership verification");
        return false;
    }

    // Get SYSTEM SID for comparison
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    PSID pSystemSid = nullptr;
    if (!AllocateAndInitializeSid(&ntAuthority, 1, SECURITY_LOCAL_SYSTEM_RID,
                                  0, 0, 0, 0, 0, 0, 0, &pSystemSid))
    {
        FreeUserSid(pCurrentUserSid);
        LocalFree(pSD);
        LogError("[-] Failed to create SYSTEM SID for ownership verification");
        return false;
    }

    // Check if owner is current user or SYSTEM
    bool isOwnerValid = EqualSid(pOwnerSid, pCurrentUserSid) || 
                        EqualSid(pOwnerSid, pSystemSid);

    FreeSid(pSystemSid);
    FreeUserSid(pCurrentUserSid);
    LocalFree(pSD);

    if (!isOwnerValid)
    {
        LogError("[-] Directory is owned by an untrusted user: " + WideToUtf8(path));
    }

    return isOwnerValid;
}

bool IsDirectorySafeToUse(const std::wstring& path)
{
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        // Directory doesn't exist - this is safe (we'll create it)
        return true;
    }

    // Check 1: Verify it's actually a directory, not a file
    if (!IsDirectory(path))
    {
        LogError("[-] Security: Path exists but is not a directory: " + WideToUtf8(path));
        return false;
    }

    // Check 2: Verify it's not a reparse point (symbolic link, junction, etc.)
    if (IsReparsePoint(path))
    {
        LogError("[-] Security: Directory is a reparse point (symlink/junction), potential attack: " + WideToUtf8(path));
        return false;
    }

    // Check 3: Verify ownership - directory must be owned by current user or SYSTEM
    if (!IsDirectoryOwnedByTrustedUser(path))
    {
        LogError("[-] Security: Directory is owned by untrusted user: " + WideToUtf8(path));
        return false;
    }

    return true;
}