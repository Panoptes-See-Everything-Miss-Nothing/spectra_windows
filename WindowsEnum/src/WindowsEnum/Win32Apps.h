#pragma once

#include "UserProfiles.h"
#include "../Utils/Utils.h"

// Installed application information
struct InstalledApp
{
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

// Get apps from a specific registry uninstall key
std::vector<InstalledApp> GetAppsFromUninstallKey(HKEY root, const std::wstring& subkey);

// Get installed apps for a specific user (loads registry hive if needed)
std::vector<InstalledApp> GetUserInstalledApps(const UserProfile& userProfile);
