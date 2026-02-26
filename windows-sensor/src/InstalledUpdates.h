#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>

// Represents a single installed Windows update (KB)
struct InstalledUpdate
{
    std::wstring title;             // e.g., "2024-11 Cumulative Update for Windows 11..."
    std::wstring updateId;          // WUA UpdateIdentity GUID
    int revisionNumber = 0;         // UpdateIdentity revision
    std::wstring description;       // Update description text
    std::vector<std::wstring> kbArticleIds;   // KB numbers (e.g., "5044285")
    std::vector<std::wstring> categories;     // Category names (e.g., "Security Updates")
    std::wstring supportUrl;        // Link to KB article
    std::wstring msrcSeverity;      // MSRC severity (Critical, Important, etc.)
    std::wstring installedDate;     // ISO 8601 timestamp from update history
    int operationResultCode = 0;    // 0=NotStarted, 1=InProgress, 2=Succeeded, 3=SucceededWithErrors, 4=Failed, 5=Aborted
};

// RAII collector that enumerates installed Windows updates via the
// Windows Update Agent (WUA) COM API. Handles COM lifetime internally.
class InstalledUpdatesCollector
{
public:
    InstalledUpdatesCollector() = default;
    ~InstalledUpdatesCollector() = default;

    // Non-copyable, non-movable (stateless — call Collect() each time)
    InstalledUpdatesCollector(const InstalledUpdatesCollector&) = delete;
    InstalledUpdatesCollector& operator=(const InstalledUpdatesCollector&) = delete;
    InstalledUpdatesCollector(InstalledUpdatesCollector&&) = delete;
    InstalledUpdatesCollector& operator=(InstalledUpdatesCollector&&) = delete;

    // Enumerate all installed updates on the local machine.
    // Returns an empty vector on failure (errors are logged internally).
    std::vector<InstalledUpdate> Collect();

private:
    // Query WUA for installed updates (IsInstalled=1) and extract metadata.
    static bool QueryInstalledUpdates(std::vector<InstalledUpdate>& updates);

    // Enrich updates with installation date from update history.
    static void EnrichWithHistory(std::vector<InstalledUpdate>& updates);

    // Convert a COM DATE (VARIANT time) to an ISO 8601 string.
    static std::wstring DateToIso8601(double variantDate);
};
