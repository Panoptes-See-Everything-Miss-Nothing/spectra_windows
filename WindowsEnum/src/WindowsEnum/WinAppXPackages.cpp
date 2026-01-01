#include "WinAppXPackages.h"

// Only include WinRT headers if Windows 8 or later
#if _WIN32_WINNT >= _WIN32_WINNT_WIN8
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Management.Deployment.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.h>
#include <appmodel.h>

// Link required libraries
#pragma comment(lib, "WindowsApp.lib")

using namespace winrt;
using namespace winrt::Windows::Management::Deployment;
using namespace winrt::Windows::ApplicationModel;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::Storage;

#define WINRT_AVAILABLE 1
#else
#define WINRT_AVAILABLE 0
#endif

#include <sddl.h>

// Link kernel32 for version detection
#pragma comment(lib, "kernel32.lib")

// Helper: Check if modern apps are supported on this Windows version
bool IsModernAppsSupported()
{
    // Check Windows version at runtime
    // Modern apps require Windows 8 (6.2) or later
    
    OSVERSIONINFOEXW osvi = {};
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    osvi.dwMajorVersion = 6;
    osvi.dwMinorVersion = 2;  // Windows 8 is 6.2

    DWORDLONG dwlConditionMask = 0;
    VER_SET_CONDITION(dwlConditionMask, VER_MAJORVERSION, VER_GREATER_EQUAL);
    VER_SET_CONDITION(dwlConditionMask, VER_MINORVERSION, VER_GREATER_EQUAL);

    return VerifyVersionInfoW(&osvi, VER_MAJORVERSION | VER_MINORVERSION, dwlConditionMask) != FALSE;
}

// Helper: Get Windows version string for logging
std::string GetWindowsVersionString()
{
    OSVERSIONINFOEXW osvi = {};
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    
    // Use RtlGetVersion for accurate version (GetVersionEx is deprecated and lies)
    typedef LONG(WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
    auto RtlGetVersion = reinterpret_cast<RtlGetVersionPtr>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    
    if (RtlGetVersion)
    {
        RtlGetVersion(reinterpret_cast<PRTL_OSVERSIONINFOW>(&osvi));
    }
    
    std::stringstream ss;
    ss << osvi.dwMajorVersion << "." << osvi.dwMinorVersion << "." << osvi.dwBuildNumber;
    return ss.str();
}

// ComInitializer implementation
ComInitializer::ComInitializer() : m_initialized(false)
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE)
    {
        m_initialized = true;
    }
    else
    {
        LogError("[-] Failed to initialize COM for Package Manager, HRESULT: 0x" + 
                 std::to_string(static_cast<unsigned long>(hr)));
    }
}

ComInitializer::~ComInitializer()
{
    if (m_initialized)
    {
        CoUninitialize();
    }
}

std::wstring GetArchitectureString(UINT32 architecture)
{
    switch (architecture)
    {
    case 0: return L"x86";
    case 5: return L"ARM";
    case 9: return L"x64";
    case 11: return L"Neutral";
    case 12: return L"ARM64";
    default: return L"Unknown";
    }
}

std::wstring GetPackageOrigin(UINT32 origin)
{
    switch (origin)
    {
    case 0: return L"Unknown";
    case 1: return L"Unsigned";
    case 2: return L"Inbox";
    case 3: return L"Store";
    case 4: return L"DeveloperUnsigned";
    case 5: return L"DeveloperSigned";
    case 6: return L"LineOfBusiness";
    default: return L"Unknown";
    }
}

#if WINRT_AVAILABLE
// WinRT-based implementation (Windows 8+)

std::vector<ModernAppPackage> EnumerateAllModernAppPackages()
{
    std::vector<ModernAppPackage> packages;
    
    // Runtime check for Windows 8+
    if (!IsModernAppsSupported())
    {
        LogError("[*] Modern apps not supported on Windows " + GetWindowsVersionString() + 
                 " (requires Windows 8 or later)");
        LogError("[*] Skipping modern app enumeration - not applicable for this OS version");
        return packages;
    }
    
    // Initialize COM
    ComInitializer comInit;
    if (!comInit.IsInitialized())
    {
        LogError("[-] COM initialization failed for Package Manager");
        return packages;
    }

    try
    {
        // Initialize WinRT
        init_apartment();
        
        LogError("[+] Initializing Windows Package Manager on Windows " + GetWindowsVersionString() + "...");
        
        // Create PackageManager instance
        PackageManager packageManager;
        
        // Get all packages for all users
        IIterable<Package> allPackages = packageManager.FindPackages();
        
        LogError("[+] Enumerating all modern app packages...");
        
        for (const auto& package : allPackages)
        {
            try
            {
                ModernAppPackage appPackage{};
                
                // Get package identity
                auto packageId = package.Id();
                
                appPackage.packageFullName = packageId.FullName().c_str();
                appPackage.packageFamilyName = packageId.FamilyName().c_str();
                appPackage.publisher = packageId.Publisher().c_str();
                appPackage.publisherId = packageId.PublisherId().c_str();
                appPackage.resourceId = packageId.ResourceId().c_str();
                
                // Get version
                auto version = packageId.Version();
                std::wstringstream versionStream;
                versionStream << version.Major << L"." << version.Minor << L"." 
                             << version.Build << L"." << version.Revision;
                appPackage.version = versionStream.str();
                
                // Get architecture
                appPackage.architecture = GetArchitectureString(static_cast<UINT32>(packageId.Architecture()));
                
                // Get install location
                try
                {
                    auto installedLocation = package.InstalledLocation();
                    if (installedLocation)
                    {
                        appPackage.installLocation = installedLocation.Path();
                    }
                }
                catch (...)
                {
                    appPackage.installLocation = L"";
                }
                
                // Get display name and other manifest properties
                try
                {
                    appPackage.displayName = package.DisplayName().c_str();
                    appPackage.publisherDisplayName = package.PublisherDisplayName().c_str();
                    appPackage.description = package.Description().c_str();
                    
                    auto logo = package.Logo();
                    if (logo)
                    {
                        appPackage.logo = logo.ToString().c_str();
                    }
                }
                catch (...)
                {
                    appPackage.displayName = appPackage.packageFamilyName;
                }
                
                // Get package properties
                appPackage.isFramework = package.IsFramework();
                appPackage.isBundle = package.IsBundle();
                appPackage.isResourcePackage = package.IsResourcePackage();
                appPackage.isDevelopmentMode = package.IsDevelopmentMode();
                
                // Get users who have this package
                try
                {
                    auto users = packageManager.FindUsers(appPackage.packageFullName);
                    for (const auto& user : users)
                    {
                        std::wstring userSid = user.UserSecurityId().c_str();
                        if (!userSid.empty())
                        {
                            appPackage.users.push_back(userSid);
                        }
                    }
                }
                catch (...)
                {
                    // Failed to get users - package might be provisioned or system-level
                }
                
                packages.push_back(std::move(appPackage));
            }
            catch (const hresult_error& ex)
            {
                std::wstring errorMsg = ex.message().c_str();
                LogError("[-] Failed to process package: " + WideToUtf8(errorMsg));
            }
        }
        
        LogError("[+] Successfully enumerated " + std::to_string(packages.size()) + " modern app packages");
    }
    catch (const hresult_error& ex)
    {
        std::wstring errorMsg = ex.message().c_str();
        LogError("[-] PackageManager enumeration failed: " + WideToUtf8(errorMsg) + 
                 ", HRESULT: 0x" + std::to_string(static_cast<unsigned long>(ex.code())));
    }
    catch (const std::exception& ex)
    {
        LogError(std::string("[-] Exception during package enumeration: ") + ex.what());
    }
    
    return packages;
}

std::vector<ModernAppPackage> GetModernAppPackagesForUser(const std::wstring& userSid)
{
    std::vector<ModernAppPackage> packages;
    
    // Runtime check for Windows 8+
    if (!IsModernAppsSupported())
    {
        LogError("[*] Modern apps not supported on Windows " + GetWindowsVersionString() + 
                 " (requires Windows 8 or later)");
        return packages;
    }
    
    // Initialize COM
    ComInitializer comInit;
    if (!comInit.IsInitialized())
    {
        LogError("[-] COM initialization failed for Package Manager");
        return packages;
    }

    try
    {
        // Initialize WinRT
        init_apartment();
        
        LogError("[+] Enumerating modern app packages for user SID: " + WideToUtf8(userSid));
        
        // Create PackageManager instance
        PackageManager packageManager;
        
        // Get packages for specific user
        hstring userSidHstring{ userSid };
        
        IIterable<Package> userPackages = packageManager.FindPackagesForUser(userSidHstring);
        
        for (const auto& package : userPackages)
        {
            try
            {
                ModernAppPackage appPackage{};
                
                // Get package identity
                auto packageId = package.Id();
                
                appPackage.packageFullName = packageId.FullName().c_str();
                appPackage.packageFamilyName = packageId.FamilyName().c_str();
                appPackage.publisher = packageId.Publisher().c_str();
                appPackage.publisherId = packageId.PublisherId().c_str();
                appPackage.resourceId = packageId.ResourceId().c_str();
                
                // Get version
                auto version = packageId.Version();
                std::wstringstream versionStream;
                versionStream << version.Major << L"." << version.Minor << L"." 
                             << version.Build << L"." << version.Revision;
                appPackage.version = versionStream.str();
                
                // Get architecture
                appPackage.architecture = GetArchitectureString(static_cast<UINT32>(packageId.Architecture()));
                
                // Get install location
                try
                {
                    auto installedLocation = package.InstalledLocation();
                    if (installedLocation)
                    {
                        appPackage.installLocation = installedLocation.Path();
                    }
                }
                catch (...)
                {
                    appPackage.installLocation = L"";
                }
                
                // Get display name and other manifest properties
                try
                {
                    appPackage.displayName = package.DisplayName().c_str();
                    appPackage.publisherDisplayName = package.PublisherDisplayName().c_str();
                    appPackage.description = package.Description().c_str();
                    
                    auto logo = package.Logo();
                    if (logo)
                    {
                        appPackage.logo = logo.ToString().c_str();
                    }
                }
                catch (...)
                {
                    appPackage.displayName = appPackage.packageFamilyName;
                }
                
                // Get package properties
                appPackage.isFramework = package.IsFramework();
                appPackage.isBundle = package.IsBundle();
                appPackage.isResourcePackage = package.IsResourcePackage();
                appPackage.isDevelopmentMode = package.IsDevelopmentMode();
                
                // Add the current user SID
                appPackage.users.push_back(userSid);
                
                packages.push_back(std::move(appPackage));
            }
            catch (const hresult_error& ex)
            {
                std::wstring errorMsg = ex.message().c_str();
                LogError("[-] Failed to process package for user: " + WideToUtf8(errorMsg));
            }
        }
        
        LogError("[+] Found " + std::to_string(packages.size()) + 
                 " modern app packages for user: " + WideToUtf8(userSid));
    }
    catch (const hresult_error& ex)
    {
        std::wstring errorMsg = ex.message().c_str();
        LogError("[-] PackageManager enumeration failed for user: " + WideToUtf8(errorMsg) + 
                 ", HRESULT: 0x" + std::to_string(static_cast<unsigned long>(ex.code())));
    }
    catch (const std::exception& ex)
    {
        LogError(std::string("[-] Exception during user package enumeration: ") + ex.what());
    }
    
    return packages;
}

#else
// Fallback implementation for Windows 7 and earlier

std::vector<ModernAppPackage> EnumerateAllModernAppPackages()
{
    std::vector<ModernAppPackage> packages;
    LogError("[*] Modern apps (UWP/MSIX) are not supported on Windows " + GetWindowsVersionString());
    LogError("[*] Modern apps require Windows 8 or later - skipping enumeration");
    return packages;
}

std::vector<ModernAppPackage> GetModernAppPackagesForUser(const std::wstring& userSid)
{
    std::vector<ModernAppPackage> packages;
    LogError("[*] Modern apps (UWP/MSIX) are not supported on Windows " + GetWindowsVersionString());
    return packages;
}

#endif // WINRT_AVAILABLE
