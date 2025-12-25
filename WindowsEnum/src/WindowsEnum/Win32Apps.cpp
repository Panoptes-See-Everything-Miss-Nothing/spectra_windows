#include "Win32Apps.h"
#include "RegistryUtils.h"
#include "PrivMgmt.h"
#include "../Utils/Utils.h"

std::vector<InstalledApp> GetAppsFromUninstallKey(HKEY root, const std::wstring& subkey)
{
    std::vector<InstalledApp> apps;
    HKEY hKey;
    DWORD index = 0;
    constexpr DWORD MAX_KEY_LENGTH = 256;
    WCHAR name[MAX_KEY_LENGTH];
    DWORD nameSize = MAX_KEY_LENGTH;
    LONG result;

    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return apps;

    while ((result = RegEnumKeyExW(hKey, index, name, &nameSize, 
                                   nullptr, nullptr, nullptr, nullptr)) == ERROR_SUCCESS)
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
        nameSize = MAX_KEY_LENGTH;
    }

    if (result != ERROR_NO_MORE_ITEMS) {
        LogError("[-] Registry enumeration ended with error: " + std::to_string(result));
    }

    RegCloseKey(hKey);
    return apps;
}

std::vector<InstalledApp> GetUserInstalledApps(const UserProfile& userProfile)
{
    std::vector<InstalledApp> apps;
    bool hiveLoadedByUs = false;
    std::wstring hiveKeyName = L"TempHive_" + userProfile.username;
    std::wstring ntUserPath = userProfile.profilePath + L"\\NTUSER.DAT";

    if (!userProfile.isLoaded)
    {
        if (!EnablePrivilege(SE_RESTORE_NAME) || !EnablePrivilege(SE_BACKUP_NAME))
        {
            LogError("[-] Failed to enable privileges for loading user hive: " + WideToUtf8(userProfile.username));
            return apps;
        }

        LONG loadResult = RegLoadKeyW(HKEY_USERS, hiveKeyName.c_str(), ntUserPath.c_str());
        if (loadResult != ERROR_SUCCESS)
        {
            LogError("[-] Failed to load registry hive for user '" + WideToUtf8(userProfile.username) +
                    "', error: " + std::to_string(loadResult));
            return apps;
        }

        hiveLoadedByUs = true;
        LogError("[+] Loaded registry hive for user: " + WideToUtf8(userProfile.username));
    }

    std::wstring registryPath;
    if (hiveLoadedByUs)
    {
        registryPath = hiveKeyName + L"\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    }
    else
    {
        registryPath = userProfile.sid + L"\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    }

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_USERS, registryPath.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD index = 0;
        WCHAR name[256] = {};
        DWORD nameSize = 256;
        LONG result;

        while ((result = RegEnumKeyExW(hKey, index, name, &nameSize,
                                       nullptr, nullptr, nullptr, nullptr)) == ERROR_SUCCESS)
        {
            HKEY hAppKey = nullptr;
            std::wstring appKeyPath = registryPath + L"\\" + name;

            if (RegOpenKeyExW(HKEY_USERS, appKeyPath.c_str(), 0, KEY_READ, &hAppKey) == ERROR_SUCCESS)
            {
                InstalledApp app;
                app.displayName = GetRegistryString(hAppKey, L"DisplayName");
                app.displayVersion = GetRegistryString(hAppKey, L"DisplayVersion");
                app.publisher = GetRegistryString(hAppKey, L"Publisher");
                app.installLocation = GetRegistryString(hAppKey, L"InstallLocation");
                app.uninstallString = GetRegistryString(hAppKey, L"UninstallString");
                app.installDate = GetRegistryString(hAppKey, L"InstallDate");

                if (!app.displayName.empty())
                    apps.push_back(app);

                RegCloseKey(hAppKey);
            }

            index++;
            nameSize = 256;
            ZeroMemory(name, sizeof(name));
        }

        RegCloseKey(hKey);
    }

    if (hiveLoadedByUs)
    {
        LONG unloadResult = RegUnLoadKeyW(HKEY_USERS, hiveKeyName.c_str());

        if (unloadResult != ERROR_SUCCESS)
        {
            LogError("[-] Warning: Failed to unload registry hive for user '" + WideToUtf8(userProfile.username) +
                    "', error: " + std::to_string(unloadResult));
        }
        else
        {
            LogError("[+] Unloaded registry hive for user: " + WideToUtf8(userProfile.username));
        }

        DisablePrivilege(SE_RESTORE_NAME);
        DisablePrivilege(SE_BACKUP_NAME);
    }

    LogError("[+] Found " + std::to_string(apps.size()) + " apps for user: " + WideToUtf8(userProfile.username));

    return apps;
}
