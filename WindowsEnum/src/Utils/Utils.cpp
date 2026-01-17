#include "Utils.h"
#include "../WindowsEnum.h"
#include "../WinAppXPackages.h"
#include "../Service/MachineId.h"
#include "../Service/ServiceConfig.h"
#include <aclapi.h>
#include <sddl.h>
#include <unordered_map>

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

bool IsRunningUnderWow64()
{
#ifdef _WIN64
    // 64-bit build always returns false (can't be WOW64)
    return false;
#else
    // 32-bit build: check if running under WOW64
    typedef BOOL (WINAPI *LPFN_ISWOW64PROCESS)(HANDLE, PBOOL);
    LPFN_ISWOW64PROCESS fnIsWow64Process = (LPFN_ISWOW64PROCESS)GetProcAddress(
        GetModuleHandleW(L"kernel32"), "IsWow64Process");
    
    if (fnIsWow64Process != nullptr)
    {
        BOOL isWow64 = FALSE;
        if (fnIsWow64Process(GetCurrentProcess(), &isWow64))
        {
            return isWow64 == TRUE;
        }
    }
    return false;
#endif
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

std::string GenerateJSON()
{
    std::unordered_map<std::wstring, std::vector<InstalledApp>> userApps;
    std::unordered_map<std::wstring, std::wstring> userSIDs;  // Map username -> SID
    std::unordered_map<std::wstring, std::vector<ModernAppPackage>> userAppXPackages;  // Map username -> Modern AppX packages (WinRT)
    std::unordered_map<std::wstring, std::vector<MsiApp>> userMsiApps;  // Map username -> MSI products
    std::vector<InstalledApp> systemApps = {};
    std::vector<MsiApp> systemMsiApps = {};
    MachineNames machineNames = GetMachineName();
    std::vector<std::string> svipAddresses = {};

    // Get system wide installed applications from x64 and WOW6432 registry keys
    // Note: 32-bit build is blocked from running on 64-bit Windows at startup
    auto sys64 = GetAppsFromUninstallKey(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall");
    auto sys32 = GetAppsFromUninstallKey(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall");

    systemApps = sys64;
    systemApps.insert(systemApps.end(), sys32.begin(), sys32.end());
    userApps[L"SYSTEM"] = systemApps;
    userSIDs[L"SYSTEM"] = L"S-1-5-18";  // Well-known SID for SYSTEM

    // Enumerate MSI-installed products (system-wide)
    systemMsiApps = EnumerateMsiProducts();
    if (!systemMsiApps.empty()) {
        userMsiApps[L"SYSTEM"] = systemMsiApps;
    }

    // Enumerate all user profiles and collect their apps
    std::vector<UserProfile> profiles = EnumerateUserProfiles();
    
    for (const auto& profile : profiles) {
        // Get Win32 apps for this user (don't shadow the outer map!)
        std::vector<InstalledApp> profileApps = GetUserInstalledApps(profile);
        
        // Get MSI products for this user
        std::vector<MsiApp> profileMsiApps = EnumerateMsiProductsForUser(profile.sid);
        
        // DEPRECATED: Registry-based AppX enumeration (limited data quality)
        // std::vector<AppXPackage> profilePackages = GetUserAppXPackages(profile.sid);
        
        // NEW: WinRT-based modern app enumeration (Windows 8+, gracefully skips on Windows 7)
        // Pass username for better logging
        std::vector<ModernAppPackage> profilePackages = GetModernAppPackagesForUser(profile.sid, profile.username);
        
        // Add to maps if we found any apps
        if (!profileApps.empty()) {
            userApps[profile.username] = profileApps;
            userSIDs[profile.username] = profile.sid;
        }
        
        if (!profileMsiApps.empty()) {
            userMsiApps[profile.username] = profileMsiApps;
        }
        
        if (!profilePackages.empty()) {
            userAppXPackages[profile.username] = profilePackages;
        }
    }

    // DEPRECATED: Registry-based system AppX enumeration
    // std::vector<AppXPackage> systemPackages = EnumerateAppXPackages();
    
    // NEW: WinRT-based system-wide modern app enumeration
    std::vector<ModernAppPackage> systemPackages = EnumerateAllModernAppPackages();
    if (!systemPackages.empty()) {
        userAppXPackages[L"SYSTEM"] = systemPackages;
    }

    svipAddresses = GetLocalIPAddresses();

    // Get ISO 8601 timestamp for collection
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    struct tm timeinfo = {};
    std::string timestamp;
    
    if (localtime_s(&timeinfo, &time) == 0)
    {
        std::ostringstream timestampStream;
        timestampStream << std::put_time(&timeinfo, "%Y-%m-%dT%H:%M:%S");
        timestamp = timestampStream.str();
    }
    else
    {
        timestamp = "UNKNOWN";
    }

    // JSON Begin
    std::ostringstream out;
    out << "{\n";
    
    // Add Spectra Machine ID as the first field
    out << "  \"spectraMachineId\": \"" << MachineId::GetMachineIdUtf8() << "\",\n";
    out << "  \"collectionTimestamp\": \"" << timestamp << "\",\n";
    out << "  \"agentVersion\": \"" << WideToUtf8(ServiceConfig::VERSION) << "\",\n";
    
    out << "  \"machineNetBiosName\": " << JsonEscape(machineNames.netbiosName) << ",\n";
    out << "  \"machineDnsName\": " << JsonEscape(machineNames.dnsName) << ",\n";
    out << "  \"ipAddresses\": [\n";
    
    for (size_t i = 0; i < svipAddresses.size(); i++) {
        out << "    \"" << svipAddresses[i] << "\"";
        if (i + 1 < svipAddresses.size()) out << ",";
        out << "\n";
    }
    
    out << "  ],\n";
    out << "  \"installedAppsByUser\": [\n";

    bool firstUser = true;
    for (auto& pair : userApps)
    {
        if (!firstUser) out << ",\n";
        firstUser = false;

        const std::wstring& username = pair.first;
        const std::wstring& userSID = userSIDs[username];

        out << "    {\n";
        out << "      \"user\": " << JsonEscape(username) << ",\n";
        out << "      \"userSID\": " << JsonEscape(userSID) << ",\n";
        out << "      \"applications\": [\n";

        const auto& apps = pair.second;
        for (size_t i = 0; i < apps.size(); i++)
        {
            out << "        {\n";
            out << "          \"displayName\": " << JsonEscape(apps[i].displayName) << ",\n";
            out << "          \"displayVersion\": " << JsonEscape(apps[i].displayVersion) << ",\n";
            out << "          \"publisher\": " << JsonEscape(apps[i].publisher) << ",\n";
            out << "          \"installLocation\": " << JsonEscape(apps[i].installLocation) << ",\n";
            out << "          \"versionMajor\": " << JsonEscape(apps[i].versionMajor) << ",\n";
            out << "          \"versionMinor\": " << JsonEscape(apps[i].versionMinor) << ",\n";
            out << "          \"installDate\": " << JsonEscape(apps[i].installDate) << ",\n";
            out << "          \"modifyPath\": " << JsonEscape(apps[i].modifyPath) << ",\n";
            out << "          \"quietUninstallString\": " << JsonEscape(apps[i].quietUninstallString) << ",\n";
            out << "          \"uninstallString\": " << JsonEscape(apps[i].uninstallString) << "\n";
            out << "        }";
            if (i + 1 < apps.size()) out << ",";
            out << "\n";
        }

        out << "      ]";
        
        // Add MSI products if available for this user
        if (userMsiApps.find(username) != userMsiApps.end()) {
            out << ",\n";
            out << "      \"msiProducts\": [\n";
            
            const auto& msiProducts = userMsiApps[username];
            for (size_t i = 0; i < msiProducts.size(); i++)
            {
                out << "        {\n";
                out << "          \"productCode\": " << JsonEscape(msiProducts[i].productCode) << ",\n";
                out << "          \"productName\": " << JsonEscape(msiProducts[i].productName) << ",\n";
                out << "          \"productVersion\": " << JsonEscape(msiProducts[i].productVersion) << ",\n";
                out << "          \"publisher\": " << JsonEscape(msiProducts[i].publisher) << ",\n";
                out << "          \"installDate\": " << JsonEscape(msiProducts[i].installDate) << ",\n";
                out << "          \"installLocation\": " << JsonEscape(msiProducts[i].installLocation) << ",\n";
                out << "          \"installSource\": " << JsonEscape(msiProducts[i].installSource) << ",\n";
                out << "          \"packageCode\": " << JsonEscape(msiProducts[i].packageCode) << ",\n";
                out << "          \"assignmentType\": " << JsonEscape(msiProducts[i].assignmentType) << ",\n";
                out << "          \"language\": " << JsonEscape(msiProducts[i].language) << "\n";
                out << "        }";
                if (i + 1 < msiProducts.size()) out << ",";
                out << "\n";
            }
            
            out << "      ]";
        }
        
        // Add Modern AppX packages if available for this user
        if (userAppXPackages.find(username) != userAppXPackages.end()) {
            out << ",\n";
            out << "      \"modernAppPackages\": [\n";
            
            const auto& packages = userAppXPackages[username];
            for (size_t i = 0; i < packages.size(); i++)
            {
                out << "        {\n";
                out << "          \"packageFullName\": " << JsonEscape(packages[i].packageFullName) << ",\n";
                out << "          \"packageFamilyName\": " << JsonEscape(packages[i].packageFamilyName) << ",\n";
                out << "          \"displayName\": " << JsonEscape(packages[i].displayName) << ",\n";
                out << "          \"publisher\": " << JsonEscape(packages[i].publisher) << ",\n";
                out << "          \"publisherId\": " << JsonEscape(packages[i].publisherId) << ",\n";
                out << "          \"publisherDisplayName\": " << JsonEscape(packages[i].publisherDisplayName) << ",\n";
                out << "          \"version\": " << JsonEscape(packages[i].version) << ",\n";
                out << "          \"architecture\": " << JsonEscape(packages[i].architecture) << ",\n";
                out << "          \"installLocation\": " << JsonEscape(packages[i].installLocation) << ",\n";
                out << "          \"resourceId\": " << JsonEscape(packages[i].resourceId) << ",\n";
                out << "          \"description\": " << JsonEscape(packages[i].description) << ",\n";
                out << "          \"logo\": " << JsonEscape(packages[i].logo) << ",\n";
                out << "          \"isFramework\": " << (packages[i].isFramework ? "true" : "false") << ",\n";
                out << "          \"isBundle\": " << (packages[i].isBundle ? "true" : "false") << ",\n";
                out << "          \"isResourcePackage\": " << (packages[i].isResourcePackage ? "true" : "false") << ",\n";
                out << "          \"isDevelopmentMode\": " << (packages[i].isDevelopmentMode ? "true" : "false") << ",\n";
                out << "          \"users\": [";
                
                // Resolve SIDs to usernames
                for (size_t j = 0; j < packages[i].users.size(); j++)
                {
                    const std::wstring& userSid = packages[i].users[j];
                    
                    // Find username for this SID by searching through userSIDs map
                    std::wstring resolvedUsername = L"Unknown";
                    for (const auto& sidPair : userSIDs)
                    {
                        if (sidPair.second == userSid)
                        {
                            resolvedUsername = sidPair.first;
                            break;
                        }
                    }
                    
                    out << "{\"sid\": " << JsonEscape(userSid) << ", \"username\": " << JsonEscape(resolvedUsername) << "}";
                    if (j + 1 < packages[i].users.size()) out << ", ";
                }
                
                out << "]\n";
                out << "        }";
                if (i + 1 < packages.size()) out << ",";
                out << "\n";
            }
            
            out << "      ]\n";
        } else {
            out << "\n";
        }

        out << "    }";
    }

    out << "\n  ]\n";
    out << "}\n";

    return out.str();
}