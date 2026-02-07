#include "Win32Apps.h"
#include "RegistryUtils.h"
#include "PrivMgmt.h"
#include "VSSSnapshot.h"
#include "RegistryHiveLoader.h"
#include "./Utils/Utils.h"
#include "./Service/ServiceConfig.h"
#include <shlobj.h>
#include <sstream>
#include <iomanip>
#include <array>

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

// Helper: Enumerate installed apps from an open uninstall registry key.
// Shared by both the direct HKEY_USERS path (loaded hives) and the
// VSS snapshot + RegLoadKeyW path (unloaded hives).
static std::vector<InstalledApp> EnumerateAppsFromUninstallKey(HKEY hUninstallKey)
{
    std::vector<InstalledApp> apps;

    DWORD index = 0;
    WCHAR name[256] = {};
    DWORD nameSize = 256;
    LONG enumResult;

    while ((enumResult = RegEnumKeyExW(hUninstallKey, index, name, &nameSize,
                                        nullptr, nullptr, nullptr, nullptr)) == ERROR_SUCCESS)
    {
        HKEY hAppKey = nullptr;

        if (RegOpenKeyExW(hUninstallKey, name, 0, KEY_READ, &hAppKey) == ERROR_SUCCESS)
        {
            InstalledApp app;
            app.displayName = GetRegistryString(hAppKey, L"DisplayName");
            app.displayVersion = GetRegistryString(hAppKey, L"DisplayVersion");
            app.publisher = GetRegistryString(hAppKey, L"Publisher");
            app.installLocation = GetRegistryString(hAppKey, L"InstallLocation");
            app.uninstallString = GetRegistryString(hAppKey, L"UninstallString");
            app.installDate = GetRegistryString(hAppKey, L"InstallDate");
            app.versionMajor = L"";
            app.versionMinor = L"";
            app.modifyPath = L"";
            app.quietUninstallString = L"";

            if (!app.displayName.empty())
                apps.push_back(app);

            RegCloseKey(hAppKey);
        }

        index++;
        nameSize = 256;
        ZeroMemory(name, sizeof(name));
    }

    return apps;
}

// Fast path: Read installed apps directly from HKEY_USERS\<SID> for users
// whose registry hive is already loaded (i.e., currently logged in).
// This avoids COM initialization, VSS snapshot creation, and temp file I/O.
static std::vector<InstalledApp> GetUserInstalledAppsFromLoadedHive(const UserProfile& userProfile)
{
    constexpr auto uninstallKeyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

    std::wstring registryPath = userProfile.sid + L"\\" + uninstallKeyPath;

    LogError("[+] Reading installed apps directly from loaded hive for user: " + WideToUtf8(userProfile.username) +
             " (SID: " + WideToUtf8(userProfile.sid) + ")");

    HKEY hKey = nullptr;
    LONG result = RegOpenKeyExW(HKEY_USERS, registryPath.c_str(), 0, KEY_READ, &hKey);
    if (result != ERROR_SUCCESS)
    {
        LogError("[-] Failed to open uninstall key in loaded hive for user '" + WideToUtf8(userProfile.username) +
                 "', error: " + std::to_string(result) + " - " + GetWindowsErrorMessage(result));
        return {};
    }

    std::vector<InstalledApp> apps = EnumerateAppsFromUninstallKey(hKey);
    RegCloseKey(hKey);

    LogError("[+] Found " + std::to_string(apps.size()) + " apps for user: " + WideToUtf8(userProfile.username) + " (direct read)");
    return apps;
}

// Slow path: Use VSS snapshot to copy NTUSER.DAT and load it via RegLoadKeyW
// for users whose registry hive is NOT loaded (not currently logged in).
static std::vector<InstalledApp> GetUserInstalledAppsFromSnapshot(const UserProfile& userProfile)
{
    std::vector<InstalledApp> apps;
    constexpr auto ntUserDatFilename = L"NTUSER.DAT";
    constexpr auto spectraVMHivePrefix = L"SpectraVM_Hive_";
    constexpr auto uninstallKeyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

    LogError("[+] Enumerating apps for user '" + WideToUtf8(userProfile.username) + "' using VSS snapshot approach.");

    // Determine the volume containing the user profile
    std::wstring profilePath = userProfile.profilePath;
    std::wstring volumePath;

    // Extract volume (e.g., "C:\\" from "C:\\Users\\username")
    if (profilePath.length() >= 3 && profilePath[1] == L':')
    {
        volumePath = profilePath.substr(0, 3); // e.g., "C:\\"
    }
    else
    {
        LogError("[-] Invalid profile path format: " + WideToUtf8(profilePath));
        return apps;
    }

    // Create VSS snapshot
    VSSSnapshot snapshot;
    if (!snapshot.CreateSnapshot(volumePath))
    {
        LogError("[-] Failed to create VSS snapshot for volume: " + WideToUtf8(volumePath));
        return apps;
    }

    // Create secure temporary directory under the service's Temp directory.
    // This directory has the service SID Modify ACE applied at install time,
    // which is required for the write-restricted SERVICE_SID_TYPE_RESTRICTED token.
    std::wstring tempBasePath = std::wstring(ServiceConfig::TEMP_DIRECTORY) + L"\\SpectraVM";

    // Ensure parent Temp directory exists (handles console mode where /install was never run)
    DWORD tempDirAttribs = GetFileAttributesW(ServiceConfig::TEMP_DIRECTORY);
    if (tempDirAttribs == INVALID_FILE_ATTRIBUTES)
    {
        int createResult = SHCreateDirectoryExW(nullptr, ServiceConfig::TEMP_DIRECTORY, nullptr);
        if (createResult != ERROR_SUCCESS && createResult != ERROR_ALREADY_EXISTS)
        {
            LogError("[-] Failed to create temp parent directory: " + WideToUtf8(ServiceConfig::TEMP_DIRECTORY) +
                     ", error: " + std::to_string(createResult));
            return apps;
        }
    }

    SecureTempDirectory tempDir(tempBasePath);
    if (!tempDir.IsValid())
    {
        LogError("[-] Failed to create secure temporary directory");
        return apps;
    }

    // Access VSS snapshots directly via their device path to
    // copy NTUSER.DAT and transaction logs directly from the snapshot
    std::wstring relativeProfilePath = profilePath.substr(3); // Remove "C:\" to get relative path
    std::wstring snapshotProfilePath = snapshot.GetSnapshotPath() + L"\\" + relativeProfilePath;
    std::wstring snapshotNtUserPath = snapshotProfilePath + L"\\" + ntUserDatFilename;
    std::wstring tempNtUserPath = tempDir.GetPath() + L"\\" + ntUserDatFilename;

    if (!CopyFileW(snapshotNtUserPath.c_str(), tempNtUserPath.c_str(), FALSE))
    {
        DWORD error = GetLastError();
        LogError("[-] Failed to copy NTUSER.DAT from snapshot for user: " + WideToUtf8(userProfile.username) +
                 ", error: " + std::to_string(error) + " - " + GetWindowsErrorMessage(error));
        return apps;
    }

    LogError("[+] Copied NTUSER.DAT from VSS snapshot for user: " + WideToUtf8(userProfile.username));

    // Copy transaction log files if they exist, otherwise the hive may be unreadable.
    constexpr std::array<std::wstring_view, 3> logFiles = {
        L"NTUSER.DAT.LOG",
        L"NTUSER.DAT.LOG1",
        L"NTUSER.DAT.LOG2"
    };

    for (const auto& logFile : logFiles)
    {
        std::wstring sourceLog = snapshotProfilePath + L"\\" + std::wstring(logFile);
        std::wstring destLog = tempDir.GetPath() + L"\\" + std::wstring(logFile);

        if (CopyFileW(sourceLog.c_str(), destLog.c_str(), FALSE))
        {
            LogError("[+] Copied " + WideToUtf8(std::wstring(logFile)) + " for user: " + WideToUtf8(userProfile.username));
        }
        else
        {
            DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND)
            {
                LogError("[-] Warning: Failed to copy " + WideToUtf8(std::wstring(logFile)) + ", error: " + std::to_string(error));
            }
        }
    }

    // Remove read-only attribute from copied files since VSS copies preserve attributes
    DWORD attributes = GetFileAttributesW(tempNtUserPath.c_str());

    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY))
    {
        SetFileAttributesW(tempNtUserPath.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
        LogError("[+] Removed read-only attribute from NTUSER.DAT");
    }

    // Load the hive using RAII
    std::wstring hiveKeyName = spectraVMHivePrefix + userProfile.username;
    RegistryHiveLoader hiveLoader(tempNtUserPath, hiveKeyName);

    if (!hiveLoader.IsLoaded())
    {
        LogError("[-] Failed to load registry hive for user: " + WideToUtf8(userProfile.username));
        return apps;
    }

    // Enumerate installed apps from the loaded hive
    std::wstring registryPath = hiveKeyName + L"\\" + uninstallKeyPath;

    HKEY hKey = nullptr;
    LONG result = RegOpenKeyExW(HKEY_USERS, registryPath.c_str(), 0, KEY_READ, &hKey);
    if (result != ERROR_SUCCESS)
    {
        LogError("[-] Failed to open registry key in loaded hive for user '" + WideToUtf8(userProfile.username) +
                 "', error: " + std::to_string(result) + " - " + GetWindowsErrorMessage(result));
    }
    else
    {
        apps = EnumerateAppsFromUninstallKey(hKey);
        RegCloseKey(hKey);
    }

    // Cleanup (RAII handles hive unloading and temp directory removal)
    LogError("[+] Found " + std::to_string(apps.size()) + " apps for user: " + WideToUtf8(userProfile.username) + " (VSS snapshot)");

    return apps;
}

std::vector<InstalledApp> GetUserInstalledApps(const UserProfile& userProfile)
{
    // Fast path: If the user's registry hive is already loaded (user is logged in),
    // read directly from HKEY_USERS\<SID>. This avoids COM initialization, VSS
    // snapshot creation, temp file I/O, and the associated restricted token issues.
    if (userProfile.isLoaded)
    {
        return GetUserInstalledAppsFromLoadedHive(userProfile);
    }

    // Slow path: User is not logged in, hive is not mounted.
    // Use VSS snapshot to safely copy NTUSER.DAT and load it via RegLoadKeyW.
    return GetUserInstalledAppsFromSnapshot(userProfile);
}
