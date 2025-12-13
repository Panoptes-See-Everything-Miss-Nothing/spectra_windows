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
    DWORD type = 0;
    DWORD size = 0;

    if (RegQueryValueExW(hKey, valueName.c_str(), nullptr, &type, nullptr, &size) != ERROR_SUCCESS)
        return L"";

    if (type != REG_SZ && type != REG_EXPAND_SZ)
        return L"";

    std::wstring data(size / sizeof(wchar_t), L'\0');

    if (RegQueryValueExW(hKey, valueName.c_str(), nullptr, nullptr, (LPBYTE)data.data(), &size) != ERROR_SUCCESS)
        return L"";

    if (!data.empty() && data.back() == L'\0')
        data.pop_back();

    return data;
}

std::vector<InstalledApp> GetAppsFromUninstallKey(HKEY root, const std::wstring& subkey)
{
    std::vector<InstalledApp> apps;

    HKEY hKey;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return apps;

    DWORD index = 0;
    WCHAR name[256];
    DWORD nameSize = 256;

    while (RegEnumKeyExW(hKey, index, name, &nameSize, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
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
        nameSize = 256;
    }

    RegCloseKey(hKey);
    return apps;
}

std::vector<std::string> GetAllIPAddresses()
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