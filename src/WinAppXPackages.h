#pragma once

#include "./Utils/Utils.h"
#include <vector>
#include <string>
#include <windows.h>

// Modern AppX/MSIX package information
struct ModernAppPackage
{
    std::wstring packageFullName;
    std::wstring packageFamilyName;
    std::wstring displayName;
    std::wstring publisher;
    std::wstring publisherId;
    std::wstring version;
    std::wstring architecture;
    std::wstring installLocation;
    std::wstring resourceId;
    std::wstring publisherDisplayName;
    std::wstring description;
    std::wstring logo;
    bool isFramework;
    bool isBundle;
    bool isResourcePackage;
    bool isDevelopmentMode;
    std::vector<std::wstring> users; // SIDs of users who have this package
};

// RAII wrapper for COM initialization
class ComInitializer
{
public:
    ComInitializer();
    ~ComInitializer();
    
    // Delete copy/move constructors
    ComInitializer(const ComInitializer&) = delete;
    ComInitializer& operator=(const ComInitializer&) = delete;
    ComInitializer(ComInitializer&&) = delete;
    ComInitializer& operator=(ComInitializer&&) = delete;
    
    bool IsInitialized() const { return m_initialized; }

private:
    bool m_initialized;
};

// Enumerate all modern app packages on the system using Windows Package Manager API
// This provides complete visibility including Store apps, sideloaded apps, provisioned apps
std::vector<ModernAppPackage> EnumerateAllModernAppPackages();

// Get packages for a specific user using Package Manager API
// userSid: The security identifier of the user
// username: Optional username for logging purposes (defaults to empty string, will use SID)
std::vector<ModernAppPackage> GetModernAppPackagesForUser(const std::wstring& userSid, const std::wstring& username = L"");

// Helper: Get package architecture as string
std::wstring GetArchitectureString(UINT32 architecture);

// Helper: Get package origin (Store, Sideload, etc.)
std::wstring GetPackageOrigin(UINT32 origin);
