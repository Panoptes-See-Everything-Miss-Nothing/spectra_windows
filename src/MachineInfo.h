#pragma once

#include "./Utils/Utils.h"
#include <WinSock2.h>
#include <WS2tcpip.h>

// Machine name information
struct MachineNames
{
    std::wstring netbiosName;
    std::wstring dnsName;
};

// Get machine NetBIOS and DNS names
MachineNames GetMachineName();

// Get local IP addresses (max 5 IPv4 + 2 IPv6)
std::vector<std::string> GetLocalIPAddresses();

// Helper: Extract IP string from addrinfo structure (internal use)
std::string ExtractIPFromAddrInfo(addrinfo* p);
