#define WIN32_LEAN_AND_MEAN
#include "WinAppXPackages.h"
#include <Windows.h>
#include <objbase.h>
#include <sddl.h>
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

// Helper: Get Windows version string for logging
std::string GetWindowsVersionString()
{
    OSVERSIONINFOEXW osvi = {};
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    
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

std::vector<ModernAppPackage> EnumerateAllModernAppPackages()
{
    std::vector<ModernAppPackage> packages;
    
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
        
        LogError("[+] Initializing Windows Package Manager...");
        
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

std::vector<ModernAppPackage> GetModernAppPackagesForUser(const std::wstring& userSid, const std::wstring& username)
{
    std::vector<ModernAppPackage> packages;
    
    // Determine display name for logging (username if provided, otherwise SID)
    std::string userDisplayName = !username.empty() ? WideToUtf8(username) : WideToUtf8(userSid);
    
    // Initialize COM
    ComInitializer comInit;
    if (!comInit.IsInitialized())
    {
        LogError("[-] COM initialization failed for Package Manager");
        return packages;
    }

    try
    {
        // Initialize WinRT (handles COM initialization internally)
        // If COM is already initialized in a different mode (e.g., from registry operations),
        // init_apartment() will handle it gracefully
        init_apartment();
        
        LogError("[+] Enumerating modern app packages for user: " + userDisplayName);
        
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
                 " modern app packages for user: " + userDisplayName);
    }
    catch (const hresult_error& ex)
    {
        std::wstring errorMsg = ex.message().c_str();
        LogError("[-] PackageManager enumeration failed for user " + userDisplayName + ": " + WideToUtf8(errorMsg) +
                 ", HRESULT: 0x" + std::to_string(static_cast<unsigned long>(ex.code())));
    }
    catch (const std::exception& ex)
    {
        LogError(std::string("[-] Exception during package enumeration for user ") + userDisplayName + ": " + ex.what());
    }
    
    
    return packages;
}
