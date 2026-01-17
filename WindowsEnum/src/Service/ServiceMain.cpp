#include "ServiceMain.h"
#include "ServiceConfig.h"
#include "../WindowsEnum.h"
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>

#pragma comment(lib, "advapi32.lib")

// Static member initialization
SERVICE_STATUS_HANDLE ServiceMain::g_hServiceStatusHandle = nullptr;
SERVICE_STATUS ServiceMain::g_serviceStatus = {};
HANDLE ServiceMain::g_hStopEvent = nullptr;
HANDLE ServiceMain::g_hWorkerThread = nullptr;

// Run the service (called from main)
bool ServiceMain::RunService()
{
    LogError("[+] Starting " + WideToUtf8(ServiceConfig::SERVICE_DISPLAY_NAME) + " as Windows Service");

    SERVICE_TABLE_ENTRYW serviceTable[] = {
        { const_cast<LPWSTR>(ServiceConfig::SERVICE_NAME), ServiceMainEntry },
        { nullptr, nullptr }
    };

    if (!StartServiceCtrlDispatcherW(serviceTable))
    {
        DWORD error = GetLastError();
        LogError("[-] StartServiceCtrlDispatcher failed, error: " + std::to_string(error));
        LogError("[-] This executable must be launched by the Service Control Manager");
        LogError("[-] To run in console mode, use: Spectra.exe /console");
        return false;
    }

    return true;
}

// Service main entry point (called by SCM)
VOID WINAPI ServiceMain::ServiceMainEntry(DWORD argc, LPWSTR* argv)
{
    LogError("[+] ServiceMain entry point called by SCM");

    // Register the service control handler
    g_hServiceStatusHandle = RegisterServiceCtrlHandlerExW(
        ServiceConfig::SERVICE_NAME,
        ServiceControlHandler,
        nullptr
    );

    if (!g_hServiceStatusHandle)
    {
        LogError("[-] RegisterServiceCtrlHandlerEx failed");
        return;
    }

    // Initialize service status
    ZeroMemory(&g_serviceStatus, sizeof(g_serviceStatus));
    g_serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_serviceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_serviceStatus.dwCurrentState = SERVICE_START_PENDING;

    // Report that service is starting
    ReportServiceStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

    // Create stop event
    g_hStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_hStopEvent)
    {
        LogError("[-] Failed to create stop event");
        ReportServiceStatus(SERVICE_STOPPED, GetLastError(), 0);
        return;
    }

    // Create worker thread
    g_hWorkerThread = CreateThread(nullptr, 0, ServiceWorkerThread, nullptr, 0, nullptr);
    if (!g_hWorkerThread)
    {
        LogError("[-] Failed to create worker thread");
        CloseHandle(g_hStopEvent);
        ReportServiceStatus(SERVICE_STOPPED, GetLastError(), 0);
        return;
    }

    // Report that service is now running
    ReportServiceStatus(SERVICE_RUNNING, NO_ERROR, 0);
    LogError("[+] Service is now RUNNING");
}

// Service control handler
DWORD WINAPI ServiceMain::ServiceControlHandler(DWORD dwControl, DWORD dwEventType,
                                                 LPVOID lpEventData, LPVOID lpContext)
{
    switch (dwControl)
    {
    case SERVICE_CONTROL_STOP:
        LogError("[!] Service STOP command received");
        ReportServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 5000);
        
        // Signal the worker thread to stop
        if (g_hStopEvent)
            SetEvent(g_hStopEvent);
        
        return NO_ERROR;

    case SERVICE_CONTROL_SHUTDOWN:
        LogError("[!] System SHUTDOWN detected");
        ReportServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 5000);
        
        // Signal the worker thread to stop
        if (g_hStopEvent)
            SetEvent(g_hStopEvent);
        
        return NO_ERROR;

    case SERVICE_CONTROL_INTERROGATE:
        // Report current status
        ReportServiceStatus(g_serviceStatus.dwCurrentState, NO_ERROR, 0);
        return NO_ERROR;

    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

// Service worker thread
DWORD WINAPI ServiceMain::ServiceWorkerThread(LPVOID lpParam)
{
    // Get dynamic collection interval from registry
    DWORD intervalSeconds = ServiceConfig::GetCollectionIntervalSeconds();
    DWORD intervalMs = intervalSeconds * 1000;
    
    LogError("[+] Service worker thread started");
    LogError("[+] Data collection interval: " + std::to_string(intervalSeconds) + " seconds");

    // Perform initial data collection
    LogError("[+] Performing initial data collection...");
    PerformDataCollection();

    // Main service loop
    while (true)
    {
        // Wait for stop event or timeout
        DWORD waitResult = WaitForSingleObject(g_hStopEvent, intervalMs);

        if (waitResult == WAIT_OBJECT_0)
        {
            // Stop event was signaled
            LogError("[+] Worker thread received stop signal");
            break;
        }
        else if (waitResult == WAIT_TIMEOUT)
        {
            // Reload configuration on each iteration (allows runtime config changes)
            intervalSeconds = ServiceConfig::GetCollectionIntervalSeconds();
            intervalMs = intervalSeconds * 1000;
            
            // Perform periodic data collection
            LogError("[+] Performing scheduled data collection...");
            PerformDataCollection();
        }
        else
        {
            // Wait failed
            LogError("[-] WaitForSingleObject failed in worker thread");
            break;
        }
    }

    LogError("[+] Worker thread shutting down");

    // Report that service is stopped
    ReportServiceStatus(SERVICE_STOPPED, NO_ERROR, 0);

    return 0;
}

// Report service status to SCM
void ServiceMain::ReportServiceStatus(DWORD dwCurrentState, DWORD dwWin32ExitCode, DWORD dwWaitHint)
{
    static DWORD dwCheckPoint = 1;

    g_serviceStatus.dwCurrentState = dwCurrentState;
    g_serviceStatus.dwWin32ExitCode = dwWin32ExitCode;
    g_serviceStatus.dwWaitHint = dwWaitHint;

    if (dwCurrentState == SERVICE_START_PENDING)
    {
        g_serviceStatus.dwControlsAccepted = 0;
    }
    else
    {
        g_serviceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    }

    if ((dwCurrentState == SERVICE_RUNNING) || (dwCurrentState == SERVICE_STOPPED))
    {
        g_serviceStatus.dwCheckPoint = 0;
    }
    else
    {
        g_serviceStatus.dwCheckPoint = dwCheckPoint++;
    }

    SetServiceStatus(g_hServiceStatusHandle, &g_serviceStatus);
}

// Perform data collection
void ServiceMain::PerformDataCollection()
{
    try
    {
        LogError("[+] Starting inventory data collection");

        // Generate JSON inventory
        std::string jsonData = GenerateJSON();

        // Get output directory from configuration
        std::wstring outputDir = ServiceConfig::GetOutputDirectory();

        // Create timestamped filename
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        struct tm timeinfo;
        
        if (localtime_s(&timeinfo, &time) == 0)
        {
            std::wstringstream filename;
            filename << outputDir << L"\\inventory_"
                    << std::put_time(&timeinfo, L"%Y%m%d_%H%M%S") << L".json";

            // Write to timestamped file
            std::ofstream outFile(filename.str(), std::ios::out | std::ios::trunc);
            if (outFile.is_open())
            {
                outFile << jsonData;
                outFile.close();
                LogError("[+] JSON written to: " + WideToUtf8(filename.str()));
            }
            else
            {
                LogError("[-] Failed to write JSON file: " + WideToUtf8(filename.str()));
            }

            // Also write to "latest" file for easy access
            std::wstring latestFile = outputDir + L"\\inventory_latest.json";
            std::ofstream latestOut(latestFile, std::ios::out | std::ios::trunc);
            if (latestOut.is_open())
            {
                latestOut << jsonData;
                latestOut.close();
                LogError("[+] Latest inventory: " + WideToUtf8(latestFile));
            }
        }

        LogError("[+] Data collection completed successfully");
    }
    catch (const std::exception& ex)
    {
        LogError("[-] Exception during data collection: " + std::string(ex.what()));
    }
    catch (...)
    {
        LogError("[-] Unknown exception during data collection");
    }
}
