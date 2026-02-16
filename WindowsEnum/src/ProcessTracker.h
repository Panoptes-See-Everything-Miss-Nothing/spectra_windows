#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <string>
#include <vector>
#include <mutex>
#include <unordered_set>
#include <unordered_map>
#include <atomic>

// Represents a single observed process execution.
// Collected via ETW (Microsoft-Windows-Kernel-Process) or Sysmon fallback.
struct RunningProcessInfo
{
    std::wstring imagePath;             // Full path to the executable image
    std::wstring commandLine;           // Full command line including arguments
    std::wstring userSid;               // SID of the user who launched the process
    std::wstring username;              // Resolved username (best-effort)
    DWORD processId = 0;               // PID of the process
    DWORD parentProcessId = 0;         // PID of the parent process
    std::wstring parentImagePath;       // Image path of the parent process (best-effort)
    std::wstring firstSeenTimestamp;    // ISO 8601 timestamp when first observed
    bool isServiceProcess = false;      // True if process is a known Windows service
};

// Tracking source indicator for diagnostics
enum class ProcessTrackingSource
{
    None = 0,
    EtwKernelProcess,      // Primary: Microsoft-Windows-Kernel-Process ETW provider
    SysmonEventLog,        // Fallback: Sysmon operational event log
    Disabled               // Administratively disabled via config
};

// Diagnostic telemetry about the tracker state
struct ProcessTrackerDiagnostics
{
    ProcessTrackingSource activeSource = ProcessTrackingSource::None;
    ULONG etwSessionStartResult = 0;           // HRESULT/Win32 error from StartTrace
    ULONG etwConsumerOpenResult = 0;            // Error from OpenTrace
    bool sysmonAvailable = false;               // Whether Sysmon event log exists
    DWORD totalEventsReceived = 0;              // Total process-start events received
    DWORD eventsDeduplicatedOut = 0;            // Events dropped by dedup filter
    DWORD serviceProcessesExcluded = 0;         // Events excluded as service processes
    std::wstring lastErrorMessage;              // Human-readable last error
};

// Real-time process execution tracker using ETW with Sysmon fallback.
// Designed to run as a background thread within the Spectra service.
//
// Thread safety: All public methods are thread-safe.
// Privilege requirement: SYSTEM account (required for kernel ETW session).
class ProcessTracker
{
public:
    ProcessTracker();
    ~ProcessTracker();

    // Non-copyable, non-movable (owns ETW session and thread handles)
    ProcessTracker(const ProcessTracker&) = delete;
    ProcessTracker& operator=(const ProcessTracker&) = delete;
    ProcessTracker(ProcessTracker&&) = delete;
    ProcessTracker& operator=(ProcessTracker&&) = delete;

    // Start the process tracking background thread.
    // Returns true if tracking was started (ETW or Sysmon fallback).
    // Returns false if disabled by config or all sources failed.
    bool Start();

    // Stop the process tracking and clean up resources.
    // Safe to call multiple times or if not started.
    void Stop();

    // Collect and return all unique observed processes since last collection.
    // Clears the internal buffer after copying.
    // Thread-safe: called from the data-collection worker thread.
    std::vector<RunningProcessInfo> CollectAndReset();

    // Get diagnostic telemetry about the tracker state.
    ProcessTrackerDiagnostics GetDiagnostics() const;

    // Check if the tracker is currently active.
    bool IsRunning() const;

private:
    // ETW session management
    bool StartEtwSession();
    void StopEtwSession();

    // Sysmon event log fallback
    bool StartSysmonFallback();
    void StopSysmonFallback();
    bool IsSysmonEventLogAvailable() const;

    // ETW callback (static, called by ETW infrastructure)
    static void WINAPI EtwEventCallback(PEVENT_RECORD pEventRecord);

    // Process an ETW event record (instance method, called from static callback)
    void HandleProcessStartEvent(PEVENT_RECORD pEventRecord);

    // Sysmon event log polling thread
    static DWORD WINAPI SysmonPollingThread(LPVOID lpParam);
    void PollSysmonEvents();

    // ETW consumer thread (ProcessTrace blocks, needs dedicated thread)
    static DWORD WINAPI EtwConsumerThread(LPVOID lpParam);

    // Deduplication: returns true if this process was already seen recently
    bool IsDuplicateProcess(const std::wstring& imagePath, const std::wstring& commandLine,
                            const std::wstring& userSid) const;

    // Add a dedup entry for a newly observed process
    void AddDeduplicationEntry(const std::wstring& imagePath, const std::wstring& commandLine,
                               const std::wstring& userSid);

    // Purge expired deduplication entries
    void PurgeExpiredDeduplicationEntries();

    // Check if a process is a known Windows service by its PID or parent
    bool IsServiceProcess(DWORD processId, DWORD parentProcessId) const;

    // Find the PID of services.exe (cached)
    DWORD GetServicesPid() const;

public:
    // These helpers are public so that SnapshotRunningProcesses() can reuse them.

    // Resolve process user SID from process token
    static std::wstring GetProcessUserSid(DWORD processId);

    // Resolve SID string to username (best-effort)
    static std::wstring ResolveSidToUsername(const std::wstring& sidString);

    // Get the image path for a given PID (best-effort, may fail for exited processes)
    static std::wstring GetProcessImagePath(DWORD processId);

    // Build ISO 8601 timestamp string
    static std::wstring GetCurrentTimestamp();

private:

    // --- Data members ---

    // Observed processes buffer (protected by mutex)
    mutable std::mutex m_processMutex;
    std::vector<RunningProcessInfo> m_observedProcesses;

    // Deduplication map: full composite key (imagePath|cmdLine|userSid) -> expiry tick.
    // Uses the full key string instead of a hash to avoid hash collisions that would
    // silently drop unique process events at enterprise scale.
    mutable std::mutex m_dedupMutex;
    std::unordered_map<std::wstring, ULONGLONG> m_deduplicationMap;

    // Dedup window: processes with same (path + cmdLine + userSid) within this
    // interval are considered duplicates. Default 1 hour (configurable).
    static constexpr ULONGLONG DEDUP_WINDOW_MS = 3600000ULL;

    // ETW session handles
    TRACEHANDLE m_sessionHandle = 0;
    TRACEHANDLE m_consumerHandle = INVALID_PROCESSTRACE_HANDLE;

    // Thread handles
    HANDLE m_etwConsumerThread = nullptr;
    HANDLE m_sysmonPollThread = nullptr;

    // Stop signal
    HANDLE m_stopEvent = nullptr;

    // Diagnostics (atomic where possible, mutex-protected otherwise)
    mutable std::mutex m_diagMutex;
    ProcessTrackerDiagnostics m_diagnostics = {};

    std::atomic<bool> m_isRunning{ false };

    // Cached services.exe PID with TTL-based expiry.
    // services.exe PID is stable under normal operation, but we refresh
    // periodically (every 5 minutes) for correctness in edge cases.
    mutable DWORD m_servicesPid = 0;
    mutable bool m_servicesPidCached = false;
    mutable ULONGLONG m_servicesPidCacheExpiry = 0;

    // ETW session name (must be unique system-wide)
    static constexpr const wchar_t* ETW_SESSION_NAME = L"PanoptesSpectraProcessTracker";

    // Microsoft-Windows-Kernel-Process provider GUID
    // {22FB2CD6-0E7B-422B-A0C7-2FAD1FD0E716}
    static const GUID s_kernelProcessProviderGuid;

    // Global instance pointer for static ETW callback routing
    static ProcessTracker* s_instance;
};

// Snapshot-based collection: enumerate currently running processes.
// Used as a one-time collection during GenerateJSON() to capture processes
// that were already running before the ETW session started.
// Excludes Windows service processes.
std::vector<RunningProcessInfo> SnapshotRunningProcesses(
    const std::vector<DWORD>& serviceProcessIds);
