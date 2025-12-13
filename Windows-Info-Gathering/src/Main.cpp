#define WIN32_LEAN_AND_MEAN
#include "WindowsInfoGathering/WindowsInfoGathering.h"
#include "Utils/Utils.h"
#include <map>

#pragma comment(lib, "Ws2_32.lib")

std::string GenerateJSON()
{
    std::map<std::wstring, std::vector<InstalledApp>> userApps;
    std::vector<InstalledApp> systemApps = {};
    MachineNames machineNames = GetMachineName();
    std::vector<std::string> svipAddresses = {};

    // Get system wide installed applications from x64 and WOW6432 registry keys
    auto sys64 = GetAppsFromUninstallKey(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall");
    auto sys32 = GetAppsFromUninstallKey(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall");

    systemApps = sys64;
    systemApps.insert(systemApps.end(), sys32.begin(), sys32.end());
    userApps[L"SYSTEM"] = systemApps;

    // Get a list of applications installed for a particular user.
    for (auto& entry : fs::directory_iterator(L"C:\\Users"))
    {
        if (!entry.is_directory()) continue;

        std::wstring username = entry.path().filename().wstring();
        if (username == L"Public" || username == L"Default" || username == L"All Users")
            continue;

        auto apps = GetAppsFromUninstallKey(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall");

        if (!apps.empty())
            userApps[username] = apps;
    }

    svipAddresses = GetAllIPAddresses();

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

        out << "    {\n";
        out << "      \"user\": " << JsonEscape(pair.first) << ",\n";
        out << "      \"userSID\": " << JsonEscape(pair.first) << ",\n"; // Placeholder
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

        out << "      ]\n";
        out << "    }";
    }

    out << "\n  ]\n";
    out << "}\n";

    return out.str();
}

int main()
{
    std::string jsonData = GenerateJSON();
    WriteJSONToFile(jsonData); 
    return 0;
}
