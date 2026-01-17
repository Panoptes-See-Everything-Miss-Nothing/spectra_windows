#include "ServiceConfig.h"
#include "../Utils/Utils.h"
#include <string>

namespace ServiceConfig
{
    // Read DWORD from registry with default fallback
    static DWORD ReadRegistryDword(const wchar_t* valueName, DWORD defaultValue)
    {
        HKEY hKey = nullptr;
        LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, REGISTRY_KEY, 0, KEY_READ, &hKey);
        
        if (result != ERROR_SUCCESS)
        {
            return defaultValue;
        }
        
        DWORD value = 0;
        DWORD valueSize = sizeof(DWORD);
        DWORD valueType = 0;
        
        result = RegQueryValueExW(hKey, valueName, nullptr, &valueType, 
                                  reinterpret_cast<BYTE*>(&value), &valueSize);
        
        RegCloseKey(hKey);
        
        if (result != ERROR_SUCCESS || valueType != REG_DWORD)
        {
            return defaultValue;
        }
        
        return value;
    }
    
    // Read string from registry with default fallback
    static std::wstring ReadRegistryString(const wchar_t* valueName, const wchar_t* defaultValue)
    {
        HKEY hKey = nullptr;
        LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, REGISTRY_KEY, 0, KEY_READ, &hKey);
        
        if (result != ERROR_SUCCESS)
        {
            return defaultValue;
        }
        
        wchar_t buffer[1024] = {};
        DWORD bufferSize = sizeof(buffer);
        DWORD valueType = 0;
        
        result = RegQueryValueExW(hKey, valueName, nullptr, &valueType,
                                  reinterpret_cast<BYTE*>(buffer), &bufferSize);
        
        RegCloseKey(hKey);
        
        if (result != ERROR_SUCCESS || valueType != REG_SZ)
        {
            return defaultValue;
        }
        
        return std::wstring(buffer);
    }
    
    DWORD GetCollectionIntervalSeconds()
    {
        DWORD interval = ReadRegistryDword(REG_COLLECTION_INTERVAL, DEFAULT_COLLECTION_INTERVAL_SECONDS);
        
        // Validate: minimum 60 seconds, maximum 24 hours
        if (interval < 60)
        {
            LogError("[!] Warning: Collection interval too short (" + std::to_string(interval) + 
                     "s), using minimum 60s");
            return 60;
        }
        
        if (interval > 86400)
        {
            LogError("[!] Warning: Collection interval too long (" + std::to_string(interval) + 
                     "s), using maximum 86400s (24 hours)");
            return 86400;
        }
        
        return interval;
    }
    
    std::wstring GetOutputDirectory()
    {
        return ReadRegistryString(REG_OUTPUT_DIRECTORY, DEFAULT_OUTPUT_DIRECTORY);
    }
    
    std::wstring GetServerUrl()
    {
        return ReadRegistryString(REG_SERVER_URL, L"");
    }
    
    bool IsDetailedLoggingEnabled()
    {
        return ReadRegistryDword(REG_ENABLE_DETAILED_LOGGING, 0) != 0;
    }
}
