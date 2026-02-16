#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../Utils/Utils.h"
#include "../ProcessTracker.h"

// Windows Service main entry point and control handlers
class ServiceMain
{
public:
    // Service entry point (called by SCM)
    static VOID WINAPI ServiceMainEntry(DWORD argc, LPWSTR* argv);
    
    // Service control handler (handles stop, pause, interrogate, etc.)
    static DWORD WINAPI ServiceControlHandler(DWORD dwControl, DWORD dwEventType, 
                                               LPVOID lpEventData, LPVOID lpContext);
    
    // Run the service
    static bool RunService();
    
    // Access the process tracker instance (used by GenerateJSON for snapshot data)
    static ProcessTracker* GetProcessTracker();

private:
    // Service worker thread
    static DWORD WINAPI ServiceWorkerThread(LPVOID lpParam);
    
    // Report service status to SCM
    static void ReportServiceStatus(DWORD dwCurrentState, DWORD dwWin32ExitCode, DWORD dwWaitHint);
    
    // Perform data collection
    static void PerformDataCollection();
    
    // Service status handle
    static SERVICE_STATUS_HANDLE g_hServiceStatusHandle;
    
    // Service status
    static SERVICE_STATUS g_serviceStatus;
    
    // Stop event
    static HANDLE g_hStopEvent;
    
    // Worker thread handle
    static HANDLE g_hWorkerThread;
    
    // Real-time process tracker (ETW-based with Sysmon fallback)
    static ProcessTracker g_processTracker;
};
