#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <Msi.h>
#include "../Utils/Utils.h"

// MSI-installed application information
struct MsiApp
{
    std::wstring productCode;        // GUID identifying the product
    std::wstring productName;        // Display name
    std::wstring productVersion;     // Version string
    std::wstring publisher;          // Vendor/Publisher
    std::wstring installDate;        // Installation date
    std::wstring installLocation;    // Install directory
    std::wstring installSource;      // Source location of MSI
    std::wstring packageCode;        // Package GUID
    std::wstring assignmentType;     // Per-user or Per-machine
    std::wstring language;           // Product language
    INSTALLSTATE installState;       // Current install state
};

// Enumerate all MSI-installed products (system-wide and per-user)
std::vector<MsiApp> EnumerateMsiProducts();

// Enumerate MSI products for a specific user context
std::vector<MsiApp> EnumerateMsiProductsForUser(const std::wstring& userSid);
