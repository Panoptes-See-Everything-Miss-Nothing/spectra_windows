#include "PatchTuesdayInventory.h"
#include "OsVersionInfo.h"
#include "Utils/Utils.h"
#include "Service/ServiceConfig.h"
#include "Service/MachineId.h"
#include <wuapi.h>
#include <comdef.h>
#include <oleauto.h>
#include <winver.h>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <fstream>
#include <algorithm>
#include <unordered_set>

#pragma comment(lib, "version.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

// ============================================================================
// RAII Helpers (scoped to this translation unit)
// ============================================================================

namespace
{
    // RAII COM scope for WUA queries.
    class PtComScope
    {
    public:
        PtComScope() : m_initialized(false)
        {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (SUCCEEDED(hr))
                m_initialized = true;
            else if (hr != RPC_E_CHANGED_MODE)
                LogError("[-] PatchTuesday: CoInitializeEx failed, HRESULT: 0x" +
                         std::to_string(static_cast<unsigned long>(hr)));
        }
        ~PtComScope() { if (m_initialized) CoUninitialize(); }
        PtComScope(const PtComScope&) = delete;
        PtComScope& operator=(const PtComScope&) = delete;
    private:
        bool m_initialized;
    };

    // RAII HMODULE wrapper for LoadLibraryExW.
    class PtModuleHandle
    {
    public:
        explicit PtModuleHandle(HMODULE h = nullptr) noexcept : m_handle(h) {}
        ~PtModuleHandle() noexcept { if (m_handle) FreeLibrary(m_handle); }
        PtModuleHandle(const PtModuleHandle&) = delete;
        PtModuleHandle& operator=(const PtModuleHandle&) = delete;
        HMODULE Get() const noexcept { return m_handle; }
        explicit operator bool() const noexcept { return m_handle != nullptr; }
    private:
        HMODULE m_handle;
    };

    // RAII wrapper for registry keys.
    class PtRegKeyHandle
    {
    public:
        explicit PtRegKeyHandle(HKEY h = nullptr) noexcept : m_key(h) {}
        ~PtRegKeyHandle() noexcept { if (m_key) RegCloseKey(m_key); }
        PtRegKeyHandle(const PtRegKeyHandle&) = delete;
        PtRegKeyHandle& operator=(const PtRegKeyHandle&) = delete;
        HKEY Get() const noexcept { return m_key; }
        HKEY* Put() noexcept { return &m_key; }
        explicit operator bool() const noexcept { return m_key != nullptr; }
    private:
        HKEY m_key;
    };

    // Safe BSTR-to-wstring extraction.
    std::wstring BstrToWstr(BSTR bstr)
    {
        if (!bstr) return {};
        return std::wstring(bstr, SysStringLen(bstr));
    }

    // Read a REG_SZ value from an open registry key. Returns empty on failure.
    std::wstring ReadRegSz(HKEY hKey, const wchar_t* valueName)
    {
        if (hKey == nullptr || valueName == nullptr) return {};

        WCHAR buffer[1024] = {};
        DWORD bufferSize = sizeof(buffer);
        DWORD type = 0;
        LONG result = RegQueryValueExW(hKey, valueName, nullptr, &type,
                                       reinterpret_cast<BYTE*>(buffer), &bufferSize);
        if (result != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
            return {};

        // Length-aware construction: bufferSize includes the null terminator in bytes.
        // Prevents reading past the buffer if the value exactly fills 1024 chars.
        const size_t charCount = bufferSize / sizeof(WCHAR);
        if (charCount > 0 && buffer[charCount - 1] == L'\0')
            return std::wstring(buffer, charCount - 1);
        return std::wstring(buffer, charCount);
    }
} // anonymous namespace

// ============================================================================
// Built-in Default File List
// ============================================================================
// These core OS binaries are patched in virtually every monthly CU.
// Compiled from CU manifest analysis across 2023-2026.
// Paths are relative to %SystemRoot%\System32.
// Each entry is {relativePath, category}.

std::vector<std::pair<std::wstring, std::wstring>> PatchTuesdayCollector::GetDefaultFileList()
{
    return {
        // Kernel and core executive
        {L"ntoskrnl.exe",                 L"kernel"},
        {L"ntdll.dll",                    L"kernel"},

        // Networking
        {L"drivers\\tcpip.sys",           L"network"},
        {L"drivers\\http.sys",            L"network"},
        {L"drivers\\afd.sys",             L"network"},

        // Cryptography and security
        {L"drivers\\cng.sys",             L"crypto"},
        {L"bcryptprimitives.dll",         L"crypto"},
        {L"schannel.dll",                 L"crypto"},
        {L"lsasrv.dll",                   L"auth"},
        {L"msv1_0.dll",                   L"auth"},
        {L"kerberos.dll",                 L"auth"},
        {L"netlogon.dll",                 L"auth"},

        // Win32k / graphics (top CVE source for kernel EoP)
        {L"win32kfull.sys",               L"win32k"},
        {L"win32kbase.sys",               L"win32k"},
        {L"win32k.sys",                   L"win32k"},

        // Remote Desktop / RDP
        {L"drivers\\termdd.sys",          L"rdp"},
        {L"rdpcorets.dll",                L"rdp"},
        {L"rdpbase.dll",                  L"rdp"},

        // HTML rendering (attack surface even without IE)
        {L"mshtml.dll",                   L"browser"},
        {L"urlmon.dll",                   L"browser"},
        {L"jscript9.dll",                 L"browser"},

        // Print spooler (PrintNightmare family)
        {L"spoolsv.exe",                  L"print"},
        {L"localspl.dll",                 L"print"},
        {L"win32spl.dll",                 L"print"},

        // SMB / file sharing
        {L"drivers\\srv2.sys",            L"smb"},
        {L"drivers\\mrxsmb.sys",          L"smb"},

        // OLE / COM / RPC (lateral movement attack surface)
        {L"ole32.dll",                    L"com"},
        {L"oleaut32.dll",                 L"com"},
        {L"combase.dll",                  L"com"},
        {L"rpcrt4.dll",                   L"com"},

        // Desktop Window Manager (DWM CVEs increasing)
        {L"dwm.exe",                      L"graphics"},
        {L"dwmcore.dll",                  L"graphics"},

        // .NET CLR (may not exist on all systems)
        {L"clr.dll",                      L"dotnet"},
        {L"clrjit.dll",                   L"dotnet"},
    };
}

// ============================================================================
// Manifest Loading
// ============================================================================

PatchManifest PatchTuesdayCollector::LoadManifestFromDisk()
{
    PatchManifest manifest = {};

    std::wstring manifestPath = std::wstring(ServiceConfig::DEFAULT_CONFIG_DIRECTORY)
                                + L"\\patch_manifest.json";

    DWORD attribs = GetFileAttributesW(manifestPath.c_str());
    if (attribs == INVALID_FILE_ATTRIBUTES)
    {
        LogError("[+] PatchTuesday: No backend manifest at " + WideToUtf8(manifestPath) +
                 " (using defaults only)");
        return manifest;
    }

    // Security: reject symlinks/junctions to prevent path traversal attacks
    if (attribs & FILE_ATTRIBUTE_REPARSE_POINT)
    {
        LogError("[!] PatchTuesday: Manifest is a reparse point — ignoring for safety");
        return manifest;
    }

    std::ifstream file(manifestPath, std::ios::in | std::ios::binary);
    if (!file.is_open())
    {
        LogError("[-] PatchTuesday: Failed to open manifest: " + WideToUtf8(manifestPath));
        return manifest;
    }

    // Cap at 1 MB to prevent DoS via oversized manifest
    file.seekg(0, std::ios::end);
    const auto fileSize = file.tellg();
    if (fileSize <= 0 || fileSize > 1048576)
    {
        LogError("[!] PatchTuesday: Manifest size invalid or >1 MB — ignoring");
        return manifest;
    }
    file.seekg(0, std::ios::beg);

    std::string content(static_cast<size_t>(fileSize), '\0');
    file.read(content.data(), fileSize);
    file.close();

    // Minimal JSON parsing for the known-simple schema.
    // We use simple string searching rather than a full JSON parser to avoid
    // adding a third-party dependency. The manifest is backend-generated with
    // a known schema, not arbitrary user input.

    // Extract manifestVersion
    {
        size_t pos = content.find("\"manifestVersion\"");
        if (pos != std::string::npos)
        {
            size_t colonPos = content.find(':', pos);
            if (colonPos != std::string::npos)
            {
                size_t q1 = content.find('"', colonPos + 1);
                size_t q2 = content.find('"', q1 + 1);
                if (q1 != std::string::npos && q2 != std::string::npos)
                {
                    std::string v = content.substr(q1 + 1, q2 - q1 - 1);
                    manifest.manifestVersion = std::wstring(v.begin(), v.end());
                }
            }
        }
    }

    // Extract generatedDate
    {
        size_t pos = content.find("\"generatedDate\"");
        if (pos != std::string::npos)
        {
            size_t colonPos = content.find(':', pos);
            if (colonPos != std::string::npos)
            {
                size_t q1 = content.find('"', colonPos + 1);
                size_t q2 = content.find('"', q1 + 1);
                if (q1 != std::string::npos && q2 != std::string::npos)
                {
                    std::string d = content.substr(q1 + 1, q2 - q1 - 1);
                    manifest.generatedDate = std::wstring(d.begin(), d.end());
                }
            }
        }
    }

    // Extract files array — supports both simple string array and object array:
    //   "files": ["ntoskrnl.exe", ...]
    //   "files": [{"path": "ntoskrnl.exe", "category": "kernel"}, ...]
    {
        size_t filesPos = content.find("\"files\"");
        if (filesPos != std::string::npos)
        {
            size_t arrayStart = content.find('[', filesPos);
            size_t arrayEnd = content.find(']', arrayStart);
            if (arrayStart != std::string::npos && arrayEnd != std::string::npos)
            {
                std::string arr = content.substr(arrayStart + 1, arrayEnd - arrayStart - 1);

                bool isObjectArray = (arr.find('{') != std::string::npos);

                if (isObjectArray)
                {
                    // Parse objects: {"path": "...", "category": "..."}
                    size_t searchPos = 0;
                    while (searchPos < arr.size())
                    {
                        size_t objStart = arr.find('{', searchPos);
                        size_t objEnd = arr.find('}', objStart);
                        if (objStart == std::string::npos || objEnd == std::string::npos) break;

                        std::string obj = arr.substr(objStart, objEnd - objStart + 1);
                        PatchManifestEntry entry = {};

                        // Extract "path"
                        size_t pathKey = obj.find("\"path\"");
                        if (pathKey != std::string::npos)
                        {
                            size_t pColon = obj.find(':', pathKey);
                            size_t pq1 = obj.find('"', pColon + 1);
                            size_t pq2 = obj.find('"', pq1 + 1);
                            if (pq1 != std::string::npos && pq2 != std::string::npos)
                            {
                                std::string p = obj.substr(pq1 + 1, pq2 - pq1 - 1);
                                entry.relativePath = std::wstring(p.begin(), p.end());
                            }
                        }

                        // Extract "category"
                        size_t catKey = obj.find("\"category\"");
                        if (catKey != std::string::npos)
                        {
                            size_t cColon = obj.find(':', catKey);
                            size_t cq1 = obj.find('"', cColon + 1);
                            size_t cq2 = obj.find('"', cq1 + 1);
                            if (cq1 != std::string::npos && cq2 != std::string::npos)
                            {
                                std::string c = obj.substr(cq1 + 1, cq2 - cq1 - 1);
                                entry.category = std::wstring(c.begin(), c.end());
                            }
                        }

                        // Security: reject path traversal and excessively long paths
                        constexpr size_t MAX_MANIFEST_PATH_LENGTH = 260;
                        if (!entry.relativePath.empty() &&
                            entry.relativePath.size() <= MAX_MANIFEST_PATH_LENGTH &&
                            entry.relativePath.find(L"..") == std::wstring::npos &&
                            entry.relativePath.find(L'/') == std::wstring::npos)
                        {
                            manifest.files.push_back(std::move(entry));
                        }
                        else if (!entry.relativePath.empty())
                        {
                            LogError("[!] PatchTuesday: Rejected manifest entry with path traversal: " +
                                     WideToUtf8(entry.relativePath));
                        }

                        searchPos = objEnd + 1;
                    }
                }
                else
                {
                    // Simple string array: ["file1.dll", "file2.sys", ...]
                    size_t searchPos = 0;
                    while (searchPos < arr.size())
                    {
                        size_t q1 = arr.find('"', searchPos);
                        if (q1 == std::string::npos) break;
                        size_t q2 = arr.find('"', q1 + 1);
                        if (q2 == std::string::npos) break;

                        std::string e = arr.substr(q1 + 1, q2 - q1 - 1);
                        if (!e.empty() && e.size() <= 260 &&
                            e.find("..") == std::string::npos && e.find('/') == std::string::npos)
                        {
                            PatchManifestEntry entry = {};
                            entry.relativePath = std::wstring(e.begin(), e.end());
                            entry.category = L"manifest";
                            manifest.files.push_back(std::move(entry));
                        }

                        searchPos = q2 + 1;
                    }
                }
            }
        }
    }

    LogError("[+] PatchTuesday: Loaded manifest v" + WideToUtf8(manifest.manifestVersion) +
             " with " + std::to_string(manifest.files.size()) + " entries");

    return manifest;
}

void PatchTuesdayCollector::MergeManifestIntoFileList(
    std::vector<std::pair<std::wstring, std::wstring>>& fileList,
    const PatchManifest& manifest)
{
    if (manifest.files.empty()) return;

    // Build a case-insensitive set of existing entries for O(1) dedup
    std::unordered_set<std::wstring> existing;
    for (const auto& [path, cat] : fileList)
    {
        std::wstring lower = path;
        CharLowerW(lower.data());
        existing.insert(std::move(lower));
    }

    size_t added = 0;
    for (const auto& entry : manifest.files)
    {
        std::wstring lower = entry.relativePath;
        CharLowerW(lower.data());
        if (existing.find(lower) == existing.end())
        {
            fileList.push_back({entry.relativePath,
                                entry.category.empty() ? L"manifest" : entry.category});
            existing.insert(std::move(lower));
            ++added;
        }
    }

    if (added > 0)
    {
        LogError("[+] PatchTuesday: Merged " + std::to_string(added) + " new manifest entries");
    }
}

// ============================================================================
// File Version Reading
// ============================================================================

std::wstring PatchTuesdayCollector::ResolveSystemFilePath(const std::wstring& relativePath)
{
    if (relativePath.empty()) return {};

    // Cache the system directory to avoid redundant syscalls (called 35+ times per cycle).
    // The system directory does not change at runtime.
    static const std::wstring cachedSystemDir = []() -> std::wstring {
        const UINT requiredLen = GetSystemDirectoryW(nullptr, 0);
        if (requiredLen == 0)
        {
            LogError("[-] PatchTuesday: GetSystemDirectoryW sizing call failed, error: " +
                     std::to_string(GetLastError()));
            return {};
        }
        std::wstring dir(static_cast<size_t>(requiredLen), L'\0');
        const UINT copiedLen = GetSystemDirectoryW(dir.data(), requiredLen);
        if (copiedLen == 0 || copiedLen >= requiredLen)
        {
            LogError("[-] PatchTuesday: GetSystemDirectoryW failed, error: " +
                     std::to_string(GetLastError()));
            return {};
        }
        dir.resize(static_cast<size_t>(copiedLen));
        return dir;
    }();

    if (cachedSystemDir.empty()) return {};

    return cachedSystemDir + L"\\" + relativePath;
}

// Read PE file version from RT_VERSION resource using LoadLibraryExW(DATAFILE).
// Bypasses GetFileVersionInfo compatibility shim that rewrites version on
// manifests without <supportedOS> GUIDs.
std::wstring PatchTuesdayCollector::ReadFileVersion(const std::wstring& filePath)
{
    if (filePath.empty()) return {};

    PtModuleHandle hModule(LoadLibraryExW(
        filePath.c_str(), nullptr,
        LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE));
    if (!hModule) return {};

    const HRSRC hResInfo = FindResourceW(hModule.Get(), MAKEINTRESOURCEW(1), RT_VERSION);
    if (!hResInfo) return {};

    const DWORD resSize = SizeofResource(hModule.Get(), hResInfo);
    if (resSize == 0) return {};

    const HGLOBAL hResData = LoadResource(hModule.Get(), hResInfo);
    if (!hResData) return {};

    const void* pRawData = LockResource(hResData);
    if (!pRawData) return {};

    // Copy into writable buffer — mapped pages are read-only and
    // VerQueryValueW may write alignment fixups.
    std::vector<BYTE> versionData(
        static_cast<const BYTE*>(pRawData),
        static_cast<const BYTE*>(pRawData) + resSize);

    VS_FIXEDFILEINFO* pFixedInfo = nullptr;
    UINT fixedInfoSize = 0;
    if (!VerQueryValueW(versionData.data(), L"\\",
                        reinterpret_cast<LPVOID*>(&pFixedInfo), &fixedInfoSize))
        return {};

    if (!pFixedInfo || fixedInfoSize < sizeof(VS_FIXEDFILEINFO)) return {};
    if (pFixedInfo->dwSignature != VS_FFI_SIGNATURE) return {};

    return std::to_wstring(HIWORD(pFixedInfo->dwFileVersionMS)) + L"." +
           std::to_wstring(LOWORD(pFixedInfo->dwFileVersionMS)) + L"." +
           std::to_wstring(HIWORD(pFixedInfo->dwFileVersionLS)) + L"." +
           std::to_wstring(LOWORD(pFixedInfo->dwFileVersionLS));
}

// ============================================================================
// System State Collection
// ============================================================================

DWORD PatchTuesdayCollector::ReadUBR()
{
    PtRegKeyHandle hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
        0, KEY_READ, hKey.Put()) != ERROR_SUCCESS)
        return 0;

    DWORD ubr = 0;
    DWORD size = sizeof(DWORD);
    DWORD type = 0;
    if (RegQueryValueExW(hKey.Get(), L"UBR", nullptr, &type,
                         reinterpret_cast<BYTE*>(&ubr), &size) != ERROR_SUCCESS || type != REG_DWORD)
        return 0;

    return ubr;
}

bool PatchTuesdayCollector::QueryCbsRebootPending()
{
    // CBS sets this key when a reboot is required after servicing.
    PtRegKeyHandle hKey;
    return (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Component Based Servicing\\RebootPending",
        0, KEY_READ, hKey.Put()) == ERROR_SUCCESS);
}

bool PatchTuesdayCollector::QueryWindowsUpdateRebootPending()
{
    // WUA sets this key when a reboot is needed to finalize an update.
    PtRegKeyHandle hKey;
    return (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update\\RebootRequired",
        0, KEY_READ, hKey.Put()) == ERROR_SUCCESS);
}

bool PatchTuesdayCollector::QueryRebootRequired()
{
    // ISystemInformation::RebootRequired is the authoritative COM source.
    // COM must already be initialized by the caller (GenerateMsptInventoryJSON).
    ISystemInformation* pSysInfo = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(SystemInformation), nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(ISystemInformation), reinterpret_cast<void**>(&pSysInfo));
    if (FAILED(hr) || !pSysInfo) return false;

    VARIANT_BOOL rebootRequired = VARIANT_FALSE;
    hr = pSysInfo->get_RebootRequired(&rebootRequired);
    pSysInfo->Release();
    if (FAILED(hr)) return false;

    return (rebootRequired == VARIANT_TRUE);
}

std::wstring PatchTuesdayCollector::QueryUpdateServiceSource()
{
    // COM must already be initialized by the caller (GenerateMsptInventoryJSON).
    IUpdateServiceManager* pServiceMgr = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(UpdateServiceManager), nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(IUpdateServiceManager), reinterpret_cast<void**>(&pServiceMgr));
    if (FAILED(hr) || !pServiceMgr) return {};

    IUpdateServiceCollection* pServices = nullptr;
    hr = pServiceMgr->get_Services(&pServices);
    if (FAILED(hr) || !pServices)
    {
        pServiceMgr->Release();
        return {};
    }

    std::wstring result;
    LONG count = 0;
    pServices->get_Count(&count);

    for (LONG i = 0; i < count; ++i)
    {
        IUpdateService* pService = nullptr;
        if (SUCCEEDED(pServices->get_Item(i, &pService)) && pService)
        {
            // get_IsDefaultAUService is on IUpdateService2, not IUpdateService.
            IUpdateService2* pService2 = nullptr;
            HRESULT qiHr = pService->QueryInterface(__uuidof(IUpdateService2),
                                                     reinterpret_cast<void**>(&pService2));
            if (SUCCEEDED(qiHr) && pService2)
            {
                VARIANT_BOOL isDefault = VARIANT_FALSE;
                pService2->get_IsDefaultAUService(&isDefault);
                if (isDefault == VARIANT_TRUE)
                {
                    BSTR bstrName = nullptr;
                    if (SUCCEEDED(pService2->get_Name(&bstrName)))
                    {
                        result = BstrToWstr(bstrName);
                        SysFreeString(bstrName);
                    }
                    pService2->Release();
                    pService->Release();
                    break;
                }
                pService2->Release();
            }
            pService->Release();
        }
    }

    pServices->Release();
    pServiceMgr->Release();
    return result;
}

std::wstring PatchTuesdayCollector::QueryLastUpdateCheckTime()
{
    // COM must already be initialized by the caller (GenerateMsptInventoryJSON).
    IAutomaticUpdates* pAutoUpdates = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(AutomaticUpdates), nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(IAutomaticUpdates), reinterpret_cast<void**>(&pAutoUpdates));
    if (FAILED(hr) || !pAutoUpdates) return {};

    // get_Results is on IAutomaticUpdates2, not the base IAutomaticUpdates.
    IAutomaticUpdates2* pAutoUpdates2 = nullptr;
    hr = pAutoUpdates->QueryInterface(__uuidof(IAutomaticUpdates2),
                                       reinterpret_cast<void**>(&pAutoUpdates2));
    if (FAILED(hr) || !pAutoUpdates2)
    {
        pAutoUpdates->Release();
        return {};
    }

    IAutomaticUpdatesResults* pResults = nullptr;
    hr = pAutoUpdates2->get_Results(&pResults);
    if (FAILED(hr) || !pResults)
    {
        pAutoUpdates2->Release();
        pAutoUpdates->Release();
        return {};
    }

    VARIANT varDate = {};
    VariantInit(&varDate);
    hr = pResults->get_LastSearchSuccessDate(&varDate);

    std::wstring result;
    if (SUCCEEDED(hr) && varDate.vt == VT_DATE)
    {
        SYSTEMTIME st = {};
        if (VariantTimeToSystemTime(varDate.date, &st))
        {
            wchar_t buffer[32] = {};
            _snwprintf_s(buffer, _countof(buffer), _TRUNCATE,
                         L"%04u-%02u-%02uT%02u:%02u:%02u",
                         st.wYear, st.wMonth, st.wDay,
                         st.wHour, st.wMinute, st.wSecond);
            result = buffer;
        }
    }

    VariantClear(&varDate);
    pResults->Release();
    pAutoUpdates2->Release();
    pAutoUpdates->Release();
    return result;
}

// ============================================================================
// Office / M365 Click-to-Run Detection
// ============================================================================

OfficePatchState PatchTuesdayCollector::DetectOfficeClickToRun()
{
    OfficePatchState state = {};

    PtRegKeyHandle hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Office\\ClickToRun\\Configuration",
        0, KEY_READ, hKey.Put()) != ERROR_SUCCESS)
    {
        LogError("[+] PatchTuesday: No Office Click-to-Run installation detected");
        return state;
    }

    state.clickToRunDetected = true;
    state.versionToReport = ReadRegSz(hKey.Get(), L"VersionToReport");
    state.updateChannel = ReadRegSz(hKey.Get(), L"UpdateChannel");
    state.platform = ReadRegSz(hKey.Get(), L"Platform");

    // Resolve human-readable channel name from the CDN URL
    if (!state.updateChannel.empty())
    {
        std::wstring lower = state.updateChannel;
        CharLowerW(lower.data());
        if (lower.find(L"current") != std::wstring::npos)
            state.updateChannel = L"Current";
        else if (lower.find(L"monthlyenterprise") != std::wstring::npos)
            state.updateChannel = L"MonthlyEnterprise";
        else if (lower.find(L"semiannual") != std::wstring::npos || lower.find(L"deferred") != std::wstring::npos)
            state.updateChannel = L"Semi-Annual";
        // Otherwise keep the raw URL for backend interpretation
    }

    // Determine Office install path
    std::wstring installPath = ReadRegSz(hKey.Get(), L"InstallationPath");
    if (installPath.empty())
    {
        wchar_t progFiles[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"ProgramFiles", progFiles, MAX_PATH) > 0)
        {
            installPath = std::wstring(progFiles) + L"\\Microsoft Office\\root\\Office16";
        }
    }
    else
    {
        // InstallationPath usually points to "...\root", binaries are in \Office16
        installPath += L"\\Office16";
    }
    state.installPath = installPath;

    LogWideStringAsUtf8("[+] PatchTuesday: Office C2R version: ", state.versionToReport);
    LogWideStringAsUtf8("[+] PatchTuesday: Office channel: ", state.updateChannel);
    LogWideStringAsUtf8("[+] PatchTuesday: Office platform: ", state.platform);

    // Key Office binaries (ground truth for Office patching)
    struct OfficeBinary
    {
        const wchar_t* filename;
        const wchar_t* category;
    };

    const OfficeBinary officeBinaries[] = {
        {L"WINWORD.EXE",       L"office_word"},
        {L"EXCEL.EXE",         L"office_excel"},
        {L"POWERPNT.EXE",      L"office_powerpoint"},
        {L"OUTLOOK.EXE",       L"office_outlook"},
        {L"MSACCESS.EXE",      L"office_access"},
        {L"mso.dll",           L"office_core"},
        {L"graph.exe",         L"office_core"},
        {L"oart.dll",          L"office_core"},
    };

    for (const auto& bin : officeBinaries)
    {
        PatchFileVersionInfo fv = {};
        fv.relativePath = bin.filename;
        fv.category = bin.category;
        fv.fullPath = installPath + L"\\" + bin.filename;

        DWORD fileAttribs = GetFileAttributesW(fv.fullPath.c_str());
        if (fileAttribs == INVALID_FILE_ATTRIBUTES)
        {
            fv.fileExists = false;
        }
        else
        {
            fv.fileExists = true;
            fv.fileVersion = ReadFileVersion(fv.fullPath);
            fv.versionReadSuccess = !fv.fileVersion.empty();
        }

        state.binaryVersions.push_back(std::move(fv));
    }

    return state;
}

// ============================================================================
// Public Collection Methods
// ============================================================================

std::vector<PatchFileVersionInfo> PatchTuesdayCollector::CollectFileVersions()
{
    LogError("[+] PatchTuesday: Collecting OS binary file versions...");

    auto fileList = GetDefaultFileList();

    PatchManifest manifest = LoadManifestFromDisk();
    MergeManifestIntoFileList(fileList, manifest);

    LogError("[+] PatchTuesday: Scanning " + std::to_string(fileList.size()) + " target files");

    std::vector<PatchFileVersionInfo> results;
    results.reserve(fileList.size());

    for (const auto& [relativePath, category] : fileList)
    {
        PatchFileVersionInfo info = {};
        info.relativePath = relativePath;
        info.category = category;
        info.fullPath = ResolveSystemFilePath(relativePath);

        if (info.fullPath.empty())
        {
            results.push_back(std::move(info));
            continue;
        }

        DWORD attribs = GetFileAttributesW(info.fullPath.c_str());
        if (attribs == INVALID_FILE_ATTRIBUTES)
        {
            // File not found — expected for optional components (e.g., clr.dll)
            results.push_back(std::move(info));
            continue;
        }

        info.fileExists = true;
        info.fileVersion = ReadFileVersion(info.fullPath);
        info.versionReadSuccess = !info.fileVersion.empty();

        if (!info.versionReadSuccess)
        {
            LogError("[!] PatchTuesday: Version unreadable: " + WideToUtf8(info.fullPath));
        }

        results.push_back(std::move(info));
    }

    size_t scanned = 0, succeeded = 0;
    for (const auto& r : results)
    {
        if (r.fileExists) ++scanned;
        if (r.versionReadSuccess) ++succeeded;
    }

    LogError("[+] PatchTuesday: " + std::to_string(succeeded) + "/" +
             std::to_string(scanned) + " versions read (" +
             std::to_string(results.size()) + " targets)");

    return results;
}

PatchSystemState PatchTuesdayCollector::CollectSystemState()
{
    LogError("[+] PatchTuesday: Collecting system patch state...");
    PatchSystemState state = {};

    // OS build via RtlGetVersion (unshimmed) + registry UBR
    typedef LONG(WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
    auto fnRtlGetVersion = reinterpret_cast<RtlGetVersionPtr>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));

    if (fnRtlGetVersion != nullptr)
    {
        RTL_OSVERSIONINFOW osvi = {};
        osvi.dwOSVersionInfoSize = sizeof(osvi);
        if (fnRtlGetVersion(&osvi) == 0)
        {
            DWORD ubr = ReadUBR();
            state.osBuild = std::to_wstring(osvi.dwMajorVersion) + L"." +
                            std::to_wstring(osvi.dwMinorVersion) + L"." +
                            std::to_wstring(osvi.dwBuildNumber) + L"." +
                            std::to_wstring(ubr);
        }
    }

    // Reuse existing OsVersionInfo for edition and architecture
    OsVersionInfo osInfo = GetOsVersionInfo();
    state.osEdition = osInfo.osDisplayName;
    state.processorArchitecture = osInfo.processorArchitecture;

    // Reboot state — check all three sources
    state.rebootRequired = QueryRebootRequired();
    state.cbsRebootPending = QueryCbsRebootPending();
    state.windowsUpdateRebootRequired = QueryWindowsUpdateRebootPending();

    state.updateServiceSource = QueryUpdateServiceSource();
    state.lastUpdateCheckTime = QueryLastUpdateCheckTime();

    LogWideStringAsUtf8("[+] PatchTuesday: OS build: ", state.osBuild);
    LogError("[+] PatchTuesday: Reboot: WUA=" + std::string(state.rebootRequired ? "Y" : "N") +
             " CBS=" + std::string(state.cbsRebootPending ? "Y" : "N") +
             " WU=" + std::string(state.windowsUpdateRebootRequired ? "Y" : "N"));
    LogWideStringAsUtf8("[+] PatchTuesday: Update source: ", state.updateServiceSource);
    LogWideStringAsUtf8("[+] PatchTuesday: Last check: ", state.lastUpdateCheckTime);

    return state;
}

OfficePatchState PatchTuesdayCollector::CollectOfficePatchState()
{
    LogError("[+] PatchTuesday: Collecting Office/M365 patch state...");
    return DetectOfficeClickToRun();
}

// ============================================================================
// JSON Generation
// ============================================================================

std::string GenerateMsptInventoryJSON()
{
    LogError("[+] PatchTuesday: Generating MSPT inventory JSON...");

    // Initialize COM once for all WUA queries in this collection cycle.
    // The worker thread may already have COM initialized (VSS, WinAppX);
    // RPC_E_CHANGED_MODE is benign and PtComScope handles it gracefully.
    // Hoisting COM scope here prevents nested CoInit/CoUninit from the
    // individual Query* methods prematurely decrementing the refcount.
    PtComScope comScope;

    PatchTuesdayCollector collector;

    PatchSystemState systemState = collector.CollectSystemState();
    std::vector<PatchFileVersionInfo> fileVersions = collector.CollectFileVersions();
    OfficePatchState officeState = collector.CollectOfficePatchState();

    // ISO 8601 timestamp
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    struct tm timeinfo = {};
    std::string timestamp;
    if (localtime_s(&timeinfo, &time) == 0)
    {
        std::ostringstream ts;
        ts << std::put_time(&timeinfo, "%Y-%m-%dT%H:%M:%S");
        timestamp = ts.str();
    }
    else
    {
        timestamp = "UNKNOWN";
    }

    std::ostringstream out;
    out << "{\n";

    // Metadata
    out << "  \"spectraMachineId\": \"" << MachineId::GetMachineIdUtf8() << "\",\n";
    out << "  \"collectionTimestamp\": \"" << timestamp << "\",\n";
    out << "  \"agentVersion\": \"" << WideToUtf8(ServiceConfig::VERSION) << "\",\n";
    out << "  \"dataType\": \"msptInventory\",\n";

    // System state
    out << "  \"systemState\": {\n";
    out << "    \"osBuild\": " << JsonEscape(systemState.osBuild) << ",\n";
    out << "    \"osEdition\": " << JsonEscape(systemState.osEdition) << ",\n";
    out << "    \"processorArchitecture\": " << JsonEscape(systemState.processorArchitecture) << ",\n";
    out << "    \"rebootRequired\": " << (systemState.rebootRequired ? "true" : "false") << ",\n";
    out << "    \"cbsRebootPending\": " << (systemState.cbsRebootPending ? "true" : "false") << ",\n";
    out << "    \"windowsUpdateRebootRequired\": " << (systemState.windowsUpdateRebootRequired ? "true" : "false") << ",\n";
    out << "    \"updateServiceSource\": " << JsonEscape(systemState.updateServiceSource) << ",\n";
    out << "    \"lastUpdateCheckTime\": " << JsonEscape(systemState.lastUpdateCheckTime) << "\n";
    out << "  },\n";

    // OS file versions
    out << "  \"osFileVersions\": [\n";
    for (size_t i = 0; i < fileVersions.size(); ++i)
    {
        const auto& fv = fileVersions[i];
        out << "    {\n";
        out << "      \"relativePath\": " << JsonEscape(fv.relativePath) << ",\n";
        out << "      \"fullPath\": " << JsonEscape(fv.fullPath) << ",\n";
        out << "      \"fileVersion\": " << JsonEscape(fv.fileVersion) << ",\n";
        out << "      \"category\": " << JsonEscape(fv.category) << ",\n";
        out << "      \"fileExists\": " << (fv.fileExists ? "true" : "false") << ",\n";
        out << "      \"versionReadSuccess\": " << (fv.versionReadSuccess ? "true" : "false") << "\n";
        out << "    }";
        if (i + 1 < fileVersions.size()) out << ",";
        out << "\n";
    }
    out << "  ],\n";

    // Office patch state
    out << "  \"officePatchState\": {\n";
    out << "    \"clickToRunDetected\": " << (officeState.clickToRunDetected ? "true" : "false") << ",\n";
    out << "    \"versionToReport\": " << JsonEscape(officeState.versionToReport) << ",\n";
    out << "    \"updateChannel\": " << JsonEscape(officeState.updateChannel) << ",\n";
    out << "    \"platform\": " << JsonEscape(officeState.platform) << ",\n";
    out << "    \"installPath\": " << JsonEscape(officeState.installPath) << ",\n";
    out << "    \"binaryVersions\": [\n";
    for (size_t i = 0; i < officeState.binaryVersions.size(); ++i)
    {
        const auto& fv = officeState.binaryVersions[i];
        out << "      {\n";
        out << "        \"relativePath\": " << JsonEscape(fv.relativePath) << ",\n";
        out << "        \"fullPath\": " << JsonEscape(fv.fullPath) << ",\n";
        out << "        \"fileVersion\": " << JsonEscape(fv.fileVersion) << ",\n";
        out << "        \"category\": " << JsonEscape(fv.category) << ",\n";
        out << "        \"fileExists\": " << (fv.fileExists ? "true" : "false") << ",\n";
        out << "        \"versionReadSuccess\": " << (fv.versionReadSuccess ? "true" : "false") << "\n";
        out << "      }";
        if (i + 1 < officeState.binaryVersions.size()) out << ",";
        out << "\n";
    }
    out << "    ]\n";
    out << "  }\n";

    out << "}\n";

    LogError("[+] PatchTuesday: MSPT inventory JSON complete");
    return out.str();
}
