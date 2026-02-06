#include "ServiceInstaller.h"
#include "ServiceConfig.h"
#include "ServiceTamperProtection.h"
#include <shlobj.h>
#include <aclapi.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

// RAII deleter for LocalAlloc-based allocations (ACLs, SIDs, security descriptors)
struct LocalFreeDeleter {
    void operator()(void* ptr) const noexcept {
        if (ptr) LocalFree(ptr);
    }
};

template <typename T>
using LocalUniquePtr = std::unique_ptr<T, LocalFreeDeleter>;

// Install the service with full security hardening
bool ServiceInstaller::InstallService()
{
    LogError("[+] ==========================================================");
    LogError("[+] Installing " + WideToUtf8(ServiceConfig::SERVICE_DISPLAY_NAME));
    LogError("[+] ==========================================================");

    // Step 1: Copy executable to Program Files
    std::wstring installedExePath = CopyExecutableToInstallLocation();
    if (installedExePath.empty())
    {
        LogError("[-] Failed to copy executable to installation directory");
        LogError("[-] Installation aborted");
        return false;
    }

    LogError("[+] Executable installed to: " + WideToUtf8(installedExePath));

    // Step 2: Create directory structure (directories only, ACLs applied later)
    // ACLs cannot be applied yet because the service SID (NT SERVICE\PanoptesSpectra)
    // does not exist until CreateServiceW registers it with SCM.
    if (!CreateSecureDirectories())
    {
        LogError("[-] Failed to create secure directories");
        LogError("[-] Installation aborted");
        return false;
    }

    // Step 2.5: Create registry configuration with default values
    if (!CreateRegistryConfiguration())
    {
        LogError("[-] Failed to create registry configuration");
        LogError("[-] Installation aborted");
        return false;
    }

    // Step 3: Get SCM handle
    SC_HANDLE hSCManager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!hSCManager)
    {
        DWORD error = GetLastError();
        LogError("[-] Failed to open Service Control Manager, error: " + std::to_string(error));
        LogError("[-] Please run as Administrator");
        return false;
    }

    // Step 4: Create quoted path for installed executable
    std::wstring servicePath = L"\"" + installedExePath + L"\"";
    LogError("[+] Service executable: " + WideToUtf8(servicePath));

    // Step 5: Check if service already exists
    SC_HANDLE hExistingService = OpenServiceW(hSCManager, ServiceConfig::SERVICE_NAME, SERVICE_QUERY_STATUS);
    if (hExistingService)
    {
        LogError("[!] Service already exists");
        CloseServiceHandle(hExistingService);
        CloseServiceHandle(hSCManager);
        LogError("[!] To reinstall, first run: Panoptes-Spectra.exe /uninstall");
        return false;
    }

    // Step 6: Create the service
    // IMPORTANT: This registers the per-service SID (NT SERVICE\PanoptesSpectra)
    // with SCM, which is required before we can apply directory ACLs referencing it.
    SC_HANDLE hService = CreateServiceW(
        hSCManager,
        ServiceConfig::SERVICE_NAME,
        ServiceConfig::SERVICE_DISPLAY_NAME,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        ServiceConfig::SERVICE_START_TYPE,
        SERVICE_ERROR_NORMAL,
        servicePath.c_str(),
        nullptr,                              // No load order group
        nullptr,                              // No tag identifier
        ServiceConfig::SERVICE_DEPENDENCIES,  // No dependencies
        nullptr,                              // LocalSystem account (default)
        nullptr                               // No password
    );

    if (!hService)
    {
        DWORD error = GetLastError();
        LogError("[-] CreateService failed, error: " + std::to_string(error) + 
                 " - " + GetWindowsErrorMessage(error));
        CloseServiceHandle(hSCManager);
        return false;
    }

    LogError("[+] Service created successfully");

    // Step 7: Set service description
    SERVICE_DESCRIPTIONW sd = {};
    sd.lpDescription = const_cast<LPWSTR>(ServiceConfig::SERVICE_DESCRIPTION);
    if (!ChangeServiceConfig2W(hService, SERVICE_CONFIG_DESCRIPTION, &sd))
    {
        LogError("[-] WARNING: Failed to set service description");
    }

    // Step 8: Apply service hardening (SID restriction, privileges, failure actions)
    if (!ApplyServiceHardening(hService))
    {
        LogError("[-] WARNING: Failed to apply full service hardening");
    }

    // Step 9: Apply directory ACLs NOW that the service SID exists in SCM.
    // SERVICE_SID_TYPE_RESTRICTED creates a write-restricted token where only
    // SIDs in the restricting list can pass write-access checks. The per-service
    // SID (NT SERVICE\PanoptesSpectra) must be explicitly granted Modify access
    // on every directory the service writes to.
    {
        const std::vector<std::wstring> directories = {
            ServiceConfig::OUTPUT_DIRECTORY,
            ServiceConfig::LOG_DIRECTORY,
            ServiceConfig::CONFIG_DIRECTORY,
            ServiceConfig::TEMP_DIRECTORY
        };

        for (const auto& dir : directories)
        {
            if (!ApplyDirectoryAcl(dir))
            {
                LogError("[-] WARNING: Failed to apply ACL to: " + WideToUtf8(dir));
                LogError("[!] Service may fail to write to this directory at runtime");
            }
        }
    }

    // Step 10: Apply tamper protection (restrict who can control the service)
    if (!ServiceTamperProtection::ApplyTamperProtectionDACL(hService))
    {
        LogError("[-] WARNING: Failed to apply tamper protection");
        LogError("[!] Service may be vulnerable to unauthorized modification");
    }

    // Step 11: Verify installation
    if (!VerifyInstallation(hService))
    {
        LogError("[-] WARNING: Installation verification failed");
    }

    // Step 12: Start the service automatically
    LogError("[+] Starting service...");
    if (!StartServiceW(hService, 0, nullptr))
    {
        DWORD error = GetLastError();
        LogError("[-] WARNING: Failed to start service automatically, error: " + std::to_string(error));
        LogError("[!] You can start it manually with: sc start " + WideToUtf8(ServiceConfig::SERVICE_NAME));
    }
    else
    {
        LogError("[+] Service started successfully!");
        
        // Wait a moment for service to fully start
        Sleep(2000);
        
        SERVICE_STATUS ss = {};
        if (QueryServiceStatus(hService, &ss) && ss.dwCurrentState == SERVICE_RUNNING)
        {
            LogError("[+] Service is now RUNNING");
        }
    }

    CloseServiceHandle(hService);
    CloseServiceHandle(hSCManager);

    LogError("[+] ==========================================================");
    LogError("[+] Installation completed successfully!");
    LogError("[+] Service Name: " + WideToUtf8(ServiceConfig::SERVICE_NAME));
    LogError("[+] Display Name: " + WideToUtf8(ServiceConfig::SERVICE_DISPLAY_NAME));
    LogError("[+] Installed to: " + WideToUtf8(installedExePath));
    LogError("[+] Output directory: " + WideToUtf8(ServiceConfig::OUTPUT_DIRECTORY));
    LogError("[+] ==========================================================");

    return true;
}

// Uninstall the service cleanly
bool ServiceInstaller::UninstallService()
{
    LogError("[+] Uninstalling " + WideToUtf8(ServiceConfig::SERVICE_DISPLAY_NAME) + "...");

    SC_HANDLE hSCManager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!hSCManager)
    {
        DWORD error = GetLastError();
        LogError("[-] Failed to open Service Control Manager, error: " + std::to_string(error));
        return false;
    }

    SC_HANDLE hService = OpenServiceW(hSCManager, ServiceConfig::SERVICE_NAME, 
                                       SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (!hService)
    {
        DWORD error = GetLastError();
        if (error == ERROR_SERVICE_DOES_NOT_EXIST)
        {
            LogError("[!] Service is not installed");
        }
        else
        {
            LogError("[-] Failed to open service, error: " + std::to_string(error));
        }
        CloseServiceHandle(hSCManager);
        return false;
    }

    // Stop the service if it's running
    SERVICE_STATUS ss = {};
    if (QueryServiceStatus(hService, &ss))
    {
        if (ss.dwCurrentState != SERVICE_STOPPED)
        {
            LogError("[+] Stopping service...");
            if (ControlService(hService, SERVICE_CONTROL_STOP, &ss))
            {
                // Wait for service to stop
                for (int i = 0; i < 30; i++)
                {
                    if (!QueryServiceStatus(hService, &ss))
                        break;
                    
                    if (ss.dwCurrentState == SERVICE_STOPPED)
                        break;
                    
                    Sleep(1000);
                }

                if (ss.dwCurrentState == SERVICE_STOPPED)
                {
                    LogError("[+] Service stopped successfully");
                }
                else
                {
                    LogError("[-] WARNING: Service did not stop in time");
                }
            }
        }
    }

    // Remove tamper protection to allow deletion
    ServiceTamperProtection::RemoveTamperProtection(hService);

    // Delete the service
    if (!DeleteService(hService))
    {
        DWORD error = GetLastError();
        LogError("[-] DeleteService failed, error: " + std::to_string(error));
        CloseServiceHandle(hService);
        CloseServiceHandle(hSCManager);
        return false;
    }

    CloseServiceHandle(hService);
    CloseServiceHandle(hSCManager);

    LogError("[+] Service uninstalled successfully");
    LogError("[!] Note: Runtime data in " + WideToUtf8(ServiceConfig::OUTPUT_DIRECTORY) + " was not removed");
    LogError("[!] To remove all data, manually delete: C:\\ProgramData\\Panoptes");

    return true;
}

// Get the quoted executable path (protects against unquoted service path attacks)
std::wstring ServiceInstaller::GetQuotedExecutablePath()
{
    WCHAR exePath[MAX_PATH] = {};
    DWORD pathLen = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    
    if (pathLen == 0 || pathLen >= MAX_PATH)
    {
        LogError("[-] GetModuleFileName failed");
        return L"";
    }

    // CRITICAL SECURITY: Always quote the path to prevent unquoted service path attacks
    // Even though our service name has no spaces, this is defense in depth
    std::wstring quotedPath = L"\"";
    quotedPath += exePath;
    quotedPath += L"\"";

    // Verify the path doesn't contain suspicious characters
    if (quotedPath.find(L'\0') != std::wstring::npos)
    {
        LogError("[-] SECURITY: Executable path contains null bytes - possible attack");
        return L"";
    }

    return quotedPath;
}

// Copy executable to Program Files installation directory
std::wstring ServiceInstaller::CopyExecutableToInstallLocation()
{
    // Get current executable path
    WCHAR currentExePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, currentExePath, MAX_PATH) == 0)
    {
        LogError("[-] Failed to get current executable path");
        return L"";
    }

    // Define installation directory
    std::wstring installDir = L"C:\\Program Files\\Panoptes\\Spectra";
    
    // Create installation directory
    int result = SHCreateDirectoryExW(nullptr, installDir.c_str(), nullptr);
    if (result != ERROR_SUCCESS && result != ERROR_ALREADY_EXISTS)
    {
        LogError("[-] Failed to create installation directory: " + WideToUtf8(installDir));
        return L"";
    }

    LogError("[+] Installation directory: " + WideToUtf8(installDir));

    // Define target path
    std::wstring targetPath = installDir + L"\\Panoptes-Spectra.exe";

    // Copy executable
    if (!CopyFileW(currentExePath, targetPath.c_str(), FALSE))
    {
        DWORD error = GetLastError();
        LogError("[-] Failed to copy executable, error: " + std::to_string(error));
        LogError("[-] Source: " + WideToUtf8(currentExePath));
        LogError("[-] Target: " + WideToUtf8(targetPath));
        return L"";
    }

    LogError("[+] Executable copied successfully");
    return targetPath;
}

// Apply service hardening (SID restriction, required privileges, failure actions)
bool ServiceInstaller::ApplyServiceHardening(SC_HANDLE hService)
{
    bool allSuccess = true;

    // Apply service SID restriction (limits attack surface).
    //
    // SECURITY NOTE: SERVICE_SID_TYPE_RESTRICTED creates a write-restricted token.
    // The system performs TWO access checks for write operations:
    //   1. Normal check against token's enabled SIDs (SYSTEM, service SID, etc.)
    //   2. Restricted check against ONLY the restricting SID list
    // Write access is granted only if BOTH checks pass.
    //
    // The restricting SID list contains:
    //   - NT SERVICE\PanoptesSpectra (per-service SID)
    //   - S-1-1-0 (World/Everyone)
    //   - Service logon SID
    //   - S-1-5-33 (Write-restricted SID)
    //
    // CONSEQUENCE: All writable directories MUST have explicit ACEs for the
    // per-service SID. SYSTEM and Administrators ACEs alone are NOT sufficient
    // for write access. See ApplyDirectoryAcl() and the ACL step in InstallService().
    SERVICE_SID_INFO sidInfo = {};
    sidInfo.dwServiceSidType = SERVICE_SID_TYPE_RESTRICTED;
    if (!ChangeServiceConfig2W(hService, SERVICE_CONFIG_SERVICE_SID_INFO, &sidInfo))
    {
        LogError("[-] WARNING: Failed to set service SID restriction");
        allSuccess = false;
    }
    else
    {
        LogError("[+] Service SID restriction applied (write-restricted token)");
    }

    // Declare required privileges (for transparency and least privilege)
    SERVICE_REQUIRED_PRIVILEGES_INFO privInfo = {};
    privInfo.pmszRequiredPrivileges = const_cast<LPWSTR>(ServiceConfig::REQUIRED_PRIVILEGES);
    if (!ChangeServiceConfig2W(hService, SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO, &privInfo))
    {
        LogError("[-] WARNING: Failed to declare required privileges");
        allSuccess = false;
    }
    else
    {
        LogError("[+] Required privileges declared: SE_BACKUP_NAME, SE_RESTORE_NAME");
    }

    // Configure failure actions (auto-restart on failure)
    SC_ACTION failureActions[3] = {
        { SC_ACTION_RESTART, 60000 },   // Restart after 1 minute
        { SC_ACTION_RESTART, 120000 },  // Restart after 2 minutes
        { SC_ACTION_NONE, 0 }           // Give up after 2 failures
    };

    SERVICE_FAILURE_ACTIONSW sfa = {};
    sfa.dwResetPeriod = 86400;  // Reset failure count after 24 hours
    sfa.lpRebootMsg = nullptr;
    sfa.lpCommand = nullptr;
    sfa.cActions = 3;
    sfa.lpsaActions = failureActions;

    if (!ChangeServiceConfig2W(hService, SERVICE_CONFIG_FAILURE_ACTIONS, &sfa))
    {
        LogError("[-] WARNING: Failed to configure failure actions");
        allSuccess = false;
    }
    else
    {
        LogError("[+] Failure recovery configured (auto-restart)");
    }

    // Set failure actions flag
    SERVICE_FAILURE_ACTIONS_FLAG flag = {};
    flag.fFailureActionsOnNonCrashFailures = TRUE;
    if (!ChangeServiceConfig2W(hService, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &flag))
    {
        LogError("[-] WARNING: Failed to set failure actions flag");
        allSuccess = false;
    }

    return allSuccess;
}

// Create directory structure (without ACLs - those are applied after service registration).
// Directories are created first so logging works during later install steps.
bool ServiceInstaller::CreateSecureDirectories()
{
    const std::vector<std::wstring> directories = {
        ServiceConfig::OUTPUT_DIRECTORY,
        ServiceConfig::LOG_DIRECTORY,
        ServiceConfig::CONFIG_DIRECTORY,
        ServiceConfig::TEMP_DIRECTORY
    };

    bool allSuccess = true;

    for (const auto& dir : directories)
    {
        // Check if directory already exists
        DWORD attribs = GetFileAttributesW(dir.c_str());
        if (attribs != INVALID_FILE_ATTRIBUTES && (attribs & FILE_ATTRIBUTE_DIRECTORY))
        {
            LogError("[+] Directory already exists: " + WideToUtf8(dir));
            continue;
        }

        // Create directory recursively.
        // NOTE: SHCreateDirectoryExW with nullptr SECURITY_ATTRIBUTES inherits parent ACLs.
        // Proper ACLs are applied later via ApplyDirectoryAcl() after service SID registration.
        int result = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        if (result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS)
        {
            LogError("[+] Created directory: " + WideToUtf8(dir));
        }
        else
        {
            LogError("[-] Failed to create directory: " + WideToUtf8(dir) + 
                     ", error: " + std::to_string(result));
            allSuccess = false;
        }
    }

    return allSuccess;
}

// Apply a restrictive DACL to a directory.
//
// Grants:
//   SYSTEM                       - Full Control (with inheritance to children)
//   BUILTIN\Administrators       - Full Control (with inheritance to children)
//   NT SERVICE\PanoptesSpectra   - Modify       (with inheritance to children)
//
// SECURITY DECISIONS:
//   - Service SID gets Modify, NOT Full Control. Full Control would include
//     WRITE_DAC and WRITE_OWNER, allowing a compromised service to change ACLs
//     and escalate access. Modify (read/write/execute/delete) is sufficient for
//     writing logs and JSON files.
//   - PROTECTED_DACL prevents inheriting permissive ACEs from parent directories.
//   - Must be called AFTER CreateServiceW so that the "NT SERVICE\<name>" virtual
//     account can be resolved by SetEntriesInAclW. Calling before registration
//     fails with ERROR_NONE_MAPPED (1332).
bool ServiceInstaller::ApplyDirectoryAcl(const std::wstring& directoryPath)
{
    // Validate input
    if (directoryPath.empty())
    {
        LogError("[-] ApplyDirectoryAcl called with empty path");
        return false;
    }

    // Verify directory exists before applying ACL
    DWORD attribs = GetFileAttributesW(directoryPath.c_str());
    if (attribs == INVALID_FILE_ATTRIBUTES || !(attribs & FILE_ATTRIBUTE_DIRECTORY))
    {
        LogError("[-] Directory does not exist for ACL application: " + WideToUtf8(directoryPath));
        return false;
    }

    // Allocate well-known SIDs using CreateWellKnownSid (consistent with ServiceTamperProtection)
    DWORD cbSystemSid = SECURITY_MAX_SID_SIZE;
    DWORD cbAdminSid = SECURITY_MAX_SID_SIZE;

    LocalUniquePtr<void> pSystemSid(LocalAlloc(LPTR, cbSystemSid));
    LocalUniquePtr<void> pAdminSid(LocalAlloc(LPTR, cbAdminSid));

    if (!pSystemSid || !pAdminSid)
    {
        LogError("[-] Failed to allocate memory for SIDs");
        return false;
    }

    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, pSystemSid.get(), &cbSystemSid))
    {
        LogError("[-] Failed to create SYSTEM SID, error: " + std::to_string(GetLastError()));
        return false;
    }

    if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, pAdminSid.get(), &cbAdminSid))
    {
        LogError("[-] Failed to create Administrators SID, error: " + std::to_string(GetLastError()));
        return false;
    }

    // Build the service trustee name: "NT SERVICE\PanoptesSpectra"
    // SetEntriesInAclW resolves this to the per-service SID at call time.
    std::wstring serviceTrustee = L"NT SERVICE\\";
    serviceTrustee += ServiceConfig::SERVICE_NAME;

    // Build EXPLICIT_ACCESS entries
    EXPLICIT_ACCESSW ea[3] = {};

    // ACE 0: SYSTEM - Full Control, inherited to sub-containers and objects
    ea[0].grfAccessPermissions = FILE_ALL_ACCESS;
    ea[0].grfAccessMode = SET_ACCESS;
    ea[0].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea[0].Trustee.TrusteeType = TRUSTEE_IS_USER;
    ea[0].Trustee.ptstrName = reinterpret_cast<LPWSTR>(pSystemSid.get());

    // ACE 1: Administrators - Full Control, inherited to sub-containers and objects
    ea[1].grfAccessPermissions = FILE_ALL_ACCESS;
    ea[1].grfAccessMode = SET_ACCESS;
    ea[1].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea[1].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea[1].Trustee.TrusteeType = TRUSTEE_IS_GROUP;
    ea[1].Trustee.ptstrName = reinterpret_cast<LPWSTR>(pAdminSid.get());

    // ACE 2: Service SID - Modify (NOT Full Control), inherited to sub-containers and objects
    // Modify = FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE
    // This intentionally excludes WRITE_DAC and WRITE_OWNER to prevent a compromised
    // service from modifying its own directory permissions.
    ea[2].grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE;
    ea[2].grfAccessMode = SET_ACCESS;
    ea[2].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea[2].Trustee.TrusteeForm = TRUSTEE_IS_NAME;
    ea[2].Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
    ea[2].Trustee.ptstrName = const_cast<LPWSTR>(serviceTrustee.c_str());

    // Create the ACL from explicit access entries (no existing ACL to merge with)
    PACL pAcl = nullptr;
    DWORD dwResult = SetEntriesInAclW(3, ea, nullptr, &pAcl);
    if (dwResult != ERROR_SUCCESS)
    {
        LogError("[-] SetEntriesInAclW failed for " + WideToUtf8(directoryPath) +
                 ", error: " + std::to_string(dwResult));
        if (dwResult == 1332) // ERROR_NONE_MAPPED
        {
            LogError("[-] The service SID could not be resolved. Ensure the service is registered with SCM first.");
        }
        return false;
    }

    // RAII guard for the ACL allocated by SetEntriesInAclW (uses LocalAlloc internally)
    LocalUniquePtr<ACL> aclGuard(pAcl);

    // Apply the DACL to the directory.
    // PROTECTED_DACL_SECURITY_INFORMATION prevents inheriting ACEs from parent directories,
    // ensuring our explicit ACL is the sole authority on permissions for this directory tree.
    dwResult = SetNamedSecurityInfoW(
        const_cast<LPWSTR>(directoryPath.c_str()),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr,    // Owner: unchanged
        nullptr,    // Group: unchanged
        pAcl,       // New DACL
        nullptr);   // SACL: unchanged

    if (dwResult != ERROR_SUCCESS)
    {
        LogError("[-] SetNamedSecurityInfoW failed for " + WideToUtf8(directoryPath) +
                 ", error: " + std::to_string(dwResult));
        return false;
    }

    LogError("[+] Applied directory ACL: " + WideToUtf8(directoryPath) +
             " (SYSTEM:F, Administrators:F, " + WideToUtf8(serviceTrustee) + ":M)");
    return true;
}

// Create registry configuration with default values
bool ServiceInstaller::CreateRegistryConfiguration()
{
    HKEY hKey = nullptr;
    DWORD disposition = 0;
    
    // Create or open the registry key
    LONG result = RegCreateKeyExW(
        HKEY_LOCAL_MACHINE,
        ServiceConfig::REGISTRY_KEY,
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_WRITE,
        nullptr,
        &hKey,
        &disposition
    );

    if (result != ERROR_SUCCESS)
    {
        LogError("[-] Failed to create registry key, error: " + std::to_string(result));
        return false;
    }

    if (disposition == REG_CREATED_NEW_KEY)
    {
        LogError("[+] Created registry key: HKLM\\" + WideToUtf8(ServiceConfig::REGISTRY_KEY));
    }
    else
    {
        LogError("[+] Registry key already exists: HKLM\\" + WideToUtf8(ServiceConfig::REGISTRY_KEY));
    }

    bool allSuccess = true;

    // Set default collection interval (24 hours)
    DWORD collectionInterval = ServiceConfig::DEFAULT_COLLECTION_INTERVAL_SECONDS;
    result = RegSetValueExW(hKey, ServiceConfig::REG_COLLECTION_INTERVAL, 0, REG_DWORD, 
                           (const BYTE*)&collectionInterval, sizeof(DWORD));
    if (result != ERROR_SUCCESS)
    {
        LogError("[-] Failed to set CollectionIntervalSeconds, error: " + std::to_string(result));
        allSuccess = false;
    }
    else
    {
        LogError("[+] Set CollectionIntervalSeconds: " + std::to_string(collectionInterval) + 
                 " seconds (" + std::to_string(collectionInterval / 3600) + " hours)");
    }

    // Set output directory
    std::wstring outputDir = ServiceConfig::DEFAULT_OUTPUT_DIRECTORY;
    result = RegSetValueExW(hKey, ServiceConfig::REG_OUTPUT_DIRECTORY, 0, REG_SZ, 
                           (const BYTE*)outputDir.c_str(), 
                           (outputDir.length() + 1) * sizeof(wchar_t));
    if (result != ERROR_SUCCESS)
    {
        LogError("[-] Failed to set OutputDirectory, error: " + std::to_string(result));
        allSuccess = false;
    }
    else
    {
        LogError("[+] Set OutputDirectory: " + WideToUtf8(outputDir));
    }

    // Set detailed logging (disabled by default)
    DWORD enableLogging = 0;
    result = RegSetValueExW(hKey, ServiceConfig::REG_ENABLE_DETAILED_LOGGING, 0, REG_DWORD, 
                           (const BYTE*)&enableLogging, sizeof(DWORD));
    if (result != ERROR_SUCCESS)
    {
        LogError("[-] Failed to set EnableDetailedLogging, error: " + std::to_string(result));
        allSuccess = false;
    }
    else
    {
        LogError("[+] Set EnableDetailedLogging: " + std::to_string(enableLogging));
    }

    // Set server URL (empty by default - for future use)
    std::wstring serverUrl = L"";
    result = RegSetValueExW(hKey, ServiceConfig::REG_SERVER_URL, 0, REG_SZ, 
                           (const BYTE*)serverUrl.c_str(), 
                           (serverUrl.length() + 1) * sizeof(wchar_t));
    if (result != ERROR_SUCCESS)
    {
        LogError("[-] Failed to set ServerUrl, error: " + std::to_string(result));
        allSuccess = false;
    }
    else
    {
        LogError("[+] Set ServerUrl: (empty - not configured)");
    }

    RegCloseKey(hKey);
    
    if (allSuccess)
    {
        LogError("[+] Registry configuration completed successfully");
    }
    
    return allSuccess;
}

// Verify service is installed correctly
bool ServiceInstaller::VerifyInstallation(SC_HANDLE hService)
{
    SERVICE_STATUS ss = {};
    if (!QueryServiceStatus(hService, &ss))
    {
        LogError("[-] Failed to query service status");
        return false;
    }

    if (ss.dwCurrentState == SERVICE_STOPPED)
    {
        LogError("[+] Service is in STOPPED state (expected)");
    }

    // Verify tamper protection
    if (!ServiceTamperProtection::VerifyTamperProtection(hService))
    {
        LogError("[-] Tamper protection verification failed");
        return false;
    }

    return true;
}
