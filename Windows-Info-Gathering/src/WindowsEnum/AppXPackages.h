#pragma once

#include <string>
#include <vector>
#include <windows.h>

// AppX/MSIX package information
struct AppXPackage
{
    std::wstring packageFullName;
    std::wstring packageFamilyName;
    std::wstring displayName;
    std::wstring publisher;
    std::wstring version;
    std::wstring architecture;
    std::wstring installLocation;
    bool isFramework;
};

// Enumerate all AppX packages installed system-wide
std::vector<AppXPackage> EnumerateAppXPackages();

// Get AppX packages for a specific user
std::vector<AppXPackage> GetUserAppXPackages(const std::wstring& userSid);

// Helper: Parse package full name into components (internal use)
void ParsePackageFullName(const std::wstring& fullName, AppXPackage& package);
