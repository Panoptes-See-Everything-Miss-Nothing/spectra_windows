#include "MachineId.h"
#include "../Utils/Utils.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <rpc.h>

#pragma comment(lib, "rpcrt4.lib")

namespace MachineId
{
    // Generate a new unique Machine ID using Windows GUID
    static std::wstring GenerateNewMachineId()
    {
        UUID uuid = {};
        UuidCreate(&uuid);
        
        // Convert UUID to string
        RPC_WSTR uuidString = nullptr;
        if (UuidToStringW(&uuid, &uuidString) != RPC_S_OK)
        {
            LogError("[-] Failed to convert UUID to string");
            return L"";
        }
        
        // Format: SPECTRA-{GUID}
        std::wstring machineId = MACHINE_ID_PREFIX;
        machineId += reinterpret_cast<wchar_t*>(uuidString);
        
        // Free the UUID string
        RpcStringFreeW(&uuidString);
        
        // Convert to uppercase for consistency
        std::transform(machineId.begin(), machineId.end(), machineId.begin(), ::towupper);
        
        LogError("[+] Generated new Spectra Machine ID: " + WideToUtf8(machineId));
        return machineId;
    }
    
    // Store Machine ID in registry (HKLM for system-wide persistence)
    static bool StoreMachineId(const std::wstring& machineId)
    {
        HKEY hKey = nullptr;
        LONG result = RegCreateKeyExW(
            HKEY_LOCAL_MACHINE,
            MACHINE_ID_REGISTRY_KEY,
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_WRITE,
            nullptr,
            &hKey,
            nullptr
        );
        
        if (result != ERROR_SUCCESS)
        {
            LogError("[-] Failed to create registry key for Machine ID, error: " + std::to_string(result));
            return false;
        }
        
        // Write Machine ID as REG_SZ
        result = RegSetValueExW(
            hKey,
            MACHINE_ID_REGISTRY_VALUE,
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(machineId.c_str()),
            static_cast<DWORD>((machineId.length() + 1) * sizeof(wchar_t))
        );
        
        RegCloseKey(hKey);
        
        if (result != ERROR_SUCCESS)
        {
            LogError("[-] Failed to write Machine ID to registry, error: " + std::to_string(result));
            return false;
        }
        
        LogError("[+] Stored Spectra Machine ID in registry");
        return true;
    }
    
    // Retrieve Machine ID from registry
    static std::wstring RetrieveMachineId()
    {
        HKEY hKey = nullptr;
        LONG result = RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            MACHINE_ID_REGISTRY_KEY,
            0,
            KEY_READ,
            &hKey
        );
        
        if (result != ERROR_SUCCESS)
        {
            return L"";
        }
        
        wchar_t buffer[256] = {};
        DWORD bufferSize = sizeof(buffer);
        DWORD valueType = 0;
        
        result = RegQueryValueExW(
            hKey,
            MACHINE_ID_REGISTRY_VALUE,
            nullptr,
            &valueType,
            reinterpret_cast<BYTE*>(buffer),
            &bufferSize
        );
        
        RegCloseKey(hKey);
        
        if (result != ERROR_SUCCESS || valueType != REG_SZ)
        {
            return L"";
        }
        
        return std::wstring(buffer);
    }
    
    bool ValidateMachineId(const std::wstring& machineId)
    {
        if (machineId.length() != MACHINE_ID_EXPECTED_LENGTH)
        {
            return false;
        }
        
        if (machineId.substr(0, wcslen(MACHINE_ID_PREFIX)) != MACHINE_ID_PREFIX)
        {
            return false;
        }
        
        // Validate GUID portion (basic format check)
        std::wstring guidPart = machineId.substr(wcslen(MACHINE_ID_PREFIX));
        if (guidPart.length() != 36)
        {
            return false;
        }
        
        // Check for hyphens in correct positions (XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX)
        if (guidPart[8] != L'-' || guidPart[13] != L'-' || 
            guidPart[18] != L'-' || guidPart[23] != L'-')
        {
            return false;
        }
        
        return true;
    }
    
    std::wstring GetOrCreateMachineId()
    {
        std::wstring machineId = RetrieveMachineId();
        
        if (!machineId.empty() && ValidateMachineId(machineId))
        {
            LogError("[+] Retrieved existing Spectra Machine ID: " + WideToUtf8(machineId));
            return machineId;
        }
        
        if (!machineId.empty())
        {
            LogError("[!] Warning: Invalid Machine ID found in registry, generating new one");
        }
        
        machineId = GenerateNewMachineId();
        
        if (machineId.empty())
        {
            LogError("[-] FATAL: Failed to generate Machine ID");
            return L"";
        }
        
        if (!StoreMachineId(machineId))
        {
            LogError("[-] Warning: Failed to store Machine ID in registry (will regenerate on next run)");
        }
        
        return machineId;
    }
    
    std::string GetMachineIdUtf8()
    {
        std::wstring machineId = GetOrCreateMachineId();
        return WideToUtf8(machineId);
    }
}
