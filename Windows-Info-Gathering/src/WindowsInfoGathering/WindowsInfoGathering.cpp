#include "WindowsInfoGathering.h"

MachineNames GetMachineName()
{
    MachineNames machineNames;
    DWORD dwError = {};
    int sizeNeeded = 0;
    WCHAR netbiosBuffer[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD netbiosSize = MAX_COMPUTERNAME_LENGTH + 1;  // Size INCLUDING null terminator
    constexpr DWORD DNS_BUFFER_SIZE = 256;  // DNS_MAX_NAME_BUFFER_LENGTH included null terminator already
    WCHAR dnsBuffer[DNS_BUFFER_SIZE] = {};
    DWORD dnsSize = DNS_BUFFER_SIZE;  //  Match buffer size exactly
    
    if (GetComputerNameW(netbiosBuffer, &netbiosSize)) {
        machineNames.netbiosName = netbiosBuffer;
        LogWideStringAsUtf8("[+] Retrieved NetBIOS name: ", machineNames.netbiosName);
    }
    else {
        dwError = GetLastError();
        if (dwError == ERROR_BUFFER_OVERFLOW) {
            // Buffer too small - netbiosSize now contains required size
            LogError("[-] NetBIOS name buffer too small, required size: " + std::to_string(netbiosSize));
        }
        else {
            LogError("[-] Failed to retrieve NetBIOS name, error: " + std::to_string(dwError));
        }
    }

    if (GetComputerNameExW(ComputerNameDnsFullyQualified, dnsBuffer, &dnsSize)) {
        machineNames.dnsName = dnsBuffer;
        LogWideStringAsUtf8("[+] Retrieved DNS/FQDN name: ", machineNames.dnsName);
    }
    else {
        dwError = GetLastError();

        if (dwError == ERROR_MORE_DATA) {
            LogError("[-] DNS name buffer too small, required size: " + std::to_string(dnsSize));
        }
        else {
            LogError("[-] Failed to retrieve DNS/FQDN name (error: " + std::to_string(dwError) + "), using NetBIOS name as fallback");
        }

		LogError("[+] Fallback: Setting DNS/FQDN name to NetBIOS name.");
        machineNames.dnsName = machineNames.netbiosName;
    }
    
    return machineNames;
}

// Helper: Extract IP string from addrinfo structure
std::string ExtractIPFromAddrInfo(addrinfo* p)
{
    char ipBuf[INET6_ADDRSTRLEN] = {};

    if (!p || !p->ai_addr) {
        return {};
    }
        
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

// Read REG_SZ safely
std::wstring GetRegistryString(HKEY hKey, const std::wstring& valueName)
{
    if (!hKey) {
        return L"";
    }

    DWORD type = 0;
    DWORD size = 0;
    DWORD actualSize = 0;
    size_t charCount = 0;

    // First call: get size and type
    if (RegQueryValueExW(hKey, valueName.c_str(), nullptr, &type, nullptr, &size) != ERROR_SUCCESS)
        return L"";

    if (type != REG_SZ && type != REG_EXPAND_SZ)
        return L"";

    if (size % sizeof(wchar_t) != 0) {
        LogError("[-] Registry value has invalid size (not wchar_t aligned): " + std::to_string(size));
        return L"";
    }

    // Allocate buffer for Windows API to write into.
    // Since, UTF-16 takes two bytes per char, convert byte size to wchar_t (UTF-16) count for buffer allocation
    charCount = size / sizeof(wchar_t); 
    std::wstring data(charCount, L'\0');

    // Second call: get data
    actualSize = size;
    if (RegQueryValueExW(hKey, valueName.c_str(), nullptr, nullptr, (LPBYTE)data.data(), &actualSize) != ERROR_SUCCESS)
    {
        return L"";
    }

    // Validate actual size matches expected
    if (actualSize != size) {
        LogError("[-] Registry value size changed between calls");
        return L"";
    }

    // Remove null terminator if present
    if (!data.empty() && data.back() == L'\0')
        data.pop_back();

    return data;
}

std::vector<InstalledApp> GetAppsFromUninstallKey(HKEY root, const std::wstring& subkey)
{
    std::vector<InstalledApp> apps;
    HKEY hKey;
    DWORD index = 0;
    constexpr DWORD MAX_KEY_LENGTH = 256;  // 255 chars + null terminator
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
        nameSize = MAX_KEY_LENGTH;  // IMP: Reset size before next iteration
    }

    // Check if enumeration ended normally or due to error
    if (result != ERROR_NO_MORE_ITEMS) {
        LogError("[-] Registry enumeration ended with error: " + std::to_string(result));
    }

    RegCloseKey(hKey);
    return apps;
}

std::vector<std::string> GetLocalIPAddresses()
{
    std::array<std::string, 7> ipArray;  // Stack allocation: 5 IPv4 + 2 IPv6
    constexpr int HOSTNAME_LEN = 256;
    constexpr int MAX_IPV4 = 5;
    constexpr int MAX_IPV6 = 2;
    int ipv4Count = 0;
    int ipv6Count = 0;
    int totalCount = 0;
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
        return std::vector<std::string>();
    }

    LogError("[+]Trying to get local machine's hostname.");
    if (gethostname(cHostname.data(), static_cast<int>(cHostname.size())) != 0) {
        LogError("[-]Error: gethostname() failed with error code: " + std::to_string(WSAGetLastError()));
        LogError("[+]Calling WSACleanup() to terminate the use of WinSock2 DLL.");
        WSACleanup();

        return std::vector<std::string>();
    }

    LogError("[+]Trying to get all IP addresses.");
    getaddrinfoResult = getaddrinfo(cHostname.data(), nullptr, &pHints, &pAddrInfoList);

    if (getaddrinfoResult != 0) {
        LogError("[-]Error: getaddrinfo() failed with error code: " + std::to_string(getaddrinfoResult));

        if (pAddrInfoList != nullptr) {
            freeaddrinfo(pAddrInfoList);
        }

        LogError("[+]Calling WSACleanup() to terminate the use of WinSock2 DLL.");
        WSACleanup();

        return std::vector<std::string>();
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
                ipArray[totalCount++] = sIPAddr;
                ipv4Count++;
                LogError("[+]IPv4 address collected: " + sIPAddr);
            }
        }
        // Collect IPv6 addresses (max 2)
        else if (p->ai_family == AF_INET6) {
            if (ipv6Count < MAX_IPV6) {
                ipArray[totalCount++] = sIPAddr;
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

    LogError("[+]Total IP addresses collected: " + std::to_string(totalCount) +
        " (IPv4: " + std::to_string(ipv4Count) + ", IPv6: " + std::to_string(ipv6Count) + ")");

    return std::vector<std::string>(ipArray.begin(), ipArray.begin() + totalCount);
}

// Enumerate all user profiles on the system
std::vector<UserProfile> EnumerateUserProfiles()
{
    std::vector<UserProfile> profiles;
    HKEY hProfileList = nullptr;

    // Open ProfileList registry key
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

    // Enumerate all SIDs in ProfileList
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

            // Extract username from profile path (e.g., C:\Users\Alice -> Alice)
            if (!profile.profilePath.empty())
            {
                size_t lastSlash = profile.profilePath.find_last_of(L"\\");
                if (lastSlash != std::wstring::npos)
                {
                    profile.username = profile.profilePath.substr(lastSlash + 1);
                }
            }

            // Check if registry hive is currently loaded
            HKEY hTestKey = nullptr;
            std::wstring testPath = sidBuffer;
            testPath += L"\\Software";
            profile.isLoaded = (RegOpenKeyExW(HKEY_USERS, testPath.c_str(), 0, KEY_READ, &hTestKey) == ERROR_SUCCESS);
            if (hTestKey)
                RegCloseKey(hTestKey);

            // Only add valid user profiles (skip system accounts)
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

// Get installed apps for a specific user (loads registry hive if needed)
std::vector<InstalledApp> GetUserInstalledApps(const UserProfile& userProfile)
{
    std::vector<InstalledApp> apps;
    bool hiveLoadedByUs = false;
    std::wstring hiveKeyName = L"TempHive_" + userProfile.username;
    std::wstring ntUserPath = userProfile.profilePath + L"\\NTUSER.DAT";

    // If hive not already loaded, load it
    if (!userProfile.isLoaded)
    {
        // Enable required privileges
        if (!EnablePrivilege(SE_RESTORE_NAME) || !EnablePrivilege(SE_BACKUP_NAME))
        {
            LogError("[-] Failed to enable privileges for loading user hive: " + WideToUtf8(userProfile.username));
            return apps;
        }

        // Load the user's registry hive
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

    // Construct registry path
    std::wstring registryPath;
    if (hiveLoadedByUs)
    {
        registryPath = hiveKeyName + L"\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    }
    else
    {
        registryPath = userProfile.sid + L"\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    }

    // Open and enumerate apps
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

    // Unload hive if we loaded it
    if (hiveLoadedByUs)
    {
        // Dynamically load RegUnloadKeyW for maximum compatibility
        typedef LSTATUS (WINAPI *PFN_REGUNLOADKEYW)(HKEY, LPCWSTR);
        HMODULE hAdvapi32 = GetModuleHandleW(L"advapi32.dll");
        
        if (hAdvapi32)
        {
            PFN_REGUNLOADKEYW pfnRegUnloadKeyW = (PFN_REGUNLOADKEYW)GetProcAddress(hAdvapi32, "RegUnloadKeyW");
            
            if (pfnRegUnloadKeyW)
            {
                LONG unloadResult = pfnRegUnloadKeyW(HKEY_USERS, hiveKeyName.c_str());
                if (unloadResult != ERROR_SUCCESS)
                {
                    LogError("[-] Warning: Failed to unload registry hive for user '" + WideToUtf8(userProfile.username) +
                            "', error: " + std::to_string(unloadResult));
                }
                else
                {
                    LogError("[+] Unloaded registry hive for user: " + WideToUtf8(userProfile.username));
                }
            }
            else
            {
                LogError("[-] Warning: RegUnloadKeyW not available, registry hive may remain loaded");
            }
        }

        // Disable privileges (security best practice)
        DisablePrivilege(SE_RESTORE_NAME);
        DisablePrivilege(SE_BACKUP_NAME);
    }

    LogError("[+] Found " + std::to_string(apps.size()) + " apps for user: " + WideToUtf8(userProfile.username));

    return apps;
}

// ============================================================================
// APPX / MSIX PACKAGE ENUMERATION
// ============================================================================

// Helper: Parse package full name into components
void ParsePackageFullName(const std::wstring& fullName, AppXPackage& package)
{
    // Package full name format: Name_Version_Architecture_ResourceId_PublisherId
    // Example: Microsoft.WindowsCalculator_10.1906.55.0_x64__8wekyb3d8bbwe
    
    size_t pos = 0;
    size_t nextUnderscore = fullName.find(L'_', pos);
    
    if (nextUnderscore != std::wstring::npos)
    {
        // Extract display name (first component)
        std::wstring name = fullName.substr(pos, nextUnderscore - pos);
        package.displayName = name;
        
        pos = nextUnderscore + 1;
        nextUnderscore = fullName.find(L'_', pos);
        
        if (nextUnderscore != std::wstring::npos)
        {
            // Extract version
            package.version = fullName.substr(pos, nextUnderscore - pos);
            
            pos = nextUnderscore + 1;
            nextUnderscore = fullName.find(L'_', pos);
            
            if (nextUnderscore != std::wstring::npos)
            {
                // Extract architecture
                package.architecture = fullName.substr(pos, nextUnderscore - pos);
            }
        }
    }
}

// Enumerate all AppX packages installed system-wide
std::vector<AppXPackage> EnumerateAppXPackages()
{
    std::vector<AppXPackage> packages;
    HKEY hPackages = nullptr;

    // Open the AppX package repository
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

    // Enumerate all package full names
    while ((result = RegEnumKeyExW(hPackages, index, packageFullName, &nameSize,
                                   nullptr, nullptr, nullptr, nullptr)) == ERROR_SUCCESS)
    {
        AppXPackage package;
        package.packageFullName = packageFullName;
        
        // Parse package name components
        ParsePackageFullName(packageFullName, package);
        
        // Open package key to read details
        HKEY hPackageKey = nullptr;
        if (RegOpenKeyExW(hPackages, packageFullName, 0, KEY_READ, &hPackageKey) == ERROR_SUCCESS)
        {
            // Read install location
            package.installLocation = GetRegistryString(hPackageKey, L"Path");
            
            // Check if it's a framework package
            std::wstring isFramework = GetRegistryString(hPackageKey, L"IsFramework");
            package.isFramework = (isFramework == L"1");
            
            RegCloseKey(hPackageKey);
        }

        // Extract package family name (Name_PublisherId)
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

// Get AppX packages for a specific user
std::vector<AppXPackage> GetUserAppXPackages(const std::wstring& userSid)
{
    std::vector<AppXPackage> packages;
    HKEY hUserPackages = nullptr;

    // Construct path to user's AppX packages
    std::wstring userPackagePath = L"SOFTWARE\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Packages";
    std::wstring fullPath = userSid + L"\\" + userPackagePath;

    if (RegOpenKeyExW(HKEY_USERS, fullPath.c_str(), 0, KEY_READ, &hUserPackages) != ERROR_SUCCESS)
    {
        // User may not have any AppX packages installed, which is normal
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

        // Open package key for additional info
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