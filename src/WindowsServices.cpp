#include "WindowsServices.h"
#include "Utils/Utils.h"
#include <memory>

#pragma comment(lib, "advapi32.lib")

// RAII wrapper for SC_HANDLE to prevent handle leaks.
// SC_HANDLE is closed via CloseServiceHandle, not CloseHandle.
class ScHandleGuard
{
public:
    explicit ScHandleGuard(SC_HANDLE handle = nullptr) noexcept : m_handle(handle) {}
    ~ScHandleGuard() noexcept
    {
        if (m_handle != nullptr)
        {
            CloseServiceHandle(m_handle);
        }
    }

    // Non-copyable
    ScHandleGuard(const ScHandleGuard&) = delete;
    ScHandleGuard& operator=(const ScHandleGuard&) = delete;

    // Movable
    ScHandleGuard(ScHandleGuard&& other) noexcept : m_handle(other.m_handle)
    {
        other.m_handle = nullptr;
    }

    ScHandleGuard& operator=(ScHandleGuard&& other) noexcept
    {
        if (this != &other)
        {
            if (m_handle != nullptr)
            {
                CloseServiceHandle(m_handle);
            }
            m_handle = other.m_handle;
            other.m_handle = nullptr;
        }
        return *this;
    }

    SC_HANDLE Get() const noexcept { return m_handle; }
    explicit operator bool() const noexcept { return m_handle != nullptr; }

private:
    SC_HANDLE m_handle;
};

// Convert SERVICE_STATUS_PROCESS.dwCurrentState to a human-readable string.
static std::wstring ServiceStateToString(DWORD state) noexcept
{
    switch (state)
    {
    case SERVICE_RUNNING:          return L"Running";
    case SERVICE_STOPPED:          return L"Stopped";
    case SERVICE_START_PENDING:    return L"StartPending";
    case SERVICE_STOP_PENDING:     return L"StopPending";
    case SERVICE_CONTINUE_PENDING: return L"ContinuePending";
    case SERVICE_PAUSE_PENDING:    return L"PausePending";
    case SERVICE_PAUSED:           return L"Paused";
    default:                       return L"Unknown";
    }
}

// Convert QUERY_SERVICE_CONFIG.dwStartType to a human-readable string.
// Checks for delayed auto-start via QueryServiceConfig2W.
static std::wstring StartTypeToString(DWORD startType, bool isDelayedAutoStart) noexcept
{
    switch (startType)
    {
    case SERVICE_AUTO_START:
        return isDelayedAutoStart ? L"DelayedAuto" : L"Auto";
    case SERVICE_DEMAND_START:     return L"Manual";
    case SERVICE_DISABLED:         return L"Disabled";
    case SERVICE_BOOT_START:       return L"Boot";
    case SERVICE_SYSTEM_START:     return L"System";
    default:                       return L"Unknown";
    }
}

// Convert QUERY_SERVICE_CONFIG.dwServiceType to a human-readable string.
static std::wstring ServiceTypeToString(DWORD serviceType) noexcept
{
    // Check combined types first (most common for Win32 services)
    if (serviceType & SERVICE_WIN32_OWN_PROCESS)
        return L"Win32OwnProcess";
    if (serviceType & SERVICE_WIN32_SHARE_PROCESS)
        return L"Win32ShareProcess";
    if (serviceType & SERVICE_KERNEL_DRIVER)
        return L"KernelDriver";
    if (serviceType & SERVICE_FILE_SYSTEM_DRIVER)
        return L"FileSystemDriver";

    return L"Unknown";
}

// Query the service description via QueryServiceConfig2W.
// Returns empty string on failure (non-fatal).
static std::wstring QueryServiceDescription(SC_HANDLE hService)
{
    DWORD bytesNeeded = 0;

    // First call: determine buffer size
    QueryServiceConfig2W(hService, SERVICE_CONFIG_DESCRIPTION, nullptr, 0, &bytesNeeded);

    if (bytesNeeded == 0)
    {
        return {};
    }

    std::vector<BYTE> buffer(bytesNeeded);
    if (!QueryServiceConfig2W(hService, SERVICE_CONFIG_DESCRIPTION,
                              buffer.data(), bytesNeeded, &bytesNeeded))
    {
        return {};
    }

    const auto* pDesc = reinterpret_cast<const SERVICE_DESCRIPTIONW*>(buffer.data());
    if (pDesc->lpDescription != nullptr)
    {
        return std::wstring(pDesc->lpDescription);
    }

    return {};
}

// Query whether the service has delayed auto-start enabled.
// Returns false on failure or if not applicable (non-fatal).
static bool QueryDelayedAutoStart(SC_HANDLE hService)
{
    DWORD bytesNeeded = 0;

    QueryServiceConfig2W(hService, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, nullptr, 0, &bytesNeeded);

    if (bytesNeeded == 0)
    {
        return false;
    }

    std::vector<BYTE> buffer(bytesNeeded);
    if (!QueryServiceConfig2W(hService, SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                              buffer.data(), bytesNeeded, &bytesNeeded))
    {
        return false;
    }

    const auto* pDelayed = reinterpret_cast<const SERVICE_DELAYED_AUTO_START_INFO*>(buffer.data());
    return pDelayed->fDelayedAutostart != FALSE;
}

// Query service configuration (binary path, start type, account) via QueryServiceConfigW.
// Populates the corresponding fields in serviceInfo.
static void QueryServiceConfiguration(SC_HANDLE hService, WindowsServiceInfo& serviceInfo)
{
    DWORD bytesNeeded = 0;

    // First call: determine buffer size
    QueryServiceConfigW(hService, nullptr, 0, &bytesNeeded);
    DWORD lastError = GetLastError();

    if (lastError != ERROR_INSUFFICIENT_BUFFER || bytesNeeded == 0)
    {
        LogError("[-] QueryServiceConfigW sizing call failed for service: " +
                 WideToUtf8(serviceInfo.serviceName) + ", error: " + std::to_string(lastError));
        return;
    }

    std::vector<BYTE> buffer(bytesNeeded);
    auto* pConfig = reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(buffer.data());

    if (!QueryServiceConfigW(hService, pConfig, bytesNeeded, &bytesNeeded))
    {
        LogError("[-] QueryServiceConfigW failed for service: " +
                 WideToUtf8(serviceInfo.serviceName) + ", error: " + std::to_string(GetLastError()));
        return;
    }

    // Extract configuration fields (validate pointers — SCM returns embedded pointers)
    if (pConfig->lpBinaryPathName != nullptr)
    {
        serviceInfo.binaryPathName = pConfig->lpBinaryPathName;
    }

    if (pConfig->lpServiceStartName != nullptr)
    {
        serviceInfo.serviceStartName = pConfig->lpServiceStartName;
    }

    // Determine if delayed auto-start is enabled (only relevant for auto-start services)
    bool isDelayedAutoStart = false;
    if (pConfig->dwStartType == SERVICE_AUTO_START)
    {
        isDelayedAutoStart = QueryDelayedAutoStart(hService);
    }

    serviceInfo.startType = StartTypeToString(pConfig->dwStartType, isDelayedAutoStart);
    serviceInfo.serviceType = ServiceTypeToString(pConfig->dwServiceType);

    // Query description (optional, non-fatal if missing)
    serviceInfo.description = QueryServiceDescription(hService);
}

std::vector<WindowsServiceInfo> EnumerateWindowsServices()
{
    std::vector<WindowsServiceInfo> services;

    // Open the SCM with enumerate access (least privilege)
    ScHandleGuard hSCManager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE));
    if (!hSCManager)
    {
        LogError("[-] OpenSCManagerW failed, error: " + std::to_string(GetLastError()) +
                 " — " + GetWindowsErrorMessage(static_cast<LONG>(GetLastError())));
        return services;
    }

    // First call: determine required buffer size for all Win32 services
    DWORD bytesNeeded = 0;
    DWORD serviceCount = 0;
    DWORD resumeHandle = 0;

    EnumServicesStatusExW(
        hSCManager.Get(),
        SC_ENUM_PROCESS_INFO,
        SERVICE_WIN32,          // Win32OwnProcess + Win32ShareProcess
        SERVICE_STATE_ALL,      // Both running and stopped services
        nullptr,
        0,
        &bytesNeeded,
        &serviceCount,
        &resumeHandle,
        nullptr                 // All groups
    );

    DWORD lastError = GetLastError();
    if (lastError != ERROR_MORE_DATA || bytesNeeded == 0)
    {
        LogError("[-] EnumServicesStatusExW sizing call failed, error: " +
                 std::to_string(lastError) + " — " + GetWindowsErrorMessage(static_cast<LONG>(lastError)));
        return services;
    }

    // Allocate buffer and enumerate all services
    // Add extra margin for services that may be registered between the two calls
    const DWORD bufferSize = bytesNeeded + 4096;
    std::vector<BYTE> buffer(bufferSize);
    resumeHandle = 0;

    if (!EnumServicesStatusExW(
            hSCManager.Get(),
            SC_ENUM_PROCESS_INFO,
            SERVICE_WIN32,
            SERVICE_STATE_ALL,
            buffer.data(),
            bufferSize,
            &bytesNeeded,
            &serviceCount,
            &resumeHandle,
            nullptr))
    {
        lastError = GetLastError();

        // ERROR_MORE_DATA means partial results — process what we have
        if (lastError != ERROR_MORE_DATA)
        {
            LogError("[-] EnumServicesStatusExW failed, error: " +
                     std::to_string(lastError) + " — " + GetWindowsErrorMessage(static_cast<LONG>(lastError)));
            return services;
        }

        LogError("[!] EnumServicesStatusExW returned partial results (" +
                 std::to_string(serviceCount) + " services), some services may be missing");
    }

    if (serviceCount == 0)
    {
        LogError("[!] EnumServicesStatusExW returned 0 services");
        return services;
    }

    services.reserve(serviceCount);

    const auto* pServiceStatus = reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());

    for (DWORD i = 0; i < serviceCount; ++i)
    {
        const auto& svc = pServiceStatus[i];

        WindowsServiceInfo info = {};

        // Service name and display name come from the enumeration
        if (svc.lpServiceName != nullptr)
        {
            info.serviceName = svc.lpServiceName;
        }
        if (svc.lpDisplayName != nullptr)
        {
            info.displayName = svc.lpDisplayName;
        }

        // Status and PID from SERVICE_STATUS_PROCESS
        info.currentState = ServiceStateToString(svc.ServiceStatusProcess.dwCurrentState);
        info.processId = svc.ServiceStatusProcess.dwProcessId;
        info.isRunning = (svc.ServiceStatusProcess.dwCurrentState == SERVICE_RUNNING);

        // Open individual service handle to query configuration
        // Use SERVICE_QUERY_CONFIG — least privilege for config queries
        ScHandleGuard hService(OpenServiceW(hSCManager.Get(), svc.lpServiceName, SERVICE_QUERY_CONFIG));
        if (hService)
        {
            QueryServiceConfiguration(hService.Get(), info);
        }
        else
        {
            // Non-fatal: we still have name and status from enumeration
            // This can happen for services with restricted ACLs
            DWORD openError = GetLastError();
            if (openError != ERROR_ACCESS_DENIED)
            {
                LogError("[!] OpenServiceW failed for: " + WideToUtf8(info.serviceName) +
                         ", error: " + std::to_string(openError));
            }
        }

        services.push_back(std::move(info));
    }

    LogError("[+] Enumerated " + std::to_string(services.size()) + " Windows services");
    return services;
}
