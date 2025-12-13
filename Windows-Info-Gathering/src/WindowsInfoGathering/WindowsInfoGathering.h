#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string>
#include <vector>
#include <array>

// Network functions
std::vector<std::string> GetAllIPAddresses();
std::wstring GetMachineName();

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

// Registry functions
std::wstring GetRegistryString(HKEY hKey, const std::wstring& valueName);
std::vector<InstalledApp> GetAppsFromUninstallKey(HKEY root, const std::wstring& subkey);

// Helper functions
bool IsLoopbackIP(const std::string& ip);
std::string ExtractIPFromAddrInfo(addrinfo* p);




