#include "ProcessTracker.h"
#include "Utils/Utils.h"
#include "Service/ServiceConfig.h"
#include <tlhelp32.h>
#include <sddl.h>
#include <tdh.h>
#include <psapi.h>
#include <winevt.h>
#include <winver.h>
#include <iomanip>
#include <chrono>
#include <sstream>
#include <functional>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "tdh.lib")
#pragma comment(lib, "version.lib")

// ============================================================================
// RAII Handle Wrappers
// Prevent handle leaks in all code paths (exceptions, early returns).
// Each wrapper calls the correct close function for its handle type.
// ============================================================================

// RAII wrapper for generic Windows HANDLE (CloseHandle).
class WinHandle {
public:
    explicit WinHandle(HANDLE h = nullptr) noexcept : m_handle(h) {}
    ~WinHandle() noexcept { Reset(); }

    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;

    WinHandle(WinHandle&& other) noexcept : m_handle(other.m_handle) { other.m_handle = nullptr; }
    WinHandle& operator=(WinHandle&& other) noexcept {
        if (this != &other) { Reset(); m_handle = other.m_handle; other.m_handle = nullptr; }
        return *this;
    }

    void Reset() noexcept {
        if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
        }
        m_handle = nullptr;
    }

    HANDLE Get() const noexcept { return m_handle; }
    HANDLE Release() noexcept { HANDLE h = m_handle; m_handle = nullptr; return h; }
    explicit operator bool() const noexcept { return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE; }

private:
    HANDLE m_handle;
};

// RAII wrapper for Toolhelp snapshot handles (INVALID_HANDLE_VALUE sentinel).
class SnapshotHandle {
public:
    explicit SnapshotHandle(HANDLE h = INVALID_HANDLE_VALUE) noexcept : m_handle(h) {}
    ~SnapshotHandle() noexcept { Reset(); }

    SnapshotHandle(const SnapshotHandle&) = delete;
    SnapshotHandle& operator=(const SnapshotHandle&) = delete;

    SnapshotHandle(SnapshotHandle&& other) noexcept : m_handle(other.m_handle) { other.m_handle = INVALID_HANDLE_VALUE; }
    SnapshotHandle& operator=(SnapshotHandle&& other) noexcept {
        if (this != &other) { Reset(); m_handle = other.m_handle; other.m_handle = INVALID_HANDLE_VALUE; }
        return *this;
    }

    void Reset() noexcept {
        if (m_handle != INVALID_HANDLE_VALUE && m_handle != nullptr) {
            CloseHandle(m_handle);
        }
        m_handle = INVALID_HANDLE_VALUE;
    }

    HANDLE Get() const noexcept { return m_handle; }
    explicit operator bool() const noexcept { return m_handle != INVALID_HANDLE_VALUE && m_handle != nullptr; }

private:
    HANDLE m_handle;
};

// RAII wrapper for HMODULE (FreeLibrary).
class LibraryHandle {
public:
    explicit LibraryHandle(HMODULE h = nullptr) noexcept : m_handle(h) {}
    ~LibraryHandle() noexcept { if (m_handle) FreeLibrary(m_handle); }

    LibraryHandle(const LibraryHandle&) = delete;
    LibraryHandle& operator=(const LibraryHandle&) = delete;

    HMODULE Get() const noexcept { return m_handle; }
    explicit operator bool() const noexcept { return m_handle != nullptr; }

private:
    HMODULE m_handle;
};

// RAII wrapper for memory allocated by LocalAlloc (e.g., ConvertStringSidToSidW).
class LocalMemGuard {
public:
    explicit LocalMemGuard(void* p = nullptr) noexcept : m_ptr(p) {}
    ~LocalMemGuard() noexcept { if (m_ptr) LocalFree(m_ptr); }

    LocalMemGuard(const LocalMemGuard&) = delete;
    LocalMemGuard& operator=(const LocalMemGuard&) = delete;

    void* Get() const noexcept { return m_ptr; }
    explicit operator bool() const noexcept { return m_ptr != nullptr; }

private:
    void* m_ptr;
};

// Microsoft-Windows-Kernel-Process provider GUID
// {22FB2CD6-0E7B-422B-A0C7-2FAD1FD0E716}
const GUID ProcessTracker::s_kernelProcessProviderGuid =
    { 0x22FB2CD6, 0x0E7B, 0x422B, { 0xA0, 0xC7, 0x2F, 0xAD, 0x1F, 0xD0, 0xE7, 0x16 } };

// Global instance for static callback routing
ProcessTracker* ProcessTracker::s_instance = nullptr;

// Kernel-Process event IDs
static constexpr USHORT KERNEL_PROCESS_EVENT_START = 1;   // ProcessStart
static constexpr USHORT KERNEL_PROCESS_EVENT_STOP  = 2;   // ProcessStop

// Maximum number of observed processes buffered between collections.
// Prevents unbounded memory growth on busy servers (e.g., build farms, CI/CD).
// At ~2 KB per RunningProcessInfo entry, 100K entries ≈ 200 MB worst-case.
static constexpr size_t MAX_OBSERVED_PROCESSES = 100000;

// Maximum length (in wchar_t) for attacker-controlled strings (imagePath, commandLine).
// Windows command lines can reach 32,767 characters. Storing the full string in
// 100K buffer entries + dedup map would allow an attacker to exhaust memory by
// spawning processes with unique near-max-length command lines.
// 8,192 characters (~16 KB) preserves full fidelity for all legitimate use
// while capping worst-case at ~1.6 GB for the process buffer.
static constexpr size_t MAX_PROCESS_STRING_LENGTH = 8192;

// Cache TTL for services.exe PID lookup (5 minutes).
// services.exe PID is stable but we refresh periodically for correctness.
static constexpr ULONGLONG SERVICES_PID_CACHE_TTL_MS = 300000ULL;

ProcessTracker::ProcessTracker()
{
    s_instance = this;
}

ProcessTracker::~ProcessTracker()
{
    Stop();
    if (s_instance == this)
    {
        s_instance = nullptr;
    }
}

bool ProcessTracker::Start()
{
    // Check if process tracking is enabled via registry config
    if (!ServiceConfig::IsProcessTrackingEnabled())
    {
        LogError("[!] Process tracking is disabled via registry configuration (EnableProcessTracking=0)");
        std::lock_guard<std::mutex> lock(m_diagMutex);
        m_diagnostics.activeSource = ProcessTrackingSource::Disabled;
        return false;
    }

    if (m_isRunning.load())
    {
        LogError("[!] ProcessTracker::Start() called but tracker is already running");
        return true;
    }

    // Create stop event for clean shutdown
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (m_stopEvent == nullptr)
    {
        LogError("[-] ProcessTracker: Failed to create stop event, error: " +
                 std::to_string(GetLastError()));
        return false;
    }

    LogError("[+] ProcessTracker: Attempting to start ETW session...");

    // Resolve managed directory prefixes once at startup for path-based exclusion.
    // Must be called before StartEtwSession() so the ETW callback can use them.
    InitExcludedPathPrefixes();

    // Primary: Try ETW kernel process provider
    if (StartEtwSession())
    {
        m_isRunning.store(true);
        LogError("[+] ProcessTracker: ETW session started successfully");
        return true;
    }

    // ETW failed — log diagnostic info
    {
        std::lock_guard<std::mutex> lock(m_diagMutex);
        LogError("[!] ProcessTracker: ETW session failed (error: " +
                 std::to_string(m_diagnostics.etwSessionStartResult) +
                 "), checking Sysmon fallback...");
    }

    // Fallback: Try Sysmon event log
    if (StartSysmonFallback())
    {
        m_isRunning.store(true);
        LogError("[+] ProcessTracker: Sysmon fallback started successfully");
        return true;
    }

    // Both sources failed
    LogError("[-] ProcessTracker: All process tracking sources failed. "
             "ETW session could not be created and Sysmon is not available. "
             "Process tracking will be inactive for this collection cycle.");

    if (m_stopEvent != nullptr)
    {
        CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
    }

    return false;
}

void ProcessTracker::Stop()
{
    if (!m_isRunning.load() && m_stopEvent == nullptr)
    {
        return;
    }

    LogError("[+] ProcessTracker: Stopping...");

    // Signal stop to all threads
    if (m_stopEvent != nullptr)
    {
        SetEvent(m_stopEvent);
    }

    StopEtwSession();
    StopSysmonFallback();

    if (m_stopEvent != nullptr)
    {
        CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
    }

    m_isRunning.store(false);
    LogError("[+] ProcessTracker: Stopped");
}

std::vector<RunningProcessInfo> ProcessTracker::CollectAndReset()
{
    std::lock_guard<std::mutex> lock(m_processMutex);
    std::vector<RunningProcessInfo> result;
    result.swap(m_observedProcesses);
    return result;
}

ProcessTrackerDiagnostics ProcessTracker::GetDiagnostics() const
{
    std::lock_guard<std::mutex> lock(m_diagMutex);
    return m_diagnostics;
}

bool ProcessTracker::IsRunning() const
{
    return m_isRunning.load();
}

// ============================================================================
// ETW Session Management
// ============================================================================

bool ProcessTracker::StartEtwSession()
{
    // Allocate EVENT_TRACE_PROPERTIES with space for session name + log file name
    const size_t sessionNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    const size_t logFileNameOffset = sessionNameOffset + (wcslen(ETW_SESSION_NAME) + 1) * sizeof(wchar_t);
    const size_t bufferSize = logFileNameOffset + sizeof(wchar_t); // No log file name needed

    std::vector<BYTE> propertiesBuffer(bufferSize, 0);
    auto* pProperties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propertiesBuffer.data());

    pProperties->Wnode.BufferSize = static_cast<ULONG>(bufferSize);
    pProperties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    pProperties->Wnode.ClientContext = 1; // QPC for high-resolution timestamps
    pProperties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    pProperties->LoggerNameOffset = static_cast<ULONG>(sessionNameOffset);
    pProperties->LogFileNameOffset = 0; // No log file — real-time only

    // Buffer settings for process events (moderate volume)
    pProperties->BufferSize = 64;      // 64 KB per buffer
    pProperties->MinimumBuffers = 4;
    pProperties->MaximumBuffers = 16;
    pProperties->FlushTimer = 1;       // Flush every 1 second for responsiveness

    // Copy session name into the properties buffer
    wcscpy_s(
        reinterpret_cast<wchar_t*>(propertiesBuffer.data() + sessionNameOffset),
        wcslen(ETW_SESSION_NAME) + 1,
        ETW_SESSION_NAME);

    // Try to stop any orphaned session from a previous crash
    {
        std::vector<BYTE> stopBuffer(bufferSize, 0);
        auto* pStopProps = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(stopBuffer.data());
        pStopProps->Wnode.BufferSize = static_cast<ULONG>(bufferSize);
        pStopProps->LoggerNameOffset = static_cast<ULONG>(sessionNameOffset);

        wcscpy_s(
            reinterpret_cast<wchar_t*>(stopBuffer.data() + sessionNameOffset),
            wcslen(ETW_SESSION_NAME) + 1,
            ETW_SESSION_NAME);

        ULONG stopResult = ControlTraceW(0, ETW_SESSION_NAME, pStopProps, EVENT_TRACE_CONTROL_STOP);
        if (stopResult == ERROR_SUCCESS)
        {
            LogError("[!] ProcessTracker: Cleaned up orphaned ETW session from previous run");
        }
    }

    // Start the trace session
    ULONG startResult = StartTraceW(&m_sessionHandle, ETW_SESSION_NAME, pProperties);
    {
        std::lock_guard<std::mutex> lock(m_diagMutex);
        m_diagnostics.etwSessionStartResult = startResult;
    }

    if (startResult != ERROR_SUCCESS)
    {
        std::lock_guard<std::mutex> lock(m_diagMutex);
        m_diagnostics.lastErrorMessage = L"StartTraceW failed: " + std::to_wstring(startResult);

        if (startResult == ERROR_ACCESS_DENIED)
        {
            LogError("[-] ProcessTracker: StartTraceW returned ERROR_ACCESS_DENIED. "
                     "Ensure the service runs as SYSTEM and SeSystemProfilePrivilege "
                     "is included in the required privileges list.");
        }
        else if (startResult == static_cast<ULONG>(ERROR_NO_SYSTEM_RESOURCES))
        {
            LogError("[-] ProcessTracker: StartTraceW returned ERROR_NO_SYSTEM_RESOURCES. "
                     "System may have reached the 64-session ETW limit. "
                     "Check for other monitoring agents consuming ETW sessions.");
        }
        else
        {
            LogError("[-] ProcessTracker: StartTraceW failed, error: " +
                     std::to_string(startResult) + " — " +
                     GetWindowsErrorMessage(static_cast<LONG>(startResult)));
        }

        m_sessionHandle = 0;
        return false;
    }

    // Enable the Microsoft-Windows-Kernel-Process provider on our session
    // TRACE_LEVEL_INFORMATION captures ProcessStart (event ID 1)
    ULONG enableResult = EnableTraceEx2(
        m_sessionHandle,
        &s_kernelProcessProviderGuid,
        EVENT_CONTROL_CODE_ENABLE_PROVIDER,
        TRACE_LEVEL_INFORMATION,
        0x10,   // Keyword: WINEVENT_KEYWORD_PROCESS (0x10) for process events only
        0,      // MatchAnyKeyword
        0,      // Timeout (0 = no timeout)
        nullptr // EnableParameters
    );

    if (enableResult != ERROR_SUCCESS)
    {
        LogError("[-] ProcessTracker: EnableTraceEx2 failed, error: " +
                 std::to_string(enableResult) + " — " +
                 GetWindowsErrorMessage(static_cast<LONG>(enableResult)));

        // Stop the session we just started
        StopEtwSession();
        return false;
    }

    // Open a real-time consumer trace handle
    EVENT_TRACE_LOGFILEW logFile = {};
    logFile.LoggerName = const_cast<LPWSTR>(ETW_SESSION_NAME);
    logFile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    logFile.EventRecordCallback = EtwEventCallback;

    m_consumerHandle = OpenTraceW(&logFile);
    {
        std::lock_guard<std::mutex> lock(m_diagMutex);
        if (m_consumerHandle == INVALID_PROCESSTRACE_HANDLE)
        {
            DWORD openError = GetLastError();
            m_diagnostics.etwConsumerOpenResult = openError;
            m_diagnostics.lastErrorMessage = L"OpenTraceW failed: " + std::to_wstring(openError);

            LogError("[-] ProcessTracker: OpenTraceW failed, error: " +
                     std::to_string(openError));

            StopEtwSession();
            return false;
        }
        m_diagnostics.etwConsumerOpenResult = ERROR_SUCCESS;
        m_diagnostics.activeSource = ProcessTrackingSource::EtwKernelProcess;
    }

    // ProcessTrace blocks, so run it on a dedicated thread
    m_etwConsumerThread = CreateThread(nullptr, 0, EtwConsumerThread, this, 0, nullptr);
    if (m_etwConsumerThread == nullptr)
    {
        LogError("[-] ProcessTracker: Failed to create ETW consumer thread, error: " +
                 std::to_string(GetLastError()));
        CloseTrace(m_consumerHandle);
        m_consumerHandle = INVALID_PROCESSTRACE_HANDLE;
        StopEtwSession();
        return false;
    }

    return true;
}

void ProcessTracker::StopEtwSession()
{
    // Close the consumer handle first (unblocks ProcessTrace)
    if (m_consumerHandle != INVALID_PROCESSTRACE_HANDLE)
    {
        CloseTrace(m_consumerHandle);
        m_consumerHandle = INVALID_PROCESSTRACE_HANDLE;
    }

    // Wait for the consumer thread to exit
    if (m_etwConsumerThread != nullptr)
    {
        WaitForSingleObject(m_etwConsumerThread, 5000);
        CloseHandle(m_etwConsumerThread);
        m_etwConsumerThread = nullptr;
    }

    // Stop the trace session
    if (m_sessionHandle != 0)
    {
        const size_t sessionNameLen = wcslen(ETW_SESSION_NAME) + 1;
        const size_t bufferSize = sizeof(EVENT_TRACE_PROPERTIES) + sessionNameLen * sizeof(wchar_t);
        std::vector<BYTE> buffer(bufferSize, 0);
        auto* pProperties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buffer.data());
        pProperties->Wnode.BufferSize = static_cast<ULONG>(bufferSize);
        pProperties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

        ControlTraceW(m_sessionHandle, nullptr, pProperties, EVENT_TRACE_CONTROL_STOP);
        m_sessionHandle = 0;
    }
}

// ============================================================================
// ETW Consumer Thread + Callback
// ============================================================================

DWORD WINAPI ProcessTracker::EtwConsumerThread(LPVOID lpParam)
{
    auto* tracker = static_cast<ProcessTracker*>(lpParam);
    if (tracker == nullptr)
        return 1;

    TRACEHANDLE handles[1] = { tracker->m_consumerHandle };

    // ProcessTrace blocks until the session is stopped or CloseTrace is called
    ULONG status = ProcessTrace(handles, 1, nullptr, nullptr);
    if (status != ERROR_SUCCESS && status != ERROR_CANCELLED)
    {
        LogError("[!] ProcessTracker: ProcessTrace exited with status: " +
                 std::to_string(status));
    }

    return 0;
}

void WINAPI ProcessTracker::EtwEventCallback(PEVENT_RECORD pEventRecord)
{
    if (pEventRecord == nullptr || s_instance == nullptr)
        return;

    // Filter: only process events from Microsoft-Windows-Kernel-Process
    if (!IsEqualGUID(pEventRecord->EventHeader.ProviderId, s_kernelProcessProviderGuid))
        return;

    // Filter: only ProcessStart events (event ID 1)
    if (pEventRecord->EventHeader.EventDescriptor.Id != KERNEL_PROCESS_EVENT_START)
        return;

    s_instance->HandleProcessStartEvent(pEventRecord);
}

void ProcessTracker::HandleProcessStartEvent(PEVENT_RECORD pEventRecord)
{
    // Defensive: validate the event record before parsing.
    // Malformed or truncated events (e.g., from buffer overflows in the kernel
    // provider) could cause TDH to read out-of-bounds if UserDataLength is bogus.
    if (pEventRecord->UserData == nullptr || pEventRecord->UserDataLength == 0)
    {
        return;
    }

    // Extract data from the event using TDH (Trace Data Helper)
    DWORD bufferSize = 0;
    TDHSTATUS status = TdhGetEventInformation(pEventRecord, 0, nullptr, nullptr, &bufferSize);
    if (status != ERROR_INSUFFICIENT_BUFFER || bufferSize == 0)
    {
        return;
    }

    std::vector<BYTE> buffer(bufferSize);
    auto* pInfo = reinterpret_cast<TRACE_EVENT_INFO*>(buffer.data());
    status = TdhGetEventInformation(pEventRecord, 0, nullptr, pInfo, &bufferSize);
    if (status != ERROR_SUCCESS)
    {
        return;
    }

    // Parse properties from the event
    std::wstring imagePath;
    std::wstring commandLine;
    DWORD processId = pEventRecord->EventHeader.ProcessId;
    DWORD parentProcessId = 0;

    // Iterate through event properties to extract ImageName, CommandLine, ParentProcessID
    for (ULONG i = 0; i < pInfo->TopLevelPropertyCount; ++i)
    {
        EVENT_PROPERTY_INFO& propInfo = pInfo->EventPropertyInfoArray[i];
        LPCWSTR propName = reinterpret_cast<LPCWSTR>(
            reinterpret_cast<BYTE*>(pInfo) + propInfo.NameOffset);

        PROPERTY_DATA_DESCRIPTOR dataDesc = {};
        dataDesc.PropertyName = reinterpret_cast<ULONGLONG>(propName);
        dataDesc.ArrayIndex = ULONG_MAX;

        DWORD propSize = 0;
        status = TdhGetPropertySize(pEventRecord, 0, nullptr, 1, &dataDesc, &propSize);
        if (status != ERROR_SUCCESS || propSize == 0)
        {
            continue;
        }

        std::vector<BYTE> propBuffer(propSize);
        status = TdhGetProperty(pEventRecord, 0, nullptr, 1, &dataDesc, propSize, propBuffer.data());
        if (status != ERROR_SUCCESS)
        {
            continue;
        }

        if (_wcsicmp(propName, L"ImageName") == 0 && propSize >= sizeof(wchar_t))
        {
            // Safe extraction: ensure null-termination within the buffer bounds.
            // TDH string properties should be null-terminated, but a truncated
            // event from a misbehaving provider could omit the terminator.
            const wchar_t* raw = reinterpret_cast<const wchar_t*>(propBuffer.data());
            size_t maxChars = propSize / sizeof(wchar_t);
            size_t len = wcsnlen_s(raw, maxChars);
            imagePath.assign(raw, len);
        }
        else if (_wcsicmp(propName, L"CommandLine") == 0 && propSize >= sizeof(wchar_t))
        {
            const wchar_t* raw = reinterpret_cast<const wchar_t*>(propBuffer.data());
            size_t maxChars = propSize / sizeof(wchar_t);
            size_t len = wcsnlen_s(raw, maxChars);
            commandLine.assign(raw, len);
        }
        else if (_wcsicmp(propName, L"ParentProcessID") == 0 && propSize >= sizeof(DWORD))
        {
            parentProcessId = *reinterpret_cast<const DWORD*>(propBuffer.data());
        }
    }

    if (imagePath.empty())
    {
        return; // No image path — skip
    }

    // Truncate attacker-controlled strings to cap memory usage.
    // Prevents a malicious actor from spawning processes with near-max-length
    // (32,767 char) command lines to exhaust the process buffer and dedup map.
    // Applied at the ingestion boundary before any storage or map insertion.
    if (imagePath.size() > MAX_PROCESS_STRING_LENGTH)
    {
        imagePath.resize(MAX_PROCESS_STRING_LENGTH);
    }
    if (commandLine.size() > MAX_PROCESS_STRING_LENGTH)
    {
        commandLine.resize(MAX_PROCESS_STRING_LENGTH);
    }

    // Check if this is a Windows service process (exclude from collection)
    if (IsServiceProcess(processId, parentProcessId))
    {
        std::lock_guard<std::mutex> lock(m_diagMutex);
        m_diagnostics.serviceProcessesExcluded++;
        return;
    }

    // Exclude processes from managed/OS directories (Windows, Program Files, etc.).
    // These binaries are already captured by the software inventory and would
    // produce noise without actionable CVE-correlation value.
    if (IsExcludedByPath(imagePath))
    {
        std::lock_guard<std::mutex> lock(m_diagMutex);
        m_diagnostics.managedPathProcessesExcluded++;
        return;
    }

    // Resolve user SID from the process token
    std::wstring userSid = GetProcessUserSid(processId);
    std::wstring username = ResolveSidToUsername(userSid);

    // Deduplication check
    if (IsDuplicateProcess(imagePath, commandLine, userSid))
    {
        std::lock_guard<std::mutex> lock(m_diagMutex);
        m_diagnostics.eventsDeduplicatedOut++;
        return;
    }

    // Add dedup entry
    AddDeduplicationEntry(imagePath, commandLine, userSid);

    // Build the process info record
    RunningProcessInfo info = {};
    info.imagePath = std::move(imagePath);
    info.commandLine = std::move(commandLine);
    info.userSid = std::move(userSid);
    info.username = std::move(username);
    info.processId = processId;
    info.parentProcessId = parentProcessId;
    info.parentImagePath = GetProcessImagePath(parentProcessId);
    info.firstSeenTimestamp = GetCurrentTimestamp();
    info.fileVersion = GetFileVersion(info.imagePath);
    info.isServiceProcess = false;

    // Add to observed processes (with buffer cap to prevent unbounded memory growth)
    {
        std::lock_guard<std::mutex> lock(m_processMutex);
        if (m_observedProcesses.size() < MAX_OBSERVED_PROCESSES)
        {
            m_observedProcesses.push_back(std::move(info));
        }
        else
        {
            // Buffer is full — log once per overflow burst to avoid log spam.
            // The oldest entries are preserved; new ones are dropped until collection resets the buffer.
            static thread_local ULONGLONG lastOverflowLogTick = 0;
            ULONGLONG now = GetTickCount64();
            if (now - lastOverflowLogTick > 60000ULL) // Log at most once per minute
            {
                LogError("[!] ProcessTracker: Observed process buffer is full ("
                         + std::to_string(MAX_OBSERVED_PROCESSES) +
                         " entries). New events will be dropped until next collection cycle.");
                lastOverflowLogTick = now;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_diagMutex);
        m_diagnostics.totalEventsReceived++;
    }

    // Periodically purge expired dedup entries
    PurgeExpiredDeduplicationEntries();
}

// ============================================================================
// Sysmon Fallback
// ============================================================================

bool ProcessTracker::IsSysmonEventLogAvailable() const
{
    // Use the Windows Event Log API (EvtOpenChannelConfig) to validate that the
    // Sysmon channel actually exists. The legacy OpenEventLogW API operates on a
    // different namespace and can return success for channels that don't exist as
    // proper Windows Event Log channels, causing EvtSubscribe to fail with 15007.
    LibraryHandle hWevtapi(LoadLibraryW(L"wevtapi.dll"));
    if (!hWevtapi)
    {
        return false;
    }

    using EvtOpenChannelConfigFn = EVT_HANDLE (WINAPI*)(EVT_HANDLE, LPCWSTR, DWORD);
    using EvtCloseFn = BOOL (WINAPI*)(EVT_HANDLE);

    auto fnEvtOpenChannelConfig = reinterpret_cast<EvtOpenChannelConfigFn>(
        GetProcAddress(hWevtapi.Get(), "EvtOpenChannelConfig"));
    auto fnEvtClose = reinterpret_cast<EvtCloseFn>(
        GetProcAddress(hWevtapi.Get(), "EvtClose"));

    if (fnEvtOpenChannelConfig == nullptr || fnEvtClose == nullptr)
    {
        return false;
    }

    EVT_HANDLE hChannel = fnEvtOpenChannelConfig(
        nullptr, L"Microsoft-Windows-Sysmon/Operational", 0);
    if (hChannel == nullptr)
    {
        return false;
    }

    fnEvtClose(hChannel);
    return true;
}

bool ProcessTracker::StartSysmonFallback()
{
    if (!IsSysmonEventLogAvailable())
    {
        LogError("[!] ProcessTracker: Sysmon event log not available on this system");
        std::lock_guard<std::mutex> lock(m_diagMutex);
        m_diagnostics.sysmonAvailable = false;
        return false;
    }

    LogError("[+] ProcessTracker: Starting Sysmon event log polling fallback");

    // Create a manual-reset event for the polling thread to signal whether
    // EvtSubscribe succeeded. This prevents the race where StartSysmonFallback()
    // returns true while the thread asynchronously discovers the channel is missing.
    m_sysmonReadyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (m_sysmonReadyEvent == nullptr)
    {
        LogError("[-] ProcessTracker: Failed to create Sysmon ready event, error: " +
                 std::to_string(GetLastError()));
        return false;
    }
    m_sysmonSubscribeSucceeded.store(false);

    m_sysmonPollThread = CreateThread(nullptr, 0, SysmonPollingThread, this, 0, nullptr);
    if (m_sysmonPollThread == nullptr)
    {
        LogError("[-] ProcessTracker: Failed to create Sysmon polling thread, error: " +
                 std::to_string(GetLastError()));
        CloseHandle(m_sysmonReadyEvent);
        m_sysmonReadyEvent = nullptr;
        return false;
    }

    // Wait for the polling thread to report whether EvtSubscribe succeeded.
    // 5-second timeout prevents indefinite blocking if the thread stalls.
    DWORD waitResult = WaitForSingleObject(m_sysmonReadyEvent, 5000);
    CloseHandle(m_sysmonReadyEvent);
    m_sysmonReadyEvent = nullptr;

    if (waitResult != WAIT_OBJECT_0 || !m_sysmonSubscribeSucceeded.load())
    {
        LogError("[-] ProcessTracker: Sysmon event subscription failed during startup");
        // Signal the thread to stop and wait for it to exit cleanly
        if (m_stopEvent != nullptr)
        {
            SetEvent(m_stopEvent);
        }
        WaitForSingleObject(m_sysmonPollThread, 5000);
        CloseHandle(m_sysmonPollThread);
        m_sysmonPollThread = nullptr;
        // Reset stop event for potential reuse (stop event is still needed by caller)
        if (m_stopEvent != nullptr)
        {
            ResetEvent(m_stopEvent);
        }
        std::lock_guard<std::mutex> lock(m_diagMutex);
        m_diagnostics.sysmonAvailable = false;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_diagMutex);
        m_diagnostics.sysmonAvailable = true;
        m_diagnostics.activeSource = ProcessTrackingSource::SysmonEventLog;
    }

    return true;
}

void ProcessTracker::StopSysmonFallback()
{
    if (m_sysmonPollThread != nullptr)
    {
        WaitForSingleObject(m_sysmonPollThread, 5000);
        CloseHandle(m_sysmonPollThread);
        m_sysmonPollThread = nullptr;
    }
}

DWORD WINAPI ProcessTracker::SysmonPollingThread(LPVOID lpParam)
{
    auto* tracker = static_cast<ProcessTracker*>(lpParam);
    if (tracker == nullptr)
        return 1;

    tracker->PollSysmonEvents();
    return 0;
}

void ProcessTracker::PollSysmonEvents()
{
    // Sysmon Event ID 1 = Process Create
    // We use the Windows Event Log API (evt* functions) to subscribe to
    // new events on the Sysmon/Operational channel.

    // Dynamically load wevtapi.dll to avoid hard dependency (RAII)
    LibraryHandle hWevtapi(LoadLibraryW(L"wevtapi.dll"));
    if (!hWevtapi)
    {
        LogError("[-] ProcessTracker: Failed to load wevtapi.dll for Sysmon fallback");
        m_sysmonSubscribeSucceeded.store(false);
        if (m_sysmonReadyEvent != nullptr)
        {
            SetEvent(m_sysmonReadyEvent);
        }
        return;
    }

    // Function pointer types for Windows Event Log API
    // Use explicit calling convention and winevt.h types
    using EvtSubscribeFn = EVT_HANDLE (WINAPI*)(
        EVT_HANDLE, HANDLE, LPCWSTR, LPCWSTR, EVT_HANDLE, PVOID,
        EVT_SUBSCRIBE_CALLBACK, DWORD);
    using EvtCloseFn = BOOL (WINAPI*)(EVT_HANDLE);

    auto fnEvtSubscribe = reinterpret_cast<EvtSubscribeFn>(
        GetProcAddress(hWevtapi.Get(), "EvtSubscribe"));
    auto fnEvtClose = reinterpret_cast<EvtCloseFn>(
        GetProcAddress(hWevtapi.Get(), "EvtClose"));

    if (fnEvtSubscribe == nullptr || fnEvtClose == nullptr)
    {
        LogError("[-] ProcessTracker: Failed to resolve wevtapi.dll functions");
        m_sysmonSubscribeSucceeded.store(false);
        if (m_sysmonReadyEvent != nullptr)
        {
            SetEvent(m_sysmonReadyEvent);
        }
        return;
    }

    // Subscribe to Sysmon process creation events (Event ID 1)
    // XPath query filters for Event ID 1 only
    const wchar_t* query = L"*[System[EventID=1]]";

    EVT_HANDLE hSubscription = fnEvtSubscribe(
        nullptr,            // Local machine
        m_stopEvent,        // Signal handle for cancellation
        L"Microsoft-Windows-Sysmon/Operational",
        query,
        nullptr,            // No bookmark
        nullptr,            // No callback context
        nullptr,            // No callback — we use signal-based
        EvtSubscribeToFutureEvents);

    if (hSubscription == nullptr)
    {
        DWORD err = GetLastError();
        LogError("[-] ProcessTracker: EvtSubscribe for Sysmon failed, error: " +
                 std::to_string(err) + " — " + GetWindowsErrorMessage(static_cast<LONG>(err)));
        // Signal the caller that subscription failed
        m_sysmonSubscribeSucceeded.store(false);
        if (m_sysmonReadyEvent != nullptr)
        {
            SetEvent(m_sysmonReadyEvent);
        }
        return;
    }

    LogError("[+] ProcessTracker: Sysmon event subscription active");

    // Signal the caller that subscription succeeded
    m_sysmonSubscribeSucceeded.store(true);
    if (m_sysmonReadyEvent != nullptr)
    {
        SetEvent(m_sysmonReadyEvent);
    }

    // Wait for stop signal — the subscription delivers events via the signal handle
    // For simplicity in the fallback path, we just keep the subscription open
    // and let the service's periodic collection gather the process snapshot
    WaitForSingleObject(m_stopEvent, INFINITE);

    fnEvtClose(hSubscription);
}

// ============================================================================
// Deduplication
// ============================================================================

bool ProcessTracker::IsDuplicateProcess(const std::wstring& imagePath,
                                        const std::wstring& commandLine,
                                        const std::wstring& userSid) const
{
    // Build composite key: imagePath|commandLine|userSid
    // Using the full key string avoids hash collisions that would silently
    // drop unique process events at enterprise scale (millions of events).
    std::wstring key = imagePath + L"|" + commandLine + L"|" + userSid;

    std::lock_guard<std::mutex> lock(m_dedupMutex);
    auto it = m_deduplicationMap.find(key);
    if (it == m_deduplicationMap.end())
    {
        return false;
    }

    // Check if the entry has expired
    ULONGLONG now = GetTickCount64();
    if (now > it->second)
    {
        // Entry expired — not a duplicate
        return false;
    }

    return true;
}

void ProcessTracker::AddDeduplicationEntry(const std::wstring& imagePath,
                                           const std::wstring& commandLine,
                                           const std::wstring& userSid)
{
    std::wstring key = imagePath + L"|" + commandLine + L"|" + userSid;
    ULONGLONG expiry = GetTickCount64() + DEDUP_WINDOW_MS;

    std::lock_guard<std::mutex> lock(m_dedupMutex);

    // Cap dedup map size to prevent unbounded memory growth from an attacker
    // spawning millions of unique short-lived processes with unique cmdlines.
    // When full, skip insertion — the process will still be recorded in the
    // observed buffer, just without dedup protection until the map is purged.
    if (m_deduplicationMap.size() >= MAX_OBSERVED_PROCESSES)
    {
        return;
    }

    m_deduplicationMap[key] = expiry;
}

void ProcessTracker::PurgeExpiredDeduplicationEntries()
{
    // Only purge periodically (every ~100 events) to avoid lock contention
    // on the ETW callback hot path.
    static thread_local DWORD purgeCounter = 0;
    if (++purgeCounter % 100 != 0)
    {
        return;
    }

    ULONGLONG now = GetTickCount64();

    std::lock_guard<std::mutex> lock(m_dedupMutex);
    for (auto it = m_deduplicationMap.begin(); it != m_deduplicationMap.end(); )
    {
        if (now > it->second)
        {
            it = m_deduplicationMap.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

// ============================================================================
// Service Process Exclusion
// ============================================================================

bool ProcessTracker::IsServiceProcess(DWORD processId, DWORD parentProcessId) const
{
    // Ensure cached PIDs are fresh
    RefreshServiceHostPids();

    // A process is considered a service process if its parent is services.exe
    // (direct SCM-launched services) or any svchost.exe instance (shared-process
    // service hosts that spawn child worker processes).
    if (m_servicesPid != 0 && parentProcessId == m_servicesPid)
    {
        return true;
    }

    if (m_svchostPids.count(parentProcessId) > 0)
    {
        return true;
    }

    return false;
}

void ProcessTracker::RefreshServiceHostPids() const
{
    // Check if cache is still valid (refresh every SERVICES_PID_CACHE_TTL_MS).
    // services.exe PID is stable under normal operation; svchost.exe PIDs are
    // mostly stable but can change when service groups start/stop. We refresh
    // periodically for correctness in edge cases (failover, recovery, etc.).
    if (m_serviceHostPidsCached)
    {
        ULONGLONG now = GetTickCount64();
        if (now < m_serviceHostPidsCacheExpiry)
        {
            return;
        }
        // Cache expired — fall through to refresh
        m_serviceHostPidsCached = false;
    }

    m_servicesPid = 0;
    m_svchostPids.clear();

    // Single snapshot to find both services.exe and all svchost.exe instances (RAII)
    SnapshotHandle hSnapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!hSnapshot)
    {
        return;
    }

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(hSnapshot.Get(), &pe))
    {
        do
        {
            if (_wcsicmp(pe.szExeFile, L"services.exe") == 0)
            {
                m_servicesPid = pe.th32ProcessID;
            }
            else if (_wcsicmp(pe.szExeFile, L"svchost.exe") == 0)
            {
                m_svchostPids.insert(pe.th32ProcessID);
            }
        } while (Process32NextW(hSnapshot.Get(), &pe));
    }

    m_serviceHostPidsCached = true;
    m_serviceHostPidsCacheExpiry = GetTickCount64() + SERVICES_PID_CACHE_TTL_MS;
}

void ProcessTracker::InitExcludedPathPrefixes()
{
    m_excludedPathPrefixes.clear();

    // Helper: resolve a directory path, lowercase it, ensure trailing backslash,
    // and add to the exclusion list. Logs a warning if resolution fails.
    auto AddPrefix = [this](const std::wstring& path, const char* label) {
        if (path.empty())
        {
            LogError(std::string("[!] ProcessTracker: Could not resolve ") + label +
                     " directory for path exclusion");
            return;
        }
        std::wstring lower = path;
        CharLowerW(lower.data());
        if (lower.back() != L'\\')
        {
            lower.push_back(L'\\');
        }
        m_excludedPathPrefixes.push_back(std::move(lower));
    };

    // Resolve the Windows directory (e.g., C:\Windows)
    {
        UINT len = GetWindowsDirectoryW(nullptr, 0);
        if (len > 0)
        {
            std::wstring winDir(static_cast<size_t>(len), L'\0');
            UINT copied = GetWindowsDirectoryW(winDir.data(), len);
            if (copied > 0 && copied < len)
            {
                winDir.resize(static_cast<size_t>(copied));
                AddPrefix(winDir, "Windows");
            }
        }
    }

    // Resolve Program Files directories from environment variables.
    // Using environment variables rather than SHGetKnownFolderPath because:
    // 1. ProgramFiles / ProgramFiles(x86) are set by the OS at boot
    // 2. No COM dependency (SHGetKnownFolderPath requires COM initialization)
    // 3. Works identically for SYSTEM account and user accounts
    {
        wchar_t buffer[MAX_PATH] = {};
        DWORD len = GetEnvironmentVariableW(L"ProgramFiles", buffer, MAX_PATH);
        if (len > 0 && len < MAX_PATH)
        {
            AddPrefix(std::wstring(buffer, len), "ProgramFiles");
        }
    }
    {
        wchar_t buffer[MAX_PATH] = {};
        DWORD len = GetEnvironmentVariableW(L"ProgramFiles(x86)", buffer, MAX_PATH);
        if (len > 0 && len < MAX_PATH)
        {
            AddPrefix(std::wstring(buffer, len), "ProgramFiles(x86)");
        }
    }
    // NOTE: %ProgramData% is intentionally NOT excluded.
    // Unlike %ProgramFiles% (admin-write-only ACL), ProgramData is world-writable
    // by default. Attackers, scripts, and automation tools commonly drop portable
    // binaries there. Excluding it would create a blind spot for:
    //   - Malware staging and persistence (e.g., C:\ProgramData\malware.exe)
    //   - Shadow IT portable applications
    //   - Scripted deployments bypassing MSI/AppX installers
    // Software properly installed via MSI under ProgramData is already captured
    // by the inventory; arbitrary executables are not, and must be tracked.

    LogError("[+] ProcessTracker: Initialized " + std::to_string(m_excludedPathPrefixes.size()) +
             " managed path exclusion prefixes");
    for (const auto& prefix : m_excludedPathPrefixes)
    {
        LogWideStringAsUtf8("[+] ProcessTracker:   Exclude prefix: ", prefix);
    }
}

bool ProcessTracker::IsExcludedByPath(const std::wstring& imagePath) const
{
    if (imagePath.empty() || m_excludedPathPrefixes.empty())
    {
        return false;
    }

    // Lowercase the image path for case-insensitive comparison.
    // Stack buffer covers ~99% of paths without heap allocation.
    // CharLowerBuffW is locale-aware and handles all Unicode correctly.
    std::wstring lower = imagePath;
    CharLowerBuffW(lower.data(), static_cast<DWORD>(lower.size()));

    for (const auto& prefix : m_excludedPathPrefixes)
    {
        if (lower.size() >= prefix.size() &&
            lower.compare(0, prefix.size(), prefix) == 0)
        {
            return true;
        }
    }

    return false;
}

std::wstring ProcessTracker::GetProcessUserSid(DWORD processId)
{
    WinHandle hProcess(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
    if (!hProcess)
    {
        return {};
    }

    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(hProcess.Get(), TOKEN_QUERY, &rawToken))
    {
        return {};
    }
    WinHandle hToken(rawToken);

    // Sizing call: must return ERROR_INSUFFICIENT_BUFFER with a non-zero size
    DWORD tokenInfoSize = 0;
    if (!GetTokenInformation(hToken.Get(), TokenUser, nullptr, 0, &tokenInfoSize))
    {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || tokenInfoSize == 0)
        {
            return {};
        }
    }

    std::vector<BYTE> tokenBuffer(tokenInfoSize);
    auto* pTokenUser = reinterpret_cast<TOKEN_USER*>(tokenBuffer.data());
    if (!GetTokenInformation(hToken.Get(), TokenUser, pTokenUser, tokenInfoSize, &tokenInfoSize))
    {
        return {};
    }

    LPWSTR sidString = nullptr;
    std::wstring result;
    if (ConvertSidToStringSidW(pTokenUser->User.Sid, &sidString))
    {
        result = sidString;
        LocalFree(sidString);
    }

    return result;
}

std::wstring ProcessTracker::ResolveSidToUsername(const std::wstring& sidString)
{
    if (sidString.empty())
    {
        return L"Unknown";
    }

    PSID pSid = nullptr;
    if (!ConvertStringSidToSidW(sidString.c_str(), &pSid))
    {
        return L"Unknown";
    }
    // RAII: ConvertStringSidToSidW allocates via LocalAlloc
    LocalMemGuard sidGuard(pSid);

    wchar_t nameBuffer[256] = {};
    DWORD nameSize = _countof(nameBuffer);
    wchar_t domainBuffer[256] = {};
    DWORD domainSize = _countof(domainBuffer);
    SID_NAME_USE sidType = SidTypeUnknown;

    std::wstring result = L"Unknown";
    if (LookupAccountSidW(nullptr, pSid, nameBuffer, &nameSize, domainBuffer, &domainSize, &sidType))
    {
        if (domainBuffer[0] != L'\0')
        {
            result = std::wstring(domainBuffer) + L"\\" + nameBuffer;
        }
        else
        {
            result = nameBuffer;
        }
    }

    return result;
}

std::wstring ProcessTracker::GetProcessImagePath(DWORD processId)
{
    if (processId == 0)
    {
        return {};
    }

    WinHandle hProcess(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
    if (!hProcess)
    {
        return {};
    }

    // First attempt with stack buffer (covers ~99% of paths)
    wchar_t stackBuffer[MAX_PATH] = {};
    DWORD pathSize = MAX_PATH;

    if (QueryFullProcessImageNameW(hProcess.Get(), 0, stackBuffer, &pathSize))
    {
        return std::wstring(stackBuffer, pathSize);
    }

    // If MAX_PATH was insufficient, retry with a larger heap buffer.
    // Extended-length paths (\\?\) can reach 32,767 characters in enterprise
    // environments with deep directory structures or long filenames.
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
    {
        constexpr DWORD extendedMaxPath = 32768;
        std::wstring largeBuffer(extendedMaxPath, L'\0');
        pathSize = extendedMaxPath;

        if (QueryFullProcessImageNameW(hProcess.Get(), 0, largeBuffer.data(), &pathSize))
        {
            largeBuffer.resize(pathSize);
            return largeBuffer;
        }
    }

    return {};
}

std::wstring ProcessTracker::GetCurrentTimestamp()
{
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    struct tm timeinfo = {};

    if (localtime_s(&timeinfo, &time) != 0)
    {
        return L"UNKNOWN";
    }

    std::wostringstream wss;
    wss << std::put_time(&timeinfo, L"%Y-%m-%dT%H:%M:%S");
    return wss.str();
}

std::wstring ProcessTracker::GetFileVersion(const std::wstring& filePath)
{
    if (filePath.empty())
    {
        return {};
    }

    // Map the PE into user-mode address space as raw data.
    // LOAD_LIBRARY_AS_IMAGE_RESOURCE: map sections at virtual addresses so
    //   FindResourceW can walk the resource directory tree correctly.
    // LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE: prevent concurrent modification
    //   of the mapping while we read from it.
    // No DllMain execution, no import resolution, no compatibility shims.
    LibraryHandle hModule(LoadLibraryExW(
        filePath.c_str(),
        nullptr,
        LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE));

    if (!hModule)
    {
        return {};
    }

    // Locate the RT_VERSION resource (resource type 16, resource ID 1)
    const HRSRC hResInfo = FindResourceW(hModule.Get(), MAKEINTRESOURCEW(1), RT_VERSION);
    if (hResInfo == nullptr)
    {
        return {};
    }

    const DWORD resSize = SizeofResource(hModule.Get(), hResInfo);
    if (resSize == 0)
    {
        return {};
    }

    const HGLOBAL hResData = LoadResource(hModule.Get(), hResInfo);
    if (hResData == nullptr)
    {
        return {};
    }

    const void* pRawData = LockResource(hResData);
    if (pRawData == nullptr)
    {
        return {};
    }

    // Copy into a writable buffer. The mapped resource pages are read-only,
    // and VerQueryValueW may write alignment fixups into the buffer.
    std::vector<BYTE> versionData(
        static_cast<const BYTE*>(pRawData),
        static_cast<const BYTE*>(pRawData) + resSize);

    // Parse VS_FIXEDFILEINFO from the raw resource data.
    // VerQueryValueW is a pure in-memory parser -- no version shim applied here.
    VS_FIXEDFILEINFO* pFixedInfo = nullptr;
    UINT fixedInfoSize = 0;

    if (!VerQueryValueW(versionData.data(), L"\\",
                        reinterpret_cast<LPVOID*>(&pFixedInfo), &fixedInfoSize))
    {
        return {};
    }

    if (pFixedInfo == nullptr || fixedInfoSize < sizeof(VS_FIXEDFILEINFO))
    {
        return {};
    }

    // Validate the magic signature (0xFEEF04BD)
    if (pFixedInfo->dwSignature != VS_FFI_SIGNATURE)
    {
        return {};
    }

    // Extract Major.Minor.Build.Revision from dwFileVersionMS / dwFileVersionLS
    const DWORD major    = HIWORD(pFixedInfo->dwFileVersionMS);
    const DWORD minor    = LOWORD(pFixedInfo->dwFileVersionMS);
    const DWORD build    = HIWORD(pFixedInfo->dwFileVersionLS);
    const DWORD revision = LOWORD(pFixedInfo->dwFileVersionLS);

    return std::to_wstring(major) + L"." +
           std::to_wstring(minor) + L"." +
           std::to_wstring(build) + L"." +
           std::to_wstring(revision);
}

// ============================================================================
// Snapshot-based Collection (point-in-time enumeration)
// ============================================================================

std::vector<RunningProcessInfo> SnapshotRunningProcesses(
const std::vector<DWORD>& serviceProcessIds,
const ProcessTracker* tracker)
{
    std::vector<RunningProcessInfo> processes;

    // Build a set of service PIDs for O(1) lookup
    std::unordered_set<DWORD> servicePidSet(serviceProcessIds.begin(), serviceProcessIds.end());

    // Take a single atomic snapshot of all processes.
    // Using one snapshot for both services.exe lookup and enumeration
    // eliminates the TOCTOU window of two separate snapshots.
    SnapshotHandle hSnapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!hSnapshot)
    {
        LogError("[-] SnapshotRunningProcesses: CreateToolhelp32Snapshot failed, error: " +
                 std::to_string(GetLastError()));
        return processes;
    }

    // First pass: collect all PROCESSENTRY32W entries into a vector and find
    // services.exe PID and all svchost.exe PIDs from the same snapshot.
    // This allows a single snapshot walk followed by filtering, avoiding iterator invalidation.
    struct ProcessEntry
    {
        DWORD processId;
        DWORD parentProcessId;
    };
    std::vector<ProcessEntry> allEntries;
    DWORD servicesPid = 0;
    std::unordered_set<DWORD> svchostPids;

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(hSnapshot.Get(), &pe))
    {
        do
        {
            if (_wcsicmp(pe.szExeFile, L"services.exe") == 0)
            {
                servicesPid = pe.th32ProcessID;
            }
            else if (_wcsicmp(pe.szExeFile, L"svchost.exe") == 0)
            {
                svchostPids.insert(pe.th32ProcessID);
            }

            allEntries.push_back({ pe.th32ProcessID, pe.th32ParentProcessID });

        } while (Process32NextW(hSnapshot.Get(), &pe));
    }

    // Second pass: filter and build RunningProcessInfo for non-service processes
    for (const auto& entry : allEntries)
    {
        DWORD pid = entry.processId;

        // Skip PID 0 (System Idle) and PID 4 (System)
        if (pid == 0 || pid == 4)
        {
            continue;
        }

        // Skip known service processes (by PID from SCM enumeration)
        if (servicePidSet.count(pid) > 0)
        {
            continue;
        }

        // Skip processes whose parent is services.exe or svchost.exe.
        // services.exe spawns standalone service processes; svchost.exe hosts
        // shared-process services and spawns worker child processes.
        if (servicesPid != 0 && entry.parentProcessId == servicesPid)
        {
            continue;
        }

        if (svchostPids.count(entry.parentProcessId) > 0)
        {
            continue;
        }

        // Get full image path
        std::wstring imagePath = ProcessTracker::GetProcessImagePath(pid);
        if (imagePath.empty())
        {
            // Cannot read — likely a protected/kernel process
            continue;
        }

        // Skip processes from managed/OS directories (Windows, Program Files, etc.).
        // These are already captured by the software inventory.
        if (tracker != nullptr && tracker->IsExcludedByPath(imagePath))
        {
            continue;
        }

        // Truncate attacker-controlled strings to cap memory usage (same as ETW path)
        if (imagePath.size() > MAX_PROCESS_STRING_LENGTH)
        {
            imagePath.resize(MAX_PROCESS_STRING_LENGTH);
        }

        // Get process user SID
        std::wstring userSid = ProcessTracker::GetProcessUserSid(pid);
        std::wstring username = ProcessTracker::ResolveSidToUsername(userSid);

        // Snapshot mode: record image path as command line (best-effort fallback).
        // Full command line retrieval from another process requires reading the PEB
        // via NtQueryInformationProcess, which may fail for protected processes.
        std::wstring commandLine = imagePath;
        if (commandLine.size() > MAX_PROCESS_STRING_LENGTH)
        {
            commandLine.resize(MAX_PROCESS_STRING_LENGTH);
        }

        RunningProcessInfo info = {};
        info.imagePath = std::move(imagePath);
        info.commandLine = std::move(commandLine);
        info.userSid = std::move(userSid);
        info.username = std::move(username);
        info.processId = pid;
        info.parentProcessId = entry.parentProcessId;
        info.parentImagePath = ProcessTracker::GetProcessImagePath(entry.parentProcessId);
        info.firstSeenTimestamp = ProcessTracker::GetCurrentTimestamp();
        info.fileVersion = ProcessTracker::GetFileVersion(info.imagePath);
        info.isServiceProcess = false;

        processes.push_back(std::move(info));
    }

    LogError("[+] SnapshotRunningProcesses: Captured " + std::to_string(processes.size()) +
             " non-service processes");

    return processes;
}
