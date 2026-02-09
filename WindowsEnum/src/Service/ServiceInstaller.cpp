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

// Helper: Stop a running service and wait for it to fully stop.
// Returns true if the service is stopped (or was already stopped).
bool ServiceInstaller::StopServiceAndWait(SC_HANDLE hService, DWORD timeoutSeconds)
{
    SERVICE_STATUS ss = {};
    if (!QueryServiceStatus(hService, &ss))
    {
        LogError("[-] Failed to query service status, error: " + std::to_string(GetLastError()));
        return false;
    }

    if (ss.dwCurrentState == SERVICE_STOPPED)
    {
        LogError("[+] Service is already stopped");
        return true;
    }

    LogError("[+] Stopping service...");
    if (!ControlService(hService, SERVICE_CONTROL_STOP, &ss))
    {
        DWORD error = GetLastError();
        if (error == ERROR_SERVICE_NOT_ACTIVE)
        {
            LogError("[+] Service was already stopped");
            return true;
        }
        LogError("[-] Failed to stop service, error: " + std::to_string(error) +
                 " - " + GetWindowsErrorMessage(error));
        return false;
    }

    for (DWORD i = 0; i < timeoutSeconds; i++)
    {
        if (!QueryServiceStatus(hService, &ss))
            break;

        if (ss.dwCurrentState == SERVICE_STOPPED)
        {
            LogError("[+] Service stopped successfully");
            return true;
        }

        Sleep(1000);
    }

    if (ss.dwCurrentState == SERVICE_STOPPED)
        return true;

    LogError("[-] Service did not stop within " + std::to_string(timeoutSeconds) + " seconds");
    return false;
}

// Helper: Wait for service process to exit after stop/delete.
// Ensures the executable file handle is released before file operations.
void ServiceInstaller::WaitForServiceProcessExit(SC_HANDLE hService, DWORD timeoutSeconds)
{
    LogError("[+] Waiting for service process to exit...");
    for (DWORD i = 0; i < timeoutSeconds; i++)
    {
        SERVICE_STATUS_PROCESS ssp = {};
        DWORD bytesNeeded = 0;
        if (!QueryServiceStatusEx(hService, SC_STATUS_PROCESS_INFO,
                                  reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp),
                                  &bytesNeeded))
        {
            break; // Service handle no longer valid, process is gone
        }

        if (ssp.dwCurrentState == SERVICE_STOPPED && ssp.dwProcessId == 0)
        {
            LogError("[+] Service process has exited");
            return;
        }

        Sleep(1000);
    }

    LogError("[!] Timed out waiting for service process to exit");
}

// Install the service with full security hardening.
// If the service already exists, automatically redirects to UpgradeService().
//
// CRITICAL: The service-existence check MUST happen before any side effects
// (file copies, directory creation, registry writes) to avoid destroying
// user-customized configuration on upgrade redirect.
bool ServiceInstaller::InstallService()
{
    LogError("[+] ==========================================================");
    LogError("[+] Installing " + WideToUtf8(ServiceConfig::SERVICE_DISPLAY_NAME));
    LogError("[+] ==========================================================");

    // Step 1: Check if service already exists BEFORE any side effects.
    // If it exists, redirect to UpgradeService() which preserves all state.
    // This MUST be first to prevent CreateRegistryConfiguration() from
    // overwriting user-customized values (collection interval, output dir, etc.)
    {
        SC_HANDLE hSCMCheck = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (hSCMCheck)
        {
            SC_HANDLE hExisting = OpenServiceW(hSCMCheck, ServiceConfig::SERVICE_NAME, SERVICE_QUERY_STATUS);
            if (hExisting)
            {
                LogError("[!] Service already exists - performing in-place upgrade");
                CloseServiceHandle(hExisting);
                CloseServiceHandle(hSCMCheck);
                return UpgradeService();
            }
            CloseServiceHandle(hSCMCheck);
        }
    }

    // Step 2: Copy executable to Program Files
    std::wstring installedExePath = CopyExecutableToInstallLocation();
    if (installedExePath.empty())
    {
        LogError("[-] Failed to copy executable to installation directory");
        LogError("[-] Installation aborted");
        return false;
    }

    LogError("[+] Executable installed to: " + WideToUtf8(installedExePath));

    // Step 3: Create directory structure (directories only, ACLs applied later)
    // ACLs cannot be applied yet because the service SID (NT SERVICE\PanoptesSpectra)
    // does not exist until CreateServiceW registers it with SCM.
    if (!CreateSecureDirectories())
    {
        LogError("[-] Failed to create secure directories");
        LogError("[-] Installation aborted");
        return false;
    }

    // Step 4: Create registry configuration with default values.
    // Safe to call here because Step 1 confirmed the service does not exist,
    // meaning this is a fresh install with no user-customized values to preserve.
    if (!CreateRegistryConfiguration())
    {
        LogError("[-] Failed to create registry configuration");
        LogError("[-] Installation aborted");
        return false;
    }

    // Step 5: Get SCM handle with full access for service creation
    SC_HANDLE hSCManager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!hSCManager)
    {
        DWORD error = GetLastError();
        LogError("[-] Failed to open Service Control Manager, error: " + std::to_string(error));
        LogError("[-] Please run as Administrator");
        return false;
    }

    // Step 6: Create quoted path for installed executable
    std::wstring servicePath = L"\"" + installedExePath + L"\"";
    LogError("[+] Service executable: " + WideToUtf8(servicePath));

    // Step 7: Create the service
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

    // Step 8: Set service description
    SERVICE_DESCRIPTIONW sd = {};
    sd.lpDescription = const_cast<LPWSTR>(ServiceConfig::SERVICE_DESCRIPTION);
    if (!ChangeServiceConfig2W(hService, SERVICE_CONFIG_DESCRIPTION, &sd))
    {
        LogError("[-] WARNING: Failed to set service description");
    }

    // Step 9: Apply service hardening (SID restriction, privileges, failure actions)
    if (!ApplyServiceHardening(hService))
    {
        LogError("[-] WARNING: Failed to apply full service hardening");
    }

    // Step 10: Apply directory ACLs NOW that the service SID exists in SCM.
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

    // Step 11: Apply registry key ACL so the restricted service token can write
    // Machine ID and runtime state to HKLM\SOFTWARE\Panoptes\Spectra.
    if (!ApplyRegistryKeyAcl())
    {
        LogError("[-] WARNING: Failed to apply registry key ACL");
        LogError("[!] Service may fail to persist Machine ID (will regenerate each run)");
    }

    // Step 12: Apply tamper protection (restrict who can control the service)
    if (!ServiceTamperProtection::ApplyTamperProtectionDACL(hService))
    {
        LogError("[-] WARNING: Failed to apply tamper protection");
        LogError("[!] Service may be vulnerable to unauthorized modification");
    }

    // Step 13: Verify installation
    if (!VerifyInstallation(hService))
    {
        LogError("[-] WARNING: Installation verification failed");
    }

    // Step 14: Start the service automatically
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

// Upgrade the service in-place: replace the binary while preserving all state.
//
// This avoids the full uninstall/reinstall cycle, keeping:
//   - Machine ID (HKLM\SOFTWARE\Panoptes\Spectra\SpectraMachineID)
//   - Registry configuration (collection interval, server URL, etc.)
//   - Directory structure and ACLs
//   - Log history
//
// SEQUENCE:
//   1. Verify the service exists (fall back to fresh install if not)
//   2. Remove tamper protection (needed for SERVICE_CHANGE_CONFIG in hardening steps)
//   3. Stop the running service
//   4. Wait for process to exit (release file handle on the .exe)
//   5. Copy new binary over the installed location (same path, new content)
//   6. Re-apply service hardening (SID, privileges, failure actions)
//   7. Re-apply tamper protection
//   8. Start the upgraded service
//
// NOTE: ChangeServiceConfigW is NOT called because the binary path does not change.
// CopyExecutableToInstallLocation() always writes to the same fixed path under
// Program Files, so the SCM's registered ImagePath remains valid.
//
// SECURITY: Requires Administrator privileges. The tamper protection DACL
// blocks SERVICE_CHANGE_CONFIG for everyone except SYSTEM. We open with
// WRITE_DAC first (allowed for admins), remove tamper protection, then
// re-open with the needed access rights for ChangeServiceConfig2W calls
// in ApplyServiceHardening().
//
// SECURITY NOTE: If the process crashes between tamper-protection removal (Step 3)
// and re-application (Step 12), the service is left with a permissive DACL until
// the next successful upgrade or manual re-application. This is acceptable because
// Administrator privileges are already required to reach this code path, and the
// permissive DACL only grants Administrators the same level of control they would
// have on a default Windows service.
bool ServiceInstaller::UpgradeService()
{
    LogError("[+] ==========================================================");
    LogError("[+] Upgrading " + WideToUtf8(ServiceConfig::SERVICE_DISPLAY_NAME));
    LogError("[+] ==========================================================");

    // Step 1: Open SCM
    SC_HANDLE hSCManager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!hSCManager)
    {
        DWORD error = GetLastError();
        LogError("[-] Failed to open Service Control Manager, error: " + std::to_string(error));
        LogError("[-] Please run as Administrator");
        return false;
    }

    // Step 2: Verify the service exists. If not, redirect to fresh install.
    {
        SC_HANDLE hServiceCheck = OpenServiceW(hSCManager, ServiceConfig::SERVICE_NAME, SERVICE_QUERY_STATUS);
        if (!hServiceCheck)
        {
            DWORD error = GetLastError();
            if (error == ERROR_SERVICE_DOES_NOT_EXIST)
            {
                LogError("[!] Service is not installed - performing fresh installation instead");
                CloseServiceHandle(hSCManager);
                return InstallService();
            }
            LogError("[-] Failed to open service, error: " + std::to_string(error) +
                     " - " + GetWindowsErrorMessage(error));
            CloseServiceHandle(hSCManager);
            return false;
        }
        CloseServiceHandle(hServiceCheck);
    }

    // Step 3: Remove tamper protection.
    // The tamper protection DACL does not grant SERVICE_CHANGE_CONFIG to Administrators.
    // ApplyServiceHardening() calls ChangeServiceConfig2W which requires that right.
    // We open with WRITE_DAC (allowed by the DACL) and replace the restrictive DACL.
    SC_HANDLE hServiceDacl = OpenServiceW(hSCManager, ServiceConfig::SERVICE_NAME, WRITE_DAC);
    if (!hServiceDacl)
    {
        DWORD error = GetLastError();
        LogError("[-] Failed to open service for DACL modification, error: " + std::to_string(error) +
                 " - " + GetWindowsErrorMessage(error));
        CloseServiceHandle(hSCManager);
        return false;
    }

    LogError("[+] Removing tamper protection for upgrade...");
    if (!ServiceTamperProtection::RemoveTamperProtection(hServiceDacl))
    {
        LogError("[-] Failed to remove tamper protection - upgrade cannot proceed");
        CloseServiceHandle(hServiceDacl);
        CloseServiceHandle(hSCManager);
        return false;
    }
    CloseServiceHandle(hServiceDacl);

    // Step 4: Re-open with access rights needed to stop, reconfigure, and start.
    // SERVICE_CHANGE_CONFIG is required by ChangeServiceConfig2W in ApplyServiceHardening().
    // WRITE_DAC is required to re-apply tamper protection at the end.
    SC_HANDLE hService = OpenServiceW(hSCManager, ServiceConfig::SERVICE_NAME,
                                       SERVICE_STOP | SERVICE_START |
                                       SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG |
                                       WRITE_DAC);
    if (!hService)
    {
        DWORD error = GetLastError();
        LogError("[-] Failed to open service for upgrade, error: " + std::to_string(error) +
                 " - " + GetWindowsErrorMessage(error));
        CloseServiceHandle(hSCManager);
        return false;
    }

    // Step 5: Stop the running service
    if (!StopServiceAndWait(hService, 30))
    {
        LogError("[-] Failed to stop service - upgrade cannot proceed");
        LogError("[!] Restoring tamper protection before aborting...");
        ServiceTamperProtection::ApplyTamperProtectionDACL(hService);
        CloseServiceHandle(hService);
        CloseServiceHandle(hSCManager);
        return false;
    }

    // Step 6: Wait for the service process to fully exit.
    // The old executable file handle must be released before we can overwrite it.
    WaitForServiceProcessExit(hService, 30);

    // Step 7: Copy the new executable over the installed location.
    // The path does not change - CopyFileW with bFailIfExists=FALSE overwrites in place.
    // The SCM's registered ImagePath remains valid without any ChangeServiceConfigW call.
    std::wstring installedExePath = CopyExecutableToInstallLocation();
    if (installedExePath.empty())
    {
        LogError("[-] Failed to copy new executable to installation directory");
        LogError("[!] Restoring tamper protection before aborting...");
        ServiceTamperProtection::ApplyTamperProtectionDACL(hService);
        CloseServiceHandle(hService);
        CloseServiceHandle(hSCManager);
        return false;
    }

    LogError("[+] New executable installed to: " + WideToUtf8(installedExePath));

    // Step 8: Update the service description (may have changed between versions)
    SERVICE_DESCRIPTIONW sd = {};
    sd.lpDescription = const_cast<LPWSTR>(ServiceConfig::SERVICE_DESCRIPTION);
    if (!ChangeServiceConfig2W(hService, SERVICE_CONFIG_DESCRIPTION, &sd))
    {
        LogError("[!] WARNING: Failed to update service description");
    }

    // Step 9: Re-apply service hardening (SID restriction, privileges, failure actions).
    // A new version may declare different required privileges or change failure behavior.
    if (!ApplyServiceHardening(hService))
    {
        LogError("[!] WARNING: Failed to re-apply full service hardening");
    }

    // Step 10: Ensure directory structure and ACLs are current.
    // A new version may introduce new directories or require updated ACLs.
    if (!CreateSecureDirectories())
    {
        LogError("[!] WARNING: Failed to verify directory structure");
    }

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
                LogError("[!] WARNING: Failed to re-apply ACL to: " + WideToUtf8(dir));
            }
        }
    }

    // Step 11: Re-apply registry key ACL (new version may need updated permissions)
    if (!ApplyRegistryKeyAcl())
    {
        LogError("[!] WARNING: Failed to re-apply registry key ACL");
    }

    // Step 12: Re-apply tamper protection
    if (!ServiceTamperProtection::ApplyTamperProtectionDACL(hService))
    {
        LogError("[!] WARNING: Failed to re-apply tamper protection");
        LogError("[!] Service may be vulnerable to unauthorized modification");
    }

    // Step 13: Start the upgraded service
    LogError("[+] Starting upgraded service...");
    if (!StartServiceW(hService, 0, nullptr))
    {
        DWORD error = GetLastError();
        LogError("[-] WARNING: Failed to start service automatically, error: " + std::to_string(error));
        LogError("[!] You can start it manually with: sc start " + WideToUtf8(ServiceConfig::SERVICE_NAME));
    }
    else
    {
        LogError("[+] Service started successfully!");

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
    LogError("[+] Upgrade completed successfully!");
    LogError("[+] Service Name: " + WideToUtf8(ServiceConfig::SERVICE_NAME));
    LogError("[+] Display Name: " + WideToUtf8(ServiceConfig::SERVICE_DISPLAY_NAME));
    LogError("[+] Binary:       " + WideToUtf8(installedExePath));
    LogError("[+] All state preserved (Machine ID, config, logs, ACLs)");
    LogError("[+] ==========================================================");

    return true;
}

// Uninstall the service and remove all installation artifacts.
//
// SECURITY: Requires Administrator privileges. Three barriers prevent non-admin uninstall:
//   1. OpenSCManagerW with SC_MANAGER_ALL_ACCESS requires admin elevation
//   2. OpenServiceW with WRITE_DAC requires admin (Users only get SERVICE_QUERY_STATUS)
//   3. Artifact cleanup writes to HKLM, Program Files, and ProgramData (admin-only)
//
// The tamper protection DACL does not grant DELETE to Administrators. To work around
// this, we open with WRITE_DAC first (allowed for admins), remove the restrictive DACL
// via RemoveTamperProtection(), then re-open with DELETE to perform the actual deletion.
bool ServiceInstaller::UninstallService()
{
    // SECURITY: Only allow uninstallation from the installed executable location.
    // This prevents an attacker from copying the binary elsewhere and running /uninstall
    // from an uncontrolled path. The installed path is protected by Program Files ACLs.
    //
    // We query SCM for the registered binary path rather than hardcoding it, so this
    // works correctly for both x64 (C:\Program Files\...) and x86 builds running under
    // WOW64 (C:\Program Files (x86)\...).
    {
        WCHAR currentExePath[MAX_PATH] = {};
        DWORD pathLen = GetModuleFileNameW(nullptr, currentExePath, MAX_PATH);
        if (pathLen == 0 || pathLen >= MAX_PATH)
        {
            LogError("[-] Failed to determine current executable path");
            return false;
        }

        // Query SCM for the registered service binary path
        std::wstring registeredPath;
        SC_HANDLE hSCMQuery = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (hSCMQuery)
        {
            SC_HANDLE hSvcQuery = OpenServiceW(hSCMQuery, ServiceConfig::SERVICE_NAME, SERVICE_QUERY_CONFIG);
            if (hSvcQuery)
            {
                // First call to get required buffer size
                DWORD bytesNeeded = 0;
                QueryServiceConfigW(hSvcQuery, nullptr, 0, &bytesNeeded);
                if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && bytesNeeded > 0)
                {
                    std::vector<BYTE> buffer(bytesNeeded);
                    auto pConfig = reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(buffer.data());
                    if (QueryServiceConfigW(hSvcQuery, pConfig, bytesNeeded, &bytesNeeded))
                    {
                        // SCM stores the path quoted: "C:\Program Files\...\Spectra.exe"
                        // Strip surrounding quotes for comparison
                        registeredPath = pConfig->lpBinaryPathName;
                        if (registeredPath.size() >= 2 &&
                            registeredPath.front() == L'"' && registeredPath.back() == L'"')
                        {
                            registeredPath = registeredPath.substr(1, registeredPath.size() - 2);
                        }
                    }
                }
                CloseServiceHandle(hSvcQuery);
            }
            CloseServiceHandle(hSCMQuery);
        }

        // If the service is registered, enforce path check.
        // If the service doesn't exist (already deleted), allow the run for artifact cleanup.
        if (!registeredPath.empty())
        {
            if (_wcsicmp(currentExePath, registeredPath.c_str()) != 0)
            {
                LogError("[-] ==========================================================");
                LogError("[-] Uninstallation denied: must run from the installed location");
                LogError("[-] Expected: " + WideToUtf8(registeredPath));
                LogError("[-] Actual:   " + WideToUtf8(currentExePath));
                LogError("[-] ==========================================================");
                LogError("[!] Run: \"" + WideToUtf8(registeredPath) + "\" /uninstall");
                return false;
            }
        }
    }

    LogError("[+] ==========================================================");
    LogError("[+] Uninstalling " + WideToUtf8(ServiceConfig::SERVICE_DISPLAY_NAME));
    LogError("[+] ==========================================================");

    bool serviceRemoved = false;

    SC_HANDLE hSCManager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!hSCManager)
    {
        DWORD error = GetLastError();
        LogError("[-] Failed to open Service Control Manager, error: " + std::to_string(error) +
                 " - " + GetWindowsErrorMessage(error));
        LogError("[-] Please run as Administrator");
        return false;
    }

    // Phase 1: Remove tamper protection FIRST.
    // The DENY ACE on DELETE|SERVICE_CHANGE_CONFIG prevents OpenServiceW with DELETE.
    // Administrators ARE allowed WRITE_DAC (not blocked by the DENY), so we open
    // with that access right to replace the restrictive DACL before attempting deletion.
    SC_HANDLE hServiceDacl = OpenServiceW(hSCManager, ServiceConfig::SERVICE_NAME, WRITE_DAC);
    if (!hServiceDacl)
    {
        DWORD error = GetLastError();
        if (error == ERROR_SERVICE_DOES_NOT_EXIST)
        {
            LogError("[!] Service is not registered with SCM - skipping service removal");
            CloseServiceHandle(hSCManager);
            // Still clean up files, directories, and registry
            RemoveAllArtifacts();
            LogError("[+] ==========================================================");
            LogError("[+] Artifact cleanup completed.");
            LogError("[+] ==========================================================");
            return true;
        }
        LogError("[-] Failed to open service for DACL modification, error: " + std::to_string(error) +
                 " - " + GetWindowsErrorMessage(error));
        CloseServiceHandle(hSCManager);
        return false;
    }

    LogError("[+] Removing tamper protection...");
    if (!ServiceTamperProtection::RemoveTamperProtection(hServiceDacl))
    {
        LogError("[-] WARNING: Failed to remove tamper protection - deletion may fail");
    }
    CloseServiceHandle(hServiceDacl);

    // Phase 2: Re-open with the access rights needed to stop and delete.
    // Now that tamper protection is removed, DELETE is permitted for Administrators.
    {
        SC_HANDLE hService = OpenServiceW(hSCManager, ServiceConfig::SERVICE_NAME,
                                           SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
        if (!hService)
        {
            DWORD error = GetLastError();
            LogError("[-] Failed to open service for deletion, error: " + std::to_string(error) +
                     " - " + GetWindowsErrorMessage(error));
            CloseServiceHandle(hSCManager);
            return false;
        }

        // Stop the service if it's running
        if (!StopServiceAndWait(hService, 30))
        {
            LogError("[!] WARNING: Service may not have stopped cleanly");
        }

        // Delete the service from SCM.
        // This also removes HKLM\SYSTEM\CurrentControlSet\Services\PanoptesSpectra
        // (SCM owns that key and deletes it automatically).
        if (DeleteService(hService))
        {
            LogError("[+] Service deleted from Service Control Manager");
            serviceRemoved = true;
        }
        else
        {
            DWORD error = GetLastError();
            LogError("[-] DeleteService failed, error: " + std::to_string(error) +
                     " - " + GetWindowsErrorMessage(error));
        }

        // Wait for the service process to fully exit.
        if (serviceRemoved)
        {
            WaitForServiceProcessExit(hService, 30);
        }

        CloseServiceHandle(hService);
    }

    CloseServiceHandle(hSCManager);

    if (!serviceRemoved)
    {
        LogError("[-] Service could not be deleted - aborting artifact cleanup");
        return false;
    }

    // Phase 3: Remove all Spectra artifacts from the system.
    // Log final messages BEFORE artifact removal, because RemoveAllArtifacts() deletes
    // the ProgramData log directory as its last step — no LogError() calls after that.
    LogError("[+] ==========================================================");
    LogError("[+] Removing all Panoptes Spectra artifacts...");
    LogError("[!] If any files are locked, they will be removed on next reboot.");
    LogError("[+] ==========================================================");

    RemoveAllArtifacts();

    // NOTE: Do NOT call LogError() here — the log directory has been deleted.

    return true;
}

// Remove all Spectra artifacts: installed executable, registry keys, data directories.
// Called after the service has been deleted from SCM.
//
// Artifacts removed:
//   - C:\Program Files\Panoptes\          (installed executable and directory tree)
//   - C:\ProgramData\Panoptes\            (output, logs, config, temp data)
//   - HKLM\SOFTWARE\Panoptes\             (Machine ID, configuration values)
//
// ORDERING: ProgramData is deleted LAST because the uninstaller's own LogError() calls
// write to C:\ProgramData\Panoptes\Spectra\Logs\spectra_log.txt. If we delete that
// directory first, subsequent LogError() calls recreate the file, causing RemoveDirectoryW
// to fail with ERROR_DIR_NOT_EMPTY. By deleting ProgramData last and avoiding LogError()
// calls after it, we ensure clean removal.
//
// Note: HKLM\SYSTEM\CurrentControlSet\Services\PanoptesSpectra is automatically
// removed by SCM when DeleteService() succeeds - we do NOT touch it manually.
void ServiceInstaller::RemoveAllArtifacts()
{
    LogError("[+] Removing Spectra artifacts...");

    // 1. Remove installed executable and Program Files directory tree
    {
        std::wstring installDir = L"C:\\Program Files\\Panoptes";
        if (DeleteDirectoryRecursive(installDir))
        {
            LogError("[+] Removed installation directory: " + WideToUtf8(installDir));
        }
        else
        {
            DWORD attribs = GetFileAttributesW(installDir.c_str());
            if (attribs == INVALID_FILE_ATTRIBUTES)
            {
                LogError("[!] Installation directory not found (already removed)");
            }
            else
            {
                LogError("[-] WARNING: Could not fully remove: " + WideToUtf8(installDir));
                LogError("[!] Locked files scheduled for deletion on reboot.");
            }
        }
    }

    // 2. Remove application registry key: HKLM\SOFTWARE\Panoptes
    {
        LONG result = RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Panoptes");
        if (result == ERROR_SUCCESS)
        {
            // RegDeleteTreeW deletes all subkeys and values but not the key itself
            RegDeleteKeyW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Panoptes");
            LogError("[+] Removed registry key: HKLM\\SOFTWARE\\Panoptes");
        }
        else if (result == ERROR_FILE_NOT_FOUND)
        {
            LogError("[!] Registry key HKLM\\SOFTWARE\\Panoptes not found (already removed)");
        }
        else
        {
            LogError("[-] WARNING: Failed to delete HKLM\\SOFTWARE\\Panoptes, error: " +
                     std::to_string(result) + " - " + GetWindowsErrorMessage(result));
        }
    }

    // 3. Remove runtime data directory tree: C:\ProgramData\Panoptes
    //
    // IMPORTANT: This MUST be the last cleanup step. LogError() writes to
    // C:\ProgramData\Panoptes\Spectra\Logs\spectra_log.txt on every call.
    // After deleting this directory, do NOT call LogError() because it would
    // recreate the log file (LogError opens/writes/closes per call). The final
    // success/failure messages are logged by the caller BEFORE this point.
    LogError("[+] Removing data directory (final step - no further log entries)...");
    {
        std::wstring dataDir = L"C:\\ProgramData\\Panoptes";
        if (!DeleteDirectoryRecursive(dataDir))
        {
            DWORD attribs = GetFileAttributesW(dataDir.c_str());
            if (attribs != INVALID_FILE_ATTRIBUTES)
            {
                // Cannot call LogError here - it would recreate files in the directory.
                // Schedule the entire tree for reboot deletion as a fallback.
                MoveFileExW(dataDir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            }
        }
    }
}

// Recursively delete a directory and all its contents (files and subdirectories).
// Returns true if the directory was fully removed, false if any part remains.
//
// SECURITY: Only operates on paths that are actual directories (not reparse points).
// Validates each path before deletion to prevent symlink-based attacks.
bool ServiceInstaller::DeleteDirectoryRecursive(const std::wstring& directoryPath)
{
    if (directoryPath.empty())
        return false;

    DWORD attribs = GetFileAttributesW(directoryPath.c_str());
    if (attribs == INVALID_FILE_ATTRIBUTES)
        return true; // Already gone

    if (!(attribs & FILE_ATTRIBUTE_DIRECTORY))
    {
        LogError("[-] Path is not a directory: " + WideToUtf8(directoryPath));
        return false;
    }

    // SECURITY: Do not follow reparse points (symlinks/junctions) to prevent
    // an attacker from tricking the uninstaller into deleting unrelated files.
    if (attribs & FILE_ATTRIBUTE_REPARSE_POINT)
    {
        LogError("[-] SECURITY: Refusing to delete reparse point: " + WideToUtf8(directoryPath));
        return false;
    }

    bool allSuccess = true;
    std::wstring searchPattern = directoryPath + L"\\*";
    WIN32_FIND_DATAW findData = {};
    HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &findData);

    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            // Skip . and ..
            if (wcscmp(findData.cFileName, L".") == 0 || wcscmp(findData.cFileName, L"..") == 0)
                continue;

            std::wstring childPath = directoryPath + L"\\" + findData.cFileName;

            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                // Skip reparse points in subdirectories too
                if (findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                {
                    LogError("[-] SECURITY: Skipping reparse point: " + WideToUtf8(childPath));
                    allSuccess = false;
                    continue;
                }

                // Recurse into subdirectory
                if (!DeleteDirectoryRecursive(childPath))
                {
                    allSuccess = false;
                }
            }
            else
            {
                // Remove read-only attribute if set (common on installed executables)
                if (findData.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
                {
                    SetFileAttributesW(childPath.c_str(),
                                       findData.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
                }

                if (!DeleteFileW(childPath.c_str()))
                {
                    DWORD deleteError = GetLastError();
                    // If the file is locked (e.g., the running uninstaller executable),
                    // schedule it for deletion on next reboot.
                    if (deleteError == ERROR_ACCESS_DENIED || deleteError == ERROR_SHARING_VIOLATION)
                    {
                        if (MoveFileExW(childPath.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT))
                        {
                            LogError("[!] File locked, scheduled for deletion on reboot: " + WideToUtf8(childPath));
                        }
                        else
                        {
                            LogError("[-] Failed to schedule reboot delete: " + WideToUtf8(childPath) +
                                     ", error: " + std::to_string(GetLastError()));
                            allSuccess = false;
                        }
                    }
                    else
                    {
                        LogError("[-] Failed to delete file: " + WideToUtf8(childPath) +
                                 ", error: " + std::to_string(deleteError));
                        allSuccess = false;
                    }
                }
            }
        } while (FindNextFileW(hFind, &findData));

        FindClose(hFind);
    }

    // Remove the now-empty directory
    if (!RemoveDirectoryW(directoryPath.c_str()))
    {
        DWORD dirError = GetLastError();
        // Directory may not be empty if it contains files scheduled for reboot deletion.
        // Schedule the directory itself for reboot deletion too.
        if (dirError == ERROR_DIR_NOT_EMPTY || dirError == ERROR_ACCESS_DENIED)
        {
            if (MoveFileExW(directoryPath.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT))
            {
                LogError("[!] Directory not empty (locked files), scheduled for deletion on reboot: " +
                         WideToUtf8(directoryPath));
            }
            else
            {
                LogError("[-] Failed to schedule reboot delete for directory: " + WideToUtf8(directoryPath) +
                         ", error: " + std::to_string(GetLastError()));
                allSuccess = false;
            }
        }
        else
        {
            LogError("[-] Failed to remove directory: " + WideToUtf8(directoryPath) +
                     ", error: " + std::to_string(dirError));
            allSuccess = false;
        }
    }

    return allSuccess;
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

// Copy executable to Program Files installation directory.
// If the source and target are the same file (running from installed location),
// the copy is skipped to avoid undefined self-copy behavior.
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

    // Skip copy if source and target are the same file (running from installed location).
    // CopyFileW behavior on self-copy is undefined on some filesystems.
    // Case-insensitive comparison because NTFS paths are case-insensitive.
    if (_wcsicmp(currentExePath, targetPath.c_str()) == 0)
    {
        LogError("[+] Already running from installed location - skipping copy");
        return targetPath;
    }

    // Copy executable (bFailIfExists=FALSE overwrites existing file)
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
    // PROTECTED_DACL_SECURITY_INFORMATION prevents inheriting ACEs from parent directories.
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

// Apply a DACL to the service's registry configuration key so the restricted
// service token can write Machine ID and other runtime state.
//
// Grants (merged with existing DACL to preserve default HKLM permissions):
//   NT SERVICE\PanoptesSpectra - KEY_READ | KEY_WRITE (on this key + subkeys)
//
// SECURITY DECISIONS:
//   - Merges with existing DACL rather than replacing, so SYSTEM and Administrators
//     retain their default HKLM permissions.
//   - Service SID gets KEY_READ | KEY_WRITE, NOT KEY_ALL_ACCESS.
//   - Uses SUB_CONTAINERS_AND_OBJECTS_INHERIT so subkeys inherit the service SID ACE.
//   - Must be called AFTER CreateServiceW so the service SID exists.
bool ServiceInstaller::ApplyRegistryKeyAcl()
{
    // Build the service trustee name
    std::wstring serviceTrustee = L"NT SERVICE\\";
    serviceTrustee += ServiceConfig::SERVICE_NAME;

    // Create a single EXPLICIT_ACCESS entry for the service SID.
    EXPLICIT_ACCESSW ea = {};
    ea.grfAccessPermissions = KEY_READ | KEY_WRITE;
    ea.grfAccessMode = GRANT_ACCESS;
    ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_NAME;
    ea.Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
    ea.Trustee.ptstrName = const_cast<LPWSTR>(serviceTrustee.c_str());

    // Open the registry key to read its current DACL
    HKEY hKey = nullptr;
    LONG regResult = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        ServiceConfig::REGISTRY_KEY,
        0,
        READ_CONTROL | WRITE_DAC,
        &hKey);

    if (regResult != ERROR_SUCCESS)
    {
        LogError("[-] Failed to open registry key for ACL modification, error: " + std::to_string(regResult));
        return false;
    }

    // Get the existing DACL
    PSECURITY_DESCRIPTOR pSD = nullptr;
    PACL pExistingDacl = nullptr;

    DWORD dwResult = GetSecurityInfo(
        hKey,
        SE_REGISTRY_KEY,
        DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        &pExistingDacl,
        nullptr,
        &pSD);

    if (dwResult != ERROR_SUCCESS)
    {
        LogError("[-] Failed to get existing registry key DACL, error: " + std::to_string(dwResult));
        RegCloseKey(hKey);
        return false;
    }

    // Merge the new ACE into the existing DACL
    PACL pNewDacl = nullptr;
    dwResult = SetEntriesInAclW(1, &ea, pExistingDacl, &pNewDacl);

    // Free the security descriptor returned by GetSecurityInfo (allocated via LocalAlloc)
    if (pSD)
    {
        LocalFree(pSD);
        pSD = nullptr;
    }

    if (dwResult != ERROR_SUCCESS)
    {
        LogError("[-] SetEntriesInAclW failed for registry key, error: " + std::to_string(dwResult));
        if (dwResult == 1332) // ERROR_NONE_MAPPED
        {
            LogError("[-] Service SID could not be resolved. Ensure service is registered with SCM first.");
        }
        RegCloseKey(hKey);
        return false;
    }

    // RAII guard for the merged ACL
    LocalUniquePtr<ACL> aclGuard(pNewDacl);

    // Apply the merged DACL back to the registry key
    dwResult = SetSecurityInfo(
        hKey,
        SE_REGISTRY_KEY,
        DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        pNewDacl,
        nullptr);

    RegCloseKey(hKey);

    if (dwResult != ERROR_SUCCESS)
    {
        LogError("[-] SetSecurityInfo failed for registry key, error: " + std::to_string(dwResult));
        return false;
    }

    LogError("[+] Applied registry key ACL: HKLM\\" + WideToUtf8(ServiceConfig::REGISTRY_KEY) +
             " (" + WideToUtf8(serviceTrustee) + ":RW)");
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
                           reinterpret_cast<const BYTE*>(&collectionInterval), sizeof(DWORD));
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
                           reinterpret_cast<const BYTE*>(outputDir.c_str()),
                           static_cast<DWORD>((outputDir.length() + 1) * sizeof(wchar_t)));
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
                           reinterpret_cast<const BYTE*>(&enableLogging), sizeof(DWORD));
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
                           reinterpret_cast<const BYTE*>(serverUrl.c_str()),
                           static_cast<DWORD>((serverUrl.length() + 1) * sizeof(wchar_t)));
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
