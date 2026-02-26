#include "ServiceMain.h"
#include "ServiceConfig.h"
#include "../WindowsEnum.h"
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <shlobj.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

static void WriteServiceMarkerFile(const wchar_t* fileName, const wchar_t* message)
{
    std::wstring logDir = ServiceConfig::DEFAULT_LOG_DIRECTORY;
    DWORD attribs = GetFileAttributesW(logDir.c_str());
    if (attribs == INVALID_FILE_ATTRIBUTES || !(attribs & FILE_ATTRIBUTE_DIRECTORY))
    {
        SHCreateDirectoryExW(nullptr, logDir.c_str(), nullptr);
    }

    std::wstring path = logDir + L"\\" + fileName;

    HANDLE hFile = CreateFileW(
        path.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (hFile == INVALID_HANDLE_VALUE)
    {
        return;
    }

    SYSTEMTIME st = {};
    GetLocalTime(&st);

    wchar_t buffer[512] = {};
    _snwprintf_s(
        buffer,
        _countof(buffer),
        _TRUNCATE,
        L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s\r\n",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        (message != nullptr ? message : L"marker"));

    DWORD bytesToWrite = static_cast<DWORD>(wcslen(buffer) * sizeof(wchar_t));
    DWORD bytesWritten = 0;
    WriteFile(hFile, buffer, bytesToWrite, &bytesWritten, nullptr);
    CloseHandle(hFile);
}

// Static member initialization
SERVICE_STATUS_HANDLE ServiceMain::g_hServiceStatusHandle = nullptr;
SERVICE_STATUS ServiceMain::g_serviceStatus = {};
HANDLE ServiceMain::g_hStopEvent = nullptr;
HANDLE ServiceMain::g_hWorkerThread = nullptr;
ProcessTracker ServiceMain::g_processTracker;

ProcessTracker* ServiceMain::GetProcessTracker()
{
    return &g_processTracker;
}

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
    WriteServiceMarkerFile(L"service_main_entry.txt", L"ServiceMainEntry reached");
    LogError("[+] ServiceMain entry point called by SCM");

    // Register the service control handler
    g_hServiceStatusHandle = RegisterServiceCtrlHandlerExW(
        ServiceConfig::SERVICE_NAME,
        ServiceControlHandler,
        nullptr
    );

    if (!g_hServiceStatusHandle)
    {
        WriteServiceMarkerFile(L"service_main_entry.txt", L"RegisterServiceCtrlHandlerExW failed");
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
        WriteServiceMarkerFile(L"service_main_entry.txt", L"CreateEventW failed");
        LogError("[-] Failed to create stop event");
        ReportServiceStatus(SERVICE_STOPPED, GetLastError(), 0);
        return;
    }

    // Create worker thread
    WriteServiceMarkerFile(L"service_main_entry.txt", L"About to CreateThread(worker)");
    g_hWorkerThread = CreateThread(nullptr, 0, ServiceWorkerThread, nullptr, 0, nullptr);
    if (!g_hWorkerThread)
    {
        WriteServiceMarkerFile(L"service_main_entry.txt", L"CreateThread failed");
        LogError("[-] Failed to create worker thread");
        CloseHandle(g_hStopEvent);
        ReportServiceStatus(SERVICE_STOPPED, GetLastError(), 0);
        return;
    }

    // Report that service is now running
    ReportServiceStatus(SERVICE_RUNNING, NO_ERROR, 0);
    LogError("[+] Successfully reported the service status that it is now RUNNING");
}

// Service control handler
DWORD WINAPI ServiceMain::ServiceControlHandler(DWORD dwControl, DWORD dwEventType,
                                                 LPVOID lpEventData, LPVOID lpContext)
{
    switch (dwControl)
    {
    case SERVICE_CONTROL_STOP:
        LogError("[!] Service STOP command received");
        ReportServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 120000);
        
        // Signal the worker thread to stop
        if (g_hStopEvent)
            SetEvent(g_hStopEvent);
        
        return NO_ERROR;

    case SERVICE_CONTROL_SHUTDOWN:
        LogError("[!] System SHUTDOWN detected");
        ReportServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 120000);
        
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
    WriteServiceMarkerFile(L"worker_thread_entry.txt", L"ServiceWorkerThread reached");
    // CRITICAL: Write to file IMMEDIATELY before any other code
    // This bypasses LogError() to catch early crashes
    {
        std::ofstream crashLog("C:\\ProgramData\\Panoptes\\Spectra\\Logs\\worker_thread_start.txt", std::ios::app);
        if (crashLog.is_open())
        {
            auto now = std::chrono::system_clock::now();
            auto time = std::chrono::system_clock::to_time_t(now);
            struct tm timeinfo;
            if (localtime_s(&timeinfo, &time) == 0)
            {
                crashLog << "[" << std::put_time(&timeinfo, "%Y-%m-%d %H:%M:%S") << "] WORKER THREAD ENTRY POINT REACHED" << std::endl;
            }
            crashLog.close();
        }
    }

    try
    {
        LogError("[+] ========== SERVICE WORKER THREAD STARTED ==========");
        
        // Get dynamic collection interval from registry
        DWORD intervalSeconds = ServiceConfig::GetCollectionIntervalSeconds();
        DWORD intervalMs = intervalSeconds * 1000;
        
        LogError("[+] Service worker thread started");
        LogError("[+] Data collection interval: " + std::to_string(intervalSeconds) + " seconds (" + 
                 std::to_string(intervalSeconds / 3600) + " hours)");

        // Ensure output directory exists
        std::wstring outputDir = ServiceConfig::GetOutputDirectory();
        DWORD attribs = GetFileAttributesW(outputDir.c_str());
        if (attribs == INVALID_FILE_ATTRIBUTES || !(attribs & FILE_ATTRIBUTE_DIRECTORY))
        {
            LogError("[!] WARNING: Output directory does not exist, creating: " + WideToUtf8(outputDir));
            if (SHCreateDirectoryExW(nullptr, outputDir.c_str(), nullptr) != ERROR_SUCCESS)
            {
                LogError("[-] FATAL: Failed to create output directory!");
                ReportServiceStatus(SERVICE_STOPPED, ERROR_PATH_NOT_FOUND, 0);
                return 1;
            }
            LogError("[+] Output directory created successfully");
        }

        // Start real-time process tracking (ETW with Sysmon fallback)
        if (g_processTracker.Start())
        {
            LogError("[+] Real-time process tracking is active");
        }
        else
        {
            LogError("[!] Real-time process tracking could not be started (disabled or all sources failed)");
        }

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

        // Flush accumulated process data to disk before stopping the tracker.
        // Without this, all ETW-buffered processes since the last scheduled
        // collection (up to 24 hours of data) would be lost on service stop
        // or system reboot. CollectAndReset() inside GenerateProcessJSON()
        // drains the buffer, so the tracker can be stopped cleanly afterward.
        //
        // IMPORTANT: Only flush process data here, NOT the full inventory.
        // PerformDataCollection() runs the entire inventory pipeline (VSS
        // snapshots, modern app enumeration, Windows Update search) which
        // takes 3+ minutes and causes SERVICE_STOP_PENDING timeouts.
        LogError("[+] Flushing process data before shutdown...");
        try
        {
            std::wstring outputDir = ServiceConfig::GetOutputDirectory();
            std::string processData = GenerateProcessJSON();

            std::wstring processFile = outputDir + L"\\processes.json";
            std::ofstream procOutFile(processFile, std::ios::out | std::ios::trunc);
            if (procOutFile.is_open())
            {
                procOutFile << processData;
                procOutFile.close();
                LogError("[+] Process JSON flushed to: " + WideToUtf8(processFile));
            }
            else
            {
                LogError("[-] Failed to write process JSON during shutdown flush");
            }
        }
        catch (const std::exception& ex)
        {
            LogError("[-] Exception during shutdown process flush: " + std::string(ex.what()));
        }
        catch (...)
        {
            LogError("[-] Unknown exception during shutdown process flush");
        }

        // Log ETW/process tracker diagnostics before stopping.
        // Gives operators visibility into whether real-time tracking was healthy
        // during this service run (events received, filtered, deduplicated).
        {
            ProcessTrackerDiagnostics diag = g_processTracker.GetDiagnostics();
            std::string sourceName;
            switch (diag.activeSource)
            {
            case ProcessTrackingSource::EtwKernelProcess: sourceName = "ETW (Kernel-Process)"; break;
            case ProcessTrackingSource::SysmonEventLog:   sourceName = "Sysmon (Event Log)";   break;
            case ProcessTrackingSource::Disabled:         sourceName = "Disabled";              break;
            default:                                      sourceName = "None";                  break;
            }
            LogError("[+] ProcessTracker diagnostics at shutdown:");
            LogError("[+]   Active source:            " + sourceName);
            LogError("[+]   Total events received:    " + std::to_string(diag.totalEventsReceived));
            LogError("[+]   Events deduplicated out:  " + std::to_string(diag.eventsDeduplicatedOut));
            LogError("[+]   Service processes excluded:" + std::to_string(diag.serviceProcessesExcluded));
            LogError("[+]   Managed path excluded:    " + std::to_string(diag.managedPathProcessesExcluded));
        }

        // Stop real-time process tracking
        g_processTracker.Stop();

        // Report that service is stopped
        ReportServiceStatus(SERVICE_STOPPED, NO_ERROR, 0);

        return 0;
    }
    catch (const std::exception& ex)
    {
        // Log crash details to both files
        std::ofstream crashLog("C:\\ProgramData\\Panoptes\\Spectra\\Logs\\worker_crash.txt", std::ios::app);
        if (crashLog.is_open())
        {
            auto now = std::chrono::system_clock::now();
            auto time = std::chrono::system_clock::to_time_t(now);
            struct tm timeinfo;
            if (localtime_s(&timeinfo, &time) == 0)
            {
                crashLog << "[" << std::put_time(&timeinfo, "%Y-%m-%d %H:%M:%S") << "] EXCEPTION: " << ex.what() << std::endl;
            }
            crashLog.close();
        }
        
        LogError("[-] FATAL: Worker thread crashed with exception: " + std::string(ex.what()));
        ReportServiceStatus(SERVICE_STOPPED, ERROR_EXCEPTION_IN_SERVICE, 0);
        return 1;
    }
    catch (...)
    {
        // Log unknown crash
        std::ofstream crashLog("C:\\ProgramData\\Panoptes\\Spectra\\Logs\\worker_crash.txt", std::ios::app);
        if (crashLog.is_open())
        {
            auto now = std::chrono::system_clock::now();
            auto time = std::chrono::system_clock::to_time_t(now);
            struct tm timeinfo;
            if (localtime_s(&timeinfo, &time) == 0)
            {
                crashLog << "[" << std::put_time(&timeinfo, "%Y-%m-%d %H:%M:%S") << "] UNKNOWN EXCEPTION" << std::endl;
            }
            crashLog.close();
        }
        
        LogError("[-] FATAL: Worker thread crashed with unknown exception");
        ReportServiceStatus(SERVICE_STOPPED, ERROR_EXCEPTION_IN_SERVICE, 0);
        return 1;
    }
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

        // Get output directory from configuration
        std::wstring outputDir = ServiceConfig::GetOutputDirectory();

        // Generate and write inventory JSON (apps, updates, services, OS info)
        std::string jsonData = GenerateJSON();

        std::wstring inventoryFile = outputDir + L"\\inventory.json";
        std::ofstream outFile(inventoryFile, std::ios::out | std::ios::trunc);
        if (outFile.is_open())
        {
            outFile << jsonData;
            outFile.close();
            LogError("[+] Inventory JSON written to: " + WideToUtf8(inventoryFile));
        }
        else
        {
            LogError("[-] Failed to write inventory JSON file: " + WideToUtf8(inventoryFile));
        }

        // Generate and write process JSON (ETW + snapshot process data)
        // Overwritten each collection cycle with cumulative ETW data + fresh snapshot.
        std::string processData = GenerateProcessJSON();

        std::wstring processFile = outputDir + L"\\processes.json";
        std::ofstream procOutFile(processFile, std::ios::out | std::ios::trunc);
        if (procOutFile.is_open())
        {
            procOutFile << processData;
            procOutFile.close();
            LogError("[+] Process JSON written to: " + WideToUtf8(processFile));
        }
        else
        {
            LogError("[-] Failed to write process JSON file: " + WideToUtf8(processFile));
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
