#pragma once

// Minimize Windows API surface area
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>    // Must be before windows.h
#include <ws2tcpip.h>    // For addrinfo, inet_ntop
#include <windows.h>     // For HKEY and basic Windows types
#include <string>
#include <vector>

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

// Network functions
std::vector<std::string> GetAllIPAddresses();
std::wstring GetMachineName();

// Registry functions
std::wstring GetRegistryString(HKEY hKey, const std::wstring& valueName);
std::vector<InstalledApp> GetAppsFromUninstallKey(HKEY root, const std::wstring& subkey);

// Helper functions
bool IsLoopbackIP(const std::string& ip);
std::string ExtractIPFromAddrInfo(addrinfo* p);






