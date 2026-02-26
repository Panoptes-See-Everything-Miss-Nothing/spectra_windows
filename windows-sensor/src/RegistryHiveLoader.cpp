#include "RegistryHiveLoader.h"
#include "PrivMgmt.h"
#include <sstream>

RegistryHiveLoader::RegistryHiveLoader(const std::wstring& hivePath, const std::wstring& keyName)
    : m_keyName(keyName), m_loaded(false), m_privilegesEnabled(false)
{
    // Enable required privileges
    if (!EnableRequiredPrivileges())
    {
        LogError("[-] Failed to enable privileges for loading registry hive");
        return;
    }

    m_privilegesEnabled = true;

    // Set registry flags for read-only access
    // REG_PROCESS_APPKEY allows loading read-only hives (Windows 8+)
    DWORD flags = 0; // For compatibility, we'll use default flags
    
    // Load the hive
    LONG result = RegLoadKeyW(HKEY_USERS, m_keyName.c_str(), hivePath.c_str());
    
    if (result != ERROR_SUCCESS)
    {
        LogError("[-] Failed to load registry hive from '" + WideToUtf8(hivePath) + 
                 "' as key '" + WideToUtf8(m_keyName) + "', error: " + 
                 std::to_string(result) + " - " + GetWindowsErrorMessage(result));
        DisableRequiredPrivileges();
        m_privilegesEnabled = false;
        return;
    }

    m_loaded = true;
    LogError("[+] Successfully loaded registry hive: " + WideToUtf8(m_keyName));
}

RegistryHiveLoader::~RegistryHiveLoader()
{
    if (m_loaded)
    {
        // Unload the hive
        LONG result = RegUnLoadKeyW(HKEY_USERS, m_keyName.c_str());
        
        if (result != ERROR_SUCCESS)
        {
            LogError("[-] Warning: Failed to unload registry hive '" + WideToUtf8(m_keyName) + 
                     "', error: " + std::to_string(result) + " - " + GetWindowsErrorMessage(result));
        }
        else
        {
            LogError("[+] Successfully unloaded registry hive: " + WideToUtf8(m_keyName));
        }
    }

    if (m_privilegesEnabled)
    {
        DisableRequiredPrivileges();
    }
}

bool RegistryHiveLoader::EnableRequiredPrivileges()
{
    bool restoreEnabled = EnablePrivilege(SE_RESTORE_NAME);
    bool backupEnabled = EnablePrivilege(SE_BACKUP_NAME);

    if (!restoreEnabled || !backupEnabled)
    {
        LogError("[-] Failed to enable required privileges (SE_RESTORE_NAME and/or SE_BACKUP_NAME)");
        return false;
    }

    return true;
}

void RegistryHiveLoader::DisableRequiredPrivileges()
{
    DisablePrivilege(SE_RESTORE_NAME);
    DisablePrivilege(SE_BACKUP_NAME);
}
