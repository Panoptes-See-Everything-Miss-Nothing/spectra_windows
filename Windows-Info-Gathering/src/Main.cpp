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
#include <array>
#include <cstring>

#pragma comment(lib, "Ws2_32.lib")

namespace fs = std::filesystem;

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

// Read REG_SZ safely
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

// Installed app structure
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

std::wstring GetMachineName()
{
    WCHAR buf[256];
    DWORD size = 256;
    if (GetComputerNameW(buf, &size))
        return buf;
    return L"";
}

std::vector<std::string> GetIPAddresses()
{
    // TO Do - 
    // 1. Change this to standard fixed size array for optimization
    // 2. Why IP addresses are blank in JSON??? Bug fix!!
    std::vector<std::string> ips;   
    std::array<char, 256> hostname{};
    const char* ipv4LoopbackAddr = "127.0.0.1";
    const char* ipv6LoopbackAddr = "::1";

    WSADATA wsa{};  // Initialize WinSock
    WORD wVersionRequired = MAKEWORD(2, 2);
    
    if (WSAStartup(wVersionRequired, &wsa) != 0) {
        return ips;  // return empty
    }

    // Type cast hostname.size() to avoid data loss warning for 64-bit Windows machines.
    // Because hostname.size() returns a size_t, which is 64-bit but gethostname() expects the second argument to be an int(int namelen).  
    if (gethostname(hostname.data(), static_cast<int>(hostname.size()) != 0)) {
        WSACleanup();
        return ips;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;      // Resolve IPv4 + IPv6 address family
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* info = nullptr;
    if (getaddrinfo(hostname.data(), nullptr, &hints, &info) != 0) {
        WSACleanup();
        return ips;
    }

    // Iterate network interfaces
    for (addrinfo* p = info; p != nullptr; p = p->ai_next)
    {
        char ipBuf[INET6_ADDRSTRLEN] = {};

        if (p->ai_family == AF_INET) {
            sockaddr_in* ipv4 = reinterpret_cast<sockaddr_in*>(p->ai_addr);
            if (!inet_ntop(AF_INET, &(ipv4->sin_addr), ipBuf, sizeof(ipBuf))) {
                continue; // skip bad entries
            }
            if (strcmp(ipBuf, ipv4LoopbackAddr) == 0) {
                continue; // skip IPv4 loopback
            }
        }
        else if (p->ai_family == AF_INET6) {
            sockaddr_in6* ipv6 = reinterpret_cast<sockaddr_in6*>(p->ai_addr);
            if (!inet_ntop(AF_INET6, &(ipv6->sin6_addr), ipBuf, sizeof(ipBuf))) {
                continue; // skip bad entries safely
            }
            if (strcmp(ipBuf, ipv6LoopbackAddr) == 0) {
                continue; // skip IPv6 loopback
            }
        }
        else {
            continue; // skip unsupported families
        }

        ips.emplace_back(ipBuf);
    }

    freeaddrinfo(info);
    WSACleanup();

    return ips;
}


std::string GenerateJSON()
{
    std::map<std::wstring, std::vector<InstalledApp>> userApps;

    // Get system wide installed applications from x64 and WOW6432 registry keys
    auto sys64 = GetAppsFromUninstallKey(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall");
    auto sys32 = GetAppsFromUninstallKey(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall");

    std::vector<InstalledApp> systemApps = sys64;
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

int main()
{
    std::string jsonData = GenerateJSON();
    WriteJSONToFile(jsonData); 
    return 0;
}
