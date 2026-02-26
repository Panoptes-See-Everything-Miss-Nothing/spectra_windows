#include "MachineInfo.h"

MachineNames GetMachineName()
{
    MachineNames machineNames;
    DWORD dwError = {};
    WCHAR netbiosBuffer[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD netbiosSize = MAX_COMPUTERNAME_LENGTH + 1;
    constexpr DWORD DNS_BUFFER_SIZE = 256;
    WCHAR dnsBuffer[DNS_BUFFER_SIZE] = {};
    DWORD dnsSize = DNS_BUFFER_SIZE;
    
    if (GetComputerNameW(netbiosBuffer, &netbiosSize)) {
        machineNames.netbiosName = netbiosBuffer;
        LogWideStringAsUtf8("[+] Retrieved NetBIOS name: ", machineNames.netbiosName);
    }
    else {
        dwError = GetLastError();
        if (dwError == ERROR_BUFFER_OVERFLOW) {
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

std::vector<std::string> GetLocalIPAddresses()
{
    std::array<std::string, 7> ipArray;
    constexpr int HOSTNAME_LEN = 256;
    constexpr int MAX_IPV4 = 5;
    constexpr int MAX_IPV6 = 2;
    int ipv4Count = 0;
    int ipv6Count = 0;
    int totalCount = 0;
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
    int getaddrinfoResult = getaddrinfo(cHostname.data(), nullptr, &pHints, &pAddrInfoList);

    if (getaddrinfoResult != 0) {
        LogError("[-]Error: getaddrinfo() failed with error code: " + std::to_string(getaddrinfoResult));

        if (pAddrInfoList != nullptr) {
            freeaddrinfo(pAddrInfoList);
        }

        LogError("[+]Calling WSACleanup() to terminate the use of WinSock2 DLL.");
        WSACleanup();
        return std::vector<std::string>();
    }

    for (addrinfo* p = pAddrInfoList; p != nullptr; p = p->ai_next)
    {
        if (ipv4Count >= MAX_IPV4 && ipv6Count >= MAX_IPV6) {
            LogError("[+]IP collection limit reached (5 IPv4 + 2 IPv6)");
            break;
        }

        sIPAddr = ExtractIPFromAddrInfo(p);

        if (sIPAddr.empty() || IsLoopbackIP(sIPAddr)) {
            continue;
        }

        if (p->ai_family == AF_INET) {
            if (ipv4Count < MAX_IPV4) {
                ipArray[totalCount++] = sIPAddr;
                ipv4Count++;
                LogError("[+]IPv4 address collected: " + sIPAddr);
            }
        }
        else if (p->ai_family == AF_INET6) {
            if (ipv6Count < MAX_IPV6) {
                ipArray[totalCount++] = sIPAddr;
                ipv6Count++;
                LogError("[+]IPv6 address collected: " + sIPAddr);
            }
        }
    }

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
