#define WIN32_LEAN_AND_MEAN
#include "WindowsInfoGathering/WindowsInfoGathering.h"
#include "Utils/Utils.h"
#include <map>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "advapi32.lib")  // For registry and privilege APIs (lowercase)

std::string GenerateJSON()
{
    std::map<std::wstring, std::vector<InstalledApp>> userApps;
    std::map<std::wstring, std::wstring> userSIDs;  // Map username -> SID
    std::map<std::wstring, std::vector<AppXPackage>> userAppXPackages;  // Map username -> AppX packages
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
        
        // Get AppX packages for this user
        std::vector<AppXPackage> profilePackages = GetUserAppXPackages(profile.sid);
        
        // Add to maps if we found any apps
        if (!profileApps.empty()) {
            userApps[profile.username] = profileApps;
            userSIDs[profile.username] = profile.sid;
        }
        
        if (!profilePackages.empty()) {
            userAppXPackages[profile.username] = profilePackages;
        }
    }

    // Get system-wide AppX packages
    std::vector<AppXPackage> systemPackages = EnumerateAppXPackages();
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
        
        // Add AppX packages if available for this user
        if (userAppXPackages.find(username) != userAppXPackages.end()) {
            out << ",\n";
            out << "      \"appxPackages\": [\n";
            
            const auto& packages = userAppXPackages[username];
            for (size_t i = 0; i < packages.size(); i++)
            {
                out << "        {\n";
                out << "          \"packageFullName\": " << JsonEscape(packages[i].packageFullName) << ",\n";
                out << "          \"packageFamilyName\": " << JsonEscape(packages[i].packageFamilyName) << ",\n";
                out << "          \"displayName\": " << JsonEscape(packages[i].displayName) << ",\n";
                out << "          \"publisher\": " << JsonEscape(packages[i].publisher) << ",\n";
                out << "          \"version\": " << JsonEscape(packages[i].version) << ",\n";
                out << "          \"architecture\": " << JsonEscape(packages[i].architecture) << ",\n";
                out << "          \"installLocation\": " << JsonEscape(packages[i].installLocation) << ",\n";
                out << "          \"isFramework\": " << (packages[i].isFramework ? "true" : "false") << "\n";
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

    LogError("[!] IMPORTANT: This application must run as SYSTEM account to enumerate all user profiles.");
    LogError("[!] Running as Administrator is NOT sufficient.");
    LogError("[!] Use PsExec or Task Scheduler to run as SYSTEM:");
    LogError("[!]   psexec -s -i Windows-Info-Gathering.exe");
    LogError("[!]   OR create a scheduled task with 'Run whether user is logged on or not' + 'Run with highest privileges'");
    
    std::string jsonData = GenerateJSON();
    WriteJSONToFile(jsonData); 
    return 0;
}

