#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>

// Represents a single Windows service installed on the system.
// Collected via the Service Control Manager (SCM) APIs.
struct WindowsServiceInfo
{
    std::wstring serviceName;           // Internal service name (e.g., "wuauserv")
    std::wstring displayName;           // Human-readable display name (e.g., "Windows Update")
    std::wstring description;           // Service description text
    std::wstring binaryPathName;        // Path to the service executable (may include arguments)
    std::wstring serviceStartName;      // Account under which the service runs (e.g., "LocalSystem")
    std::wstring startType;             // "Auto", "DelayedAuto", "Manual", "Disabled", "Boot", "System"
    std::wstring currentState;          // "Running", "Stopped", "StartPending", "StopPending", etc.
    std::wstring serviceType;           // "Win32OwnProcess", "Win32ShareProcess", "KernelDriver", etc.
    DWORD processId = 0;                // PID of the running service process (0 if not running)
    bool isRunning = false;             // Convenience flag: true if currentState is "Running"
};

// Enumerate all installed Win32 services and their current status.
// Uses SCM APIs: OpenSCManagerW, EnumServicesStatusExW, QueryServiceConfigW, QueryServiceConfig2W.
// Privilege requirement: SC_MANAGER_ENUMERATE_SERVICE and SERVICE_QUERY_CONFIG (no elevation needed).
// Returns an empty vector on failure (errors are logged internally).
std::vector<WindowsServiceInfo> EnumerateWindowsServices();
