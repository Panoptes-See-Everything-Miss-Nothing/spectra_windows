#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>

// ============================================================================
// Patch Fingerprint Data Structures
// ============================================================================

// File version of a single OS or Microsoft binary checked for patch compliance.
// The agent collects on-disk file versions; the backend compares them against
// expected post-patch versions from MSRC feeds to determine missing patches.
struct PatchFileVersionInfo
{
    std::wstring relativePath;      // e.g., "ntoskrnl.exe", "drivers\\cng.sys"
    std::wstring fullPath;          // Resolved absolute path
    std::wstring fileVersion;       // PE RT_VERSION (e.g., "10.0.22621.4751")
    std::wstring category;          // Grouping: "kernel", "network", "crypto", "win32k", etc.
    bool fileExists = false;        // False if file not found (optional component)
    bool versionReadSuccess = false;// False if file exists but version resource unreadable
};

// System-level patch state metadata.
// Gives the backend the OS identity and pending-operation context needed
// to select the correct patch baseline for compliance comparison.
struct PatchSystemState
{
    // Exact OS build: Major.Minor.Build.UBR (e.g., "10.0.22621.4751")
    // UBR (Update Build Revision) is the single most important data point — it
    // tells the backend exactly which CU is applied without individual file checks.
    std::wstring osBuild;

    // OS edition and architecture for correct patch baseline selection
    std::wstring osEdition;                 // e.g., "Microsoft Windows 11 Pro 64-bit"
    std::wstring processorArchitecture;     // e.g., "x64", "ARM64"

    // Reboot state: patches may be staged but not active until reboot.
    bool rebootRequired = false;

    // CBS (Component-Based Servicing) pending operations.
    // More granular than rebootRequired — tells the backend what kind of
    // operation is pending.
    bool cbsRebootPending = false;
    bool windowsUpdateRebootRequired = false;

    // WUA configuration: determines the scope of available updates.
    // "Windows Update" = public Microsoft Update (full catalog)
    // "Windows Server Update Services" = WSUS (only approved updates visible)
    // Empty = could not determine
    std::wstring updateServiceSource;

    // Last time WUA successfully checked for updates (ISO 8601).
    // If stale (>7 days), the backend should rely on file-version comparison.
    std::wstring lastUpdateCheckTime;
};

// Microsoft Office / M365 Apps patch state.
// Office has its own servicing channel (Click-to-Run) and is patched
// independently from the OS.
struct OfficePatchState
{
    bool clickToRunDetected = false;
    std::wstring versionToReport;           // Registry: VersionToReport (e.g., "16.0.18324.20194")
    std::wstring updateChannel;             // e.g., "Current", "MonthlyEnterprise", "Semi-Annual"
    std::wstring platform;                  // "x64" or "x86"
    std::wstring installPath;               // e.g., "C:\\Program Files\\Microsoft Office\\root\\Office16"

    // Key Office binary file versions (ground truth)
    std::vector<PatchFileVersionInfo> binaryVersions;
};

// Entry in the backend-provided patch manifest.
struct PatchManifestEntry
{
    std::wstring relativePath;
    std::wstring category;
};

// Patch manifest loaded from disk or defaults.
struct PatchManifest
{
    std::wstring manifestVersion;
    std::wstring generatedDate;
    std::vector<PatchManifestEntry> files;
};

// ============================================================================
// Collector
// ============================================================================

// Collects a rich "patch fingerprint" that the backend correlates against MSRC
// feeds to determine compliance. The agent collects facts; the backend decides.
//
// Data collected:
//   1. OS build (Major.Minor.Build.UBR)
//   2. File versions of ~35 core OS binaries
//   3. Office/M365 Click-to-Run version + key binary versions
//   4. Reboot/CBS pending state
//   5. WUA service config
//
// File list sources (merged, deduplicated):
//   Built-in defaults (~35 core files + ~8 Office files)
//   Backend manifest from patch_manifest.json (extensible)
//
// Performance: ~1-5 seconds (targeted file checks, no full directory scan)
// Privilege: Standard read for file versions. SYSTEM for WUA COM + registry.
class PatchTuesdayCollector
{
public:
    PatchTuesdayCollector() = default;
    ~PatchTuesdayCollector() = default;

    PatchTuesdayCollector(const PatchTuesdayCollector&) = delete;
    PatchTuesdayCollector& operator=(const PatchTuesdayCollector&) = delete;
    PatchTuesdayCollector(PatchTuesdayCollector&&) = delete;
    PatchTuesdayCollector& operator=(PatchTuesdayCollector&&) = delete;

    // Collect file versions for all target OS binaries.
    std::vector<PatchFileVersionInfo> CollectFileVersions();

    // Collect system-level patch state metadata.
    PatchSystemState CollectSystemState();

    // Collect Office / M365 Apps patch state.
    OfficePatchState CollectOfficePatchState();

private:
    // Built-in default file list (core OS binaries patched nearly every month).
    static std::vector<std::pair<std::wstring, std::wstring>> GetDefaultFileList();

    // Load backend-provided manifest from the config directory.
    static PatchManifest LoadManifestFromDisk();

    // Merge manifest entries into the file list, deduplicating by path.
    static void MergeManifestIntoFileList(
        std::vector<std::pair<std::wstring, std::wstring>>& fileList,
        const PatchManifest& manifest);

    // Resolve relative path to absolute path under System32.
    static std::wstring ResolveSystemFilePath(const std::wstring& relativePath);

    // Read PE file version from RT_VERSION resource (shim-free).
    static std::wstring ReadFileVersion(const std::wstring& filePath);

    // Read UBR from registry.
    static DWORD ReadUBR();

    // Query CBS and WU pending reboot state from registry.
    static bool QueryCbsRebootPending();
    static bool QueryWindowsUpdateRebootPending();

    // Query ISystemInformation::RebootRequired (authoritative COM source).
    static bool QueryRebootRequired();

    // Query WUA for the configured update service source.
    static std::wstring QueryUpdateServiceSource();

    // Query IAutomaticUpdates for last successful check time.
    static std::wstring QueryLastUpdateCheckTime();

    // Detect Office Click-to-Run installation and read configuration.
    static OfficePatchState DetectOfficeClickToRun();
};

// Generate the complete MSPT inventory JSON string.
std::string GenerateMsptInventoryJSON();
