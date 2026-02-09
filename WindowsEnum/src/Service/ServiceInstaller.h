#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../Utils/Utils.h"

// Service installation and uninstallation
class ServiceInstaller
{
public:
    // Install the service with full security hardening.
    // If the service already exists, automatically performs an in-place upgrade.
    static bool InstallService();
    
    // Uninstall the service and remove all installation artifacts
    static bool UninstallService();
    
    // Upgrade the service in-place: replace binary, re-apply hardening,
    // preserve all state (Machine ID, registry config, directories, ACLs, logs)
    static bool UpgradeService();
    
private:
    // Copy executable to Program Files installation directory
    static std::wstring CopyExecutableToInstallLocation();
    
    // Get the quoted executable path (protects against unquoted service path attacks)
    static std::wstring GetQuotedExecutablePath();
    
    // Apply service hardening (SID restriction, required privileges, failure actions)
    static bool ApplyServiceHardening(SC_HANDLE hService);
    
    // Create secure directory structure
    static bool CreateSecureDirectories();
    
    // Apply a restrictive DACL to a directory granting write access to the service SID.
    // Required because SERVICE_SID_TYPE_RESTRICTED creates a write-restricted token
    // where only SIDs in the restricting list can pass write access checks.
    // Must be called AFTER the service is registered with SCM (so the service SID exists).
    static bool ApplyDirectoryAcl(const std::wstring& directoryPath);
    
    // Apply write ACL to the service's registry configuration key.
    // Required so the restricted service token can write Machine ID and other runtime state
    // to HKLM\SOFTWARE\Panoptes\Spectra. Must be called AFTER CreateServiceW.
    static bool ApplyRegistryKeyAcl();
    
    // Create registry configuration with default values
    static bool CreateRegistryConfiguration();
    
    // Verify service is installed correctly
    static bool VerifyInstallation(SC_HANDLE hService);

    // Remove all Spectra artifacts: executable, registry keys, data directories
    static void RemoveAllArtifacts();

    // Helper: Recursively delete a directory and all its contents
    static bool DeleteDirectoryRecursive(const std::wstring& directoryPath);

    // Helper: Stop a running service and wait for it to fully stop.
    // Returns true if the service is stopped (or was already stopped).
    static bool StopServiceAndWait(SC_HANDLE hService, DWORD timeoutSeconds);

    // Helper: Wait for service process to exit after stop/delete.
    // Ensures the executable file handle is released.
    static void WaitForServiceProcessExit(SC_HANDLE hService, DWORD timeoutSeconds);
};
