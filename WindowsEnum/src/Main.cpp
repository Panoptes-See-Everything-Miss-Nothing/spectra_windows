#define WIN32_LEAN_AND_MEAN
#include "WindowsEnum/WindowsEnum.h"
#include "WindowsEnum/WinAppXPackages.h"
#include "Utils/Utils.h"
#include <unordered_map>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "advapi32.lib")  // For registry and privilege APIs (lowercase)

std::string GenerateJSON()
{
    std::unordered_map<std::wstring, std::vector<InstalledApp>> userApps;
    std::unordered_map<std::wstring, std::wstring> userSIDs;  // Map username -> SID
    std::unordered_map<std::wstring, std::vector<ModernAppPackage>> userAppXPackages;  // Map username -> Modern AppX packages (WinRT)
    std::vector<InstalledApp> systemApps = {};
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

    // Enumerate all user profiles and collect their apps
    std::vector<UserProfile> profiles = EnumerateUserProfiles();
    
    for (const auto& profile : profiles) {
        // Get Win32 apps for this user (don't shadow the outer map!)
        std::vector<InstalledApp> profileApps = GetUserInstalledApps(profile);
        
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

    // JSON Begin
    std::ostringstream out;
    out << "{\n";
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

// Helper: Check if running 32-bit app on 64-bit Windows
bool IsWow64Process()
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

int main()
{
#ifndef _WIN64
    // 32-bit build: Block execution on 64-bit Windows
    if (IsWow64Process())
    {
        MessageBoxW(nullptr,
            L"This 32-bit application is not supported on 64-bit Windows.\n\n"
            L"Please use the 64-bit version of this application.",
            L"Architecture Mismatch",
            MB_OK | MB_ICONERROR);
        LogError("[-] FATAL: 32-bit application attempted to run on 64-bit Windows. Exiting.");
        return 1;  // Exit with error code
    }
#endif

    // Check Windows version - require Windows 10 or later
    OSVERSIONINFOEXW osvi = {};
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    
    typedef LONG(WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
    auto RtlGetVersion = reinterpret_cast<RtlGetVersionPtr>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    
    if (RtlGetVersion)
    {
        RtlGetVersion(reinterpret_cast<PRTL_OSVERSIONINFOW>(&osvi));
    }
    
    // Windows 10 is version 10.0
    if (osvi.dwMajorVersion < 10)
    {
        std::wstringstream msg;
        msg << L"This application requires Windows 10 or later.\n\n"
            << L"Current Windows version: " << osvi.dwMajorVersion << L"." << osvi.dwMinorVersion << L"\n\n"
            << L"Please run this on Windows 10 or Windows 11.";
        
        MessageBoxW(nullptr, msg.str().c_str(), L"Unsupported Windows Version", MB_OK | MB_ICONERROR);
        
        LogError("[-] FATAL: This application requires Windows 10 or later. Current version: " + 
                 std::to_string(osvi.dwMajorVersion) + "." + std::to_string(osvi.dwMinorVersion));
        return 1;
    }
    
    LogError("[+] Running on Windows " + std::to_string(osvi.dwMajorVersion) + "." + 
             std::to_string(osvi.dwMinorVersion) + "." + std::to_string(osvi.dwBuildNumber));

    LogError("[!] IMPORTANT: This application must run as SYSTEM account to enumerate all user profiles.");
    LogError("[!] Running as Administrator is NOT sufficient.");
    LogError("[!] Use PsExec or Task Scheduler to run as SYSTEM:");
    LogError("[!]   psexec -s -i Windows-Info-Gathering.exe");
    LogError("[!]   OR create a scheduled task with 'Run whether user is logged on or not' + 'Run with highest privileges'");
    
    std::string jsonData = GenerateJSON();
    WriteJSONToFile(jsonData); 
    return 0;
}

