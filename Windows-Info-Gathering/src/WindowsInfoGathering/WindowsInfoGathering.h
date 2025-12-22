#pragma once

// Minimize Windows API surface area
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>    // Must be before windows.h
#include <ws2tcpip.h>    // For addrinfo, inet_ntop
#include "../Utils/Utils.h"

// Link required libraries
#pragma comment(lib, "Advapi32.lib")  // For registry and privilege APIs

// Machine name structure
struct MachineNames {
    std::wstring netbiosName;  // NetBIOS name (short name, e.g., "DESKTOP-ABC123")
    std::wstring dnsName;      // DNS/FQDN name (e.g., "desktop-abc123.example.com")
};

// Installed app structure (Win32 apps)
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

// AppX/MSIX package structure
struct AppXPackage {
    std::wstring packageFullName;      // Full package identity name
    std::wstring packageFamilyName;    // Package family name
    std::wstring displayName;          // User-friendly name
    std::wstring publisher;            // Publisher name
    std::wstring version;              // Package version
    std::wstring installLocation;      // Installation path
    std::wstring architecture;         // x86, x64, ARM, etc.
    bool isFramework;                  // Is this a framework package?
};

// User profile information
struct UserProfile {
    std::wstring username;             // User account name
    std::wstring profilePath;          // Profile directory path
    std::wstring sid;                  // Security identifier
    bool isLoaded;                     // Is registry hive currently loaded?
};

// Network functions
std::vector<std::string> GetLocalIPAddresses();
MachineNames GetMachineName();

// Registry functions
std::wstring GetRegistryString(HKEY hKey, const std::wstring& valueName);
std::vector<InstalledApp> GetAppsFromUninstallKey(HKEY root, const std::wstring& subkey);

// User enumeration functions
std::vector<UserProfile> EnumerateUserProfiles();
std::vector<InstalledApp> GetUserInstalledApps(const UserProfile& userProfile);

// AppX/MSIX package functions
std::vector<AppXPackage> EnumerateAppXPackages();
std::vector<AppXPackage> GetUserAppXPackages(const std::wstring& userSid);

// Privilege management functions
bool EnablePrivilege(const std::wstring& privilegeName);
bool DisablePrivilege(const std::wstring& privilegeName);

// Helper functions
bool IsLoopbackIP(const std::string& ip);
std::string ExtractIPFromAddrInfo(addrinfo* p);
bool IsSystemAccount(const std::wstring& username);







