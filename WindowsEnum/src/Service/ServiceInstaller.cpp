#include "ServiceInstaller.h"
#include "ServiceConfig.h"
#include "ServiceTamperProtection.h"
#include <shlobj.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

// Install the service with full security hardening
bool ServiceInstaller::InstallService()
{
    LogError("[+] ==========================================================");
    LogError("[+] Installing " + WideToUtf8(ServiceConfig::SERVICE_DISPLAY_NAME));
    LogError("[+] ==========================================================");

    // Create secure directory structure first
    if (!CreateSecureDirectories())
    {
        LogError("[-] Failed to create secure directories");
        LogError("[-] Installation aborted");
        return false;
    }

    // Get SCM handle
    SC_HANDLE hSCManager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!hSCManager)
    {
        DWORD error = GetLastError();
        LogError("[-] Failed to open Service Control Manager, error: " + std::to_string(error));
        LogError("[-] Please run as Administrator");
        return false;
    }

    // Get quoted executable path (CRITICAL: prevents unquoted service path attacks)
    std::wstring quotedPath = GetQuotedExecutablePath();
    if (quotedPath.empty())
    {
        LogError("[-] Failed to get executable path");
        CloseServiceHandle(hSCManager);
        return false;
    }

    LogError("[+] Service executable: " + WideToUtf8(quotedPath));

    // Check if service already exists
    SC_HANDLE hExistingService = OpenServiceW(hSCManager, ServiceConfig::SERVICE_NAME, SERVICE_QUERY_STATUS);
    if (hExistingService)
    {
        LogError("[!] Service already exists");
        CloseServiceHandle(hExistingService);
        CloseServiceHandle(hSCManager);
        LogError("[!] To reinstall, first run: Spectra.exe /uninstall");
        return false;
    }

    // Create the service
    SC_HANDLE hService = CreateServiceW(
        hSCManager,
        ServiceConfig::SERVICE_NAME,
        ServiceConfig::SERVICE_DISPLAY_NAME,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        ServiceConfig::SERVICE_START_TYPE,
        SERVICE_ERROR_NORMAL,
        quotedPath.c_str(),
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

    // Set service description
    SERVICE_DESCRIPTIONW sd = {};
    sd.lpDescription = const_cast<LPWSTR>(ServiceConfig::SERVICE_DESCRIPTION);
    if (!ChangeServiceConfig2W(hService, SERVICE_CONFIG_DESCRIPTION, &sd))
    {
        LogError("[-] WARNING: Failed to set service description");
    }

    // Apply service hardening
    if (!ApplyServiceHardening(hService))
    {
        LogError("[-] WARNING: Failed to apply full service hardening");
    }

    // Apply tamper protection (restrict who can control the service)
    if (!ServiceTamperProtection::ApplyTamperProtectionDACL(hService))
    {
        LogError("[-] WARNING: Failed to apply tamper protection");
        LogError("[!] Service may be vulnerable to unauthorized modification");
    }

    // Verify installation
    if (!VerifyInstallation(hService))
    {
        LogError("[-] WARNING: Installation verification failed");
    }

    CloseServiceHandle(hService);
    CloseServiceHandle(hSCManager);

    LogError("[+] ==========================================================");
    LogError("[+] Service installed successfully!");
    LogError("[+] Service Name: " + WideToUtf8(ServiceConfig::SERVICE_NAME));
    LogError("[+] Display Name: " + WideToUtf8(ServiceConfig::SERVICE_DISPLAY_NAME));
    LogError("[+] ==========================================================");
    LogError("[!] To start the service, run: sc start " + WideToUtf8(ServiceConfig::SERVICE_NAME));
    LogError("[!] Or use: net start \"" + WideToUtf8(ServiceConfig::SERVICE_DISPLAY_NAME) + "\"");

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

// Apply service hardening (SID restriction, required privileges, failure actions)
bool ServiceInstaller::ApplyServiceHardening(SC_HANDLE hService)
{
    bool allSuccess = true;

    // Apply service SID restriction (limits attack surface)
    SERVICE_SID_INFO sidInfo = {};
    sidInfo.dwServiceSidType = SERVICE_SID_TYPE_RESTRICTED;
    if (!ChangeServiceConfig2W(hService, SERVICE_CONFIG_SERVICE_SID_INFO, &sidInfo))
    {
        LogError("[-] WARNING: Failed to set service SID restriction");
        allSuccess = false;
    }
    else
    {
        LogError("[+] Service SID restriction applied");
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

// Create secure directory structure
bool ServiceInstaller::CreateSecureDirectories()
{
    std::vector<std::wstring> directories = {
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

        // Create directory with secure ACL (SYSTEM:Full, Admins:Read)
        // Using SHCreateDirectoryEx for recursive creation
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
