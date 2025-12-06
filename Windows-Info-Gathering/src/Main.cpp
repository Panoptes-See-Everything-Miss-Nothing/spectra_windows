#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <sddl.h>
#include <shlobj.h>

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include <sstream>
#include <fstream>

#pragma comment(lib, "Ws2_32.lib")

namespace fs = std::filesystem;

//----------------------------------------------------------
// UTF-8 JSON ESCAPE
//----------------------------------------------------------
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

//----------------------------------------------------------
// Read REG_SZ safely
//----------------------------------------------------------
std::wstring GetRegistryString(HKEY hKey, const std::wstring& valueName)
{
    DWORD type = 0;
    DWORD size = 0;

    if (RegQueryValueExW(hKey, valueName.c_str(), nullptr, &type, nullptr, &size) != ERROR_SUCCESS)
        return L"";

    if (type != REG_SZ && type != REG_EXPAND_SZ)
        return L"";

    std::wstring data(size / sizeof(wchar_t), L'\0');

    if (RegQueryValueExW(hKey, valueName.c_str(), nullptr, nullptr, (LPBYTE)data.data(), &size) != ERROR_SUCCESS)
        return L"";

    if (!data.empty() && data.back() == L'\0')
        data.pop_back();

    return data;
}

//----------------------------------------------------------
// Installed app structure
//----------------------------------------------------------
struct InstalledApp {
    std::wstring displayName;
    std::wstring displayVersion;
    std::wstring publisher;
    std::wstring installLocation;
    std::wstring versionMajor;
    std::wstring versionMinor;
    std::wstring installDate;
    std::wstring modifyPath;
    std::wstring quietUninstallString;
    std::wstring uninstallString;
};

//----------------------------------------------------------
// Read uninstall keys under any registry hive
//----------------------------------------------------------
std::vector<InstalledApp> GetAppsFromUninstallKey(HKEY root, const std::wstring& subkey)
{
    std::vector<InstalledApp> apps;

    HKEY hKey;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return apps;

    DWORD index = 0;
    WCHAR name[256];
    DWORD nameSize = 256;

    while (RegEnumKeyExW(hKey, index, name, &nameSize, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
    {
        HKEY hSub;
        if (RegOpenKeyExW(hKey, name, 0, KEY_READ, &hSub) == ERROR_SUCCESS)
        {
            InstalledApp app;
            app.displayName = GetRegistryString(hSub, L"DisplayName");
            app.displayVersion = GetRegistryString(hSub, L"DisplayVersion");
            app.publisher = GetRegistryString(hSub, L"Publisher");
            app.installLocation = GetRegistryString(hSub, L"InstallLocation");
            app.uninstallString = GetRegistryString(hSub, L"UninstallString");
            app.versionMajor = L"";
            app.versionMinor = L"";
            app.installDate = GetRegistryString(hSub, L"InstallDate");
            app.modifyPath = L"";
            app.quietUninstallString = L"";

            if (!app.displayName.empty())
                apps.push_back(app);

            RegCloseKey(hSub);
        }
        index++;
        nameSize = 256;
    }

    RegCloseKey(hKey);
    return apps;
}

//----------------------------------------------------------
// Get machine name
//----------------------------------------------------------
std::wstring GetMachineName()
{
    WCHAR buf[256];
    DWORD size = 256;
    if (GetComputerNameW(buf, &size))
        return buf;
    return L"";
}

//----------------------------------------------------------
// Get IP addresses
//----------------------------------------------------------
std::vector<std::string> GetIPAddresses()
{
    std::vector<std::string> ips;

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    char hostname[256];
    gethostname(hostname, sizeof(hostname));

    addrinfo hints{}, * info = nullptr;
    hints.ai_family = AF_INET;

    if (getaddrinfo(hostname, nullptr, &hints, &info) == 0)
    {
        for (auto p = info; p != nullptr; p = p->ai_next)
        {
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &((sockaddr_in*)p->ai_addr)->sin_addr, ip, sizeof(ip));
            ips.push_back(ip);
        }
    }

    if (info) freeaddrinfo(info);

    WSACleanup();
    return ips;
}

//----------------------------------------------------------
// Generate JSON inventory
//----------------------------------------------------------
std::string GenerateJSON()
{
    std::map<std::wstring, std::vector<InstalledApp>> userApps;

    // SYSTEM apps
    auto sys64 = GetAppsFromUninstallKey(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall");
    auto sys32 = GetAppsFromUninstallKey(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall");

    std::vector<InstalledApp> systemApps = sys64;
    systemApps.insert(systemApps.end(), sys32.begin(), sys32.end());
    userApps[L"SYSTEM"] = systemApps;

    // Per-user apps
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

    // Machine info
    std::wstring machineName = GetMachineName();
    auto ips = GetIPAddresses();

    // JSON Begin
    std::ostringstream out;
    out << "{\n";
    out << "  \"machineName\": " << JsonEscape(machineName) << ",\n";
    out << "  \"ipAddresses\": [\n";
    for (size_t i = 0; i < ips.size(); i++) {
        out << "    \"" << ips[i] << "\"";
        if (i + 1 < ips.size()) out << ",";
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

//----------------------------------------------------------
// Write JSON to file
//----------------------------------------------------------
void WriteJSONToFile(const std::string& jsonData, const std::wstring& filename = L"inventory.json")
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

//----------------------------------------------------------
// MAIN
//----------------------------------------------------------
int main()
{
    std::string jsonData = GenerateJSON();
    WriteJSONToFile(jsonData); // Writes to "inventory.json" in current directory
    return 0;
}
