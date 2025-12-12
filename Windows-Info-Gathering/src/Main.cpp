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
#include <chrono>
#include <iomanip>

#pragma comment(lib, "Ws2_32.lib")

namespace fs = std::filesystem;

// Helper function for logging errors
void LogError(const std::string& message)
{
    fs::path logPath = fs::current_path() / "spectra_log.txt";
    std::ofstream logFile(logPath, std::ios::app);

    if (logFile.is_open()) {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        struct tm timeinfo;
        localtime_s(&timeinfo, &time);
        logFile << "[" << std::put_time(&timeinfo, "%Y-%m-%d %H:%M:%S") << "] " << message << std::endl;
        logFile.close();
    }
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

// Helper: Extract IP string from addrinfo structure
std::string ExtractIPFromAddrInfo(addrinfo* p)
{
    char ipBuf[INET6_ADDRSTRLEN] = {};

    if (p->ai_family == AF_INET) {
        sockaddr_in* ipv4 = reinterpret_cast<sockaddr_in*>(p->ai_addr);
        if (inet_ntop(AF_INET, &(ipv4->sin_addr), ipBuf, sizeof(ipBuf))) {
            return ipBuf;
        }
    }
    else if (p->ai_family == AF_INET6) {
        sockaddr_in6* ipv6 = reinterpret_cast<sockaddr_in6*>(p->ai_addr);
        if (inet_ntop(AF_INET6, &(ipv6->sin6_addr), ipBuf, sizeof(ipBuf))) {
            return ipBuf;
        }
    }

    return "";
}

// Helper: Check if IP is loopback
bool IsLoopbackIP(const std::string& ip)
{
    return (ip == "127.0.0.1" || ip == "::1");
}

std::vector<std::string> GetAllIPAddresses()
{
    std::vector<std::string> svIPs;
    svIPs.reserve(7);  // Single heap allocation: 5 IPv4 + 2 IPv6
    constexpr int HOSTNAME_LEN = 256;
    constexpr int MAX_IPV4 = 5;
    constexpr int MAX_IPV6 = 2;
    int ipv4Count = 0;
    int ipv6Count = 0;
    int getaddrinfoResult = 0;
    std::string sIPAddr = "";
    WORD wVersionRequired = MAKEWORD(2, 2);
    WSADATA wsa = {};
    addrinfo* pAddrInfoList = nullptr;
    std::array<char, HOSTNAME_LEN> cHostname = {};
    addrinfo pHints = {};
    pHints.ai_family = AF_UNSPEC;
    pHints.ai_socktype = SOCK_STREAM;
    int wsaStartupResult = WSAStartup(wVersionRequired, &wsa);
        
    if (wsaStartupResult != 0) {
        LogError("[-]Error: WSAStartup() failed with error code: " + std::to_string(wsaStartupResult));
        return svIPs;
    }

    LogError("[+]Trying to get local machine's hostname.");
    if (gethostname(cHostname.data(), static_cast<int>(cHostname.size())) != 0) {
        LogError("[-]Error: gethostname() failed with error code: " + std::to_string(WSAGetLastError()));
        LogError("[+]Calling WSACleanup() to terminate the use of WinSock2 DLL.");
        WSACleanup();
        
        return svIPs;
    }
    
    LogError("[+]Trying to get all IP addresses.");
    getaddrinfoResult = getaddrinfo(cHostname.data(), nullptr, &pHints, &pAddrInfoList);

    if (getaddrinfoResult != 0) {
        LogError("[-]Error: getaddrinfo() failed with error code: " + std::to_string(getaddrinfoResult));
        
        if (pAddrInfoList != nullptr) {
            freeaddrinfo(pAddrInfoList);
        }

        LogError("[+]Calling WSACleanup() to terminate the use of WinSock2 DLL.");
        LogError("[+]Calling WSACleanup() to terminate the use of WinSock2 DLL.");
        WSACleanup();

        return svIPs;
    }

    // Traverse linked list and collect non-loopback IPs with hard limits
    for (addrinfo* p = pAddrInfoList; p != nullptr; p = p->ai_next)
    {
        // Stop if we've reached the total limit
        if (ipv4Count >= MAX_IPV4 && ipv6Count >= MAX_IPV6) {
            LogError("[+]IP collection limit reached (5 IPv4 + 2 IPv6)");
            break;
        }

        sIPAddr = ExtractIPFromAddrInfo(p);

        if (sIPAddr.empty() || IsLoopbackIP(sIPAddr)) {
            continue;
        }

        // Collect IPv4 addresses (max 5)
        if (p->ai_family == AF_INET) {
            if (ipv4Count < MAX_IPV4) {
                svIPs.push_back(sIPAddr);
                ipv4Count++;
                LogError("[+]IPv4 address collected: " + sIPAddr);
            }
        }
        // Collect IPv6 addresses (max 2)
        else if (p->ai_family == AF_INET6) {
            if (ipv6Count < MAX_IPV6) {
                svIPs.push_back(sIPAddr);
                ipv6Count++;
                LogError("[+]IPv6 address collected: " + sIPAddr);
            }
        }
    }

    // Clean up resources
    if (pAddrInfoList != nullptr) {
        LogError("[+]Calling freeaddrinfo() to free the address information that has been allocated to addrinfo structure.");
        freeaddrinfo(pAddrInfoList);
    }

    LogError("[+]Calling WSACleanup() to terminate the use of WinSock2 DLL.");
    WSACleanup();

    LogError("[+]Total IP addresses collected: " + std::to_string(svIPs.size()) + 
             " (IPv4: " + std::to_string(ipv4Count) + ", IPv6: " + std::to_string(ipv6Count) + ")");
    return svIPs;
}

std::string GenerateJSON()
{
    std::map<std::wstring, std::vector<InstalledApp>> userApps;
    std::vector<InstalledApp> systemApps = {};
    std::wstring machineName = L"";
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

    machineName = GetMachineName();
    svipAddresses = GetAllIPAddresses();

    // JSON Begin
    std::ostringstream out;
    out << "{\n";
    out << "  \"machineName\": " << JsonEscape(machineName) << ",\n";
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
