#pragma once

#include "../Utils/Utils.h"
#include <windows.h>
#include <string>

// RAII wrapper for loading and unloading registry hives
class RegistryHiveLoader
{
public:
    // Load a registry hive from a file (read-only)
    RegistryHiveLoader(const std::wstring& hivePath, const std::wstring& keyName);
    
    ~RegistryHiveLoader();

    // Delete copy constructor and assignment operator
    RegistryHiveLoader(const RegistryHiveLoader&) = delete;
    RegistryHiveLoader& operator=(const RegistryHiveLoader&) = delete;

    // Check if hive was successfully loaded
    bool IsLoaded() const { return m_loaded; }

    // Get the full registry path to access the loaded hive
    std::wstring GetRegistryPath() const { return L"HKEY_USERS\\" + m_keyName; }

    // Get just the key name under HKEY_USERS
    std::wstring GetKeyName() const { return m_keyName; }

private:
    std::wstring m_keyName;
    bool m_loaded;
    bool m_privilegesEnabled;

    bool EnableRequiredPrivileges();
    void DisableRequiredPrivileges();
};
