#include "AppXPackages.h"
#include "RegistryUtils.h"
#include "../Utils/Utils.h"

void ParsePackageFullName(const std::wstring& fullName, AppXPackage& package)
{
    size_t pos = 0;
    size_t nextUnderscore = fullName.find(L'_', pos);
    
    if (nextUnderscore != std::wstring::npos)
    {
        std::wstring name = fullName.substr(pos, nextUnderscore - pos);
        package.displayName = name;
        
        pos = nextUnderscore + 1;
        nextUnderscore = fullName.find(L'_', pos);
        
        if (nextUnderscore != std::wstring::npos)
        {
            package.version = fullName.substr(pos, nextUnderscore - pos);
            
            pos = nextUnderscore + 1;
            nextUnderscore = fullName.find(L'_', pos);
            
            if (nextUnderscore != std::wstring::npos)
            {
                package.architecture = fullName.substr(pos, nextUnderscore - pos);
            }
        }
    }
}

std::vector<AppXPackage> EnumerateAppXPackages()
{
    std::vector<AppXPackage> packages;
    HKEY hPackages = nullptr;

    const wchar_t* packageRepoPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Appx\\AppxAllUserStore\\Applications";
    
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, packageRepoPath, 0, KEY_READ, &hPackages) != ERROR_SUCCESS)
    {
        LogError("[-] Failed to open AppX package repository");
        return packages;
    }

    DWORD index = 0;
    WCHAR packageFullName[512] = {};
    DWORD nameSize = 512;
    LONG result;

    while ((result = RegEnumKeyExW(hPackages, index, packageFullName, &nameSize,
                                   nullptr, nullptr, nullptr, nullptr)) == ERROR_SUCCESS)
    {
        AppXPackage package;
        package.packageFullName = packageFullName;
        
        ParsePackageFullName(packageFullName, package);
        
        HKEY hPackageKey = nullptr;
        if (RegOpenKeyExW(hPackages, packageFullName, 0, KEY_READ, &hPackageKey) == ERROR_SUCCESS)
        {
            package.installLocation = GetRegistryString(hPackageKey, L"Path");
            
            std::wstring isFramework = GetRegistryString(hPackageKey, L"IsFramework");
            package.isFramework = (isFramework == L"1");
            
            RegCloseKey(hPackageKey);
        }

        size_t lastUnderscore = package.packageFullName.find_last_of(L'_');
        size_t secondLastUnderscore = package.packageFullName.find_last_of(L'_', lastUnderscore - 1);
        if (secondLastUnderscore != std::wstring::npos)
        {
            package.packageFamilyName = package.packageFullName.substr(0, secondLastUnderscore) +
                                       package.packageFullName.substr(lastUnderscore);
        }

        if (!package.displayName.empty())
        {
            packages.push_back(package);
        }

        index++;
        nameSize = 512;
        ZeroMemory(packageFullName, sizeof(packageFullName));
    }

    if (result != ERROR_NO_MORE_ITEMS)
    {
        LogError("[-] AppX package enumeration ended with error: " + std::to_string(result));
    }

    RegCloseKey(hPackages);
    LogError("[+] Total AppX packages found: " + std::to_string(packages.size()));
    return packages;
}

std::vector<AppXPackage> GetUserAppXPackages(const std::wstring& userSid)
{
    std::vector<AppXPackage> packages;
    HKEY hUserPackages = nullptr;

    std::wstring userPackagePath = L"SOFTWARE\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Packages";
    std::wstring fullPath = userSid + L"\\" + userPackagePath;

    if (RegOpenKeyExW(HKEY_USERS, fullPath.c_str(), 0, KEY_READ, &hUserPackages) != ERROR_SUCCESS)
    {
        return packages;
    }

    DWORD index = 0;
    WCHAR packageName[512] = {};
    DWORD nameSize = 512;
    LONG result;

    while ((result = RegEnumKeyExW(hUserPackages, index, packageName, &nameSize,
                                   nullptr, nullptr, nullptr, nullptr)) == ERROR_SUCCESS)
    {
        AppXPackage package;
        package.packageFullName = packageName;
        
        ParsePackageFullName(packageName, package);

        HKEY hPackageKey = nullptr;
        std::wstring packageKeyPath = fullPath + L"\\" + packageName;
        
        if (RegOpenKeyExW(HKEY_USERS, packageKeyPath.c_str(), 0, KEY_READ, &hPackageKey) == ERROR_SUCCESS)
        {
            package.installLocation = GetRegistryString(hPackageKey, L"PackageRootFolder");
            package.publisher = GetRegistryString(hPackageKey, L"Publisher");
            
            RegCloseKey(hPackageKey);
        }

        if (!package.displayName.empty())
        {
            packages.push_back(package);
        }

        index++;
        nameSize = 512;
        ZeroMemory(packageName, sizeof(packageName));
    }

    RegCloseKey(hUserPackages);
    
    if (!packages.empty())
    {
        LogError("[+] Found " + std::to_string(packages.size()) + " user AppX packages for SID: " + WideToUtf8(userSid));
    }

    return packages;
}
