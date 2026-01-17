#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../Utils/Utils.h"

// Service installation and uninstallation
class ServiceInstaller
{
public:
    // Install the service with full security hardening
    static bool InstallService();
    
    // Uninstall the service cleanly
    static bool UninstallService();
    
private:
    // Copy executable to Program Files installation directory
    static std::wstring CopyExecutableToInstallLocation();
    
    // Get the quoted executable path (protects against unquoted service path attacks)
    static std::wstring GetQuotedExecutablePath();
    
    // Apply service hardening (SID restriction, required privileges, failure actions)
    static bool ApplyServiceHardening(SC_HANDLE hService);
    
    // Create secure directory structure
    static bool CreateSecureDirectories();
    
    // Verify service is installed correctly
    static bool VerifyInstallation(SC_HANDLE hService);
};
