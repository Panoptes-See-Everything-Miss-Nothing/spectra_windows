#include "UserProfiles.h"
#include "RegistryUtils.h"

std::vector<UserProfile> EnumerateUserProfiles()
{
    std::vector<UserProfile> profiles;
    HKEY hProfileList = nullptr;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, 
                      L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList",
                      0, KEY_READ, &hProfileList) != ERROR_SUCCESS)
    {
        LogError("[-] Failed to open ProfileList registry key");
        return profiles;
    }

    DWORD index = 0;
    WCHAR sidBuffer[256] = {};
    DWORD sidSize = 256;
    LONG result;

    while ((result = RegEnumKeyExW(hProfileList, index, sidBuffer, &sidSize,
                                   nullptr, nullptr, nullptr, nullptr)) == ERROR_SUCCESS)
    {
        HKEY hProfileKey = nullptr;
        std::wstring sidKey = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList\\";
        sidKey += sidBuffer;

        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sidKey.c_str(), 0, KEY_READ, &hProfileKey) == ERROR_SUCCESS)
        {
            UserProfile profile;
            profile.sid = sidBuffer;
            profile.profilePath = GetRegistryString(hProfileKey, L"ProfileImagePath");

            if (!profile.profilePath.empty())
            {
                size_t lastSlash = profile.profilePath.find_last_of(L"\\");
                if (lastSlash != std::wstring::npos)
                {
                    profile.username = profile.profilePath.substr(lastSlash + 1);
                }
            }

            HKEY hTestKey = nullptr;
            std::wstring testPath = sidBuffer;
            testPath += L"\\Software";
            profile.isLoaded = (RegOpenKeyExW(HKEY_USERS, testPath.c_str(), 0, KEY_READ, &hTestKey) == ERROR_SUCCESS);
            if (hTestKey)
                RegCloseKey(hTestKey);

            if (!profile.username.empty() && !IsSystemAccount(profile.username))
            {
                profiles.push_back(profile);
                LogError("[+] Found user profile: " + WideToUtf8(profile.username) +
                        " (SID: " + WideToUtf8(profile.sid) + 
                        ", Loaded: " + (profile.isLoaded ? "Yes" : "No") + ")");
            }

            RegCloseKey(hProfileKey);
        }

        index++;
        sidSize = 256;
        ZeroMemory(sidBuffer, sizeof(sidBuffer));
    }

    if (result != ERROR_NO_MORE_ITEMS)
    {
        LogError("[-] User profile enumeration ended with error: " + std::to_string(result));
    }

    RegCloseKey(hProfileList);
    LogError("[+] Total user profiles found: " + std::to_string(profiles.size()));
    return profiles;
}
