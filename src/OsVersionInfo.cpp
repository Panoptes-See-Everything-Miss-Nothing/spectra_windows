#include "OsVersionInfo.h"
#include <winver.h>

#pragma comment(lib, "version.lib")

// RAII wrapper for HMODULE loaded via LoadLibraryExW.
// Ensures FreeLibrary is called on all exit paths, preventing handle leaks.
class ModuleHandle {
public:
    explicit ModuleHandle(HMODULE h = nullptr) noexcept : m_handle(h) {}
    ~ModuleHandle() { if (m_handle) FreeLibrary(m_handle); }

    ModuleHandle(const ModuleHandle&) = delete;
    ModuleHandle& operator=(const ModuleHandle&) = delete;

    ModuleHandle(ModuleHandle&& other) noexcept : m_handle(other.m_handle) {
        other.m_handle = nullptr;
    }

    ModuleHandle& operator=(ModuleHandle&& other) noexcept {
        if (this != &other) {
            if (m_handle) FreeLibrary(m_handle);
            m_handle = other.m_handle;
            other.m_handle = nullptr;
        }
        return *this;
    }

    HMODULE Get() const noexcept { return m_handle; }
    explicit operator bool() const noexcept { return m_handle != nullptr; }

private:
    HMODULE m_handle;
};

// RAII wrapper for registry keys opened via RegOpenKeyExW.
// Ensures RegCloseKey is called on all exit paths, preventing handle leaks.
class RegistryKeyHandle {
public:
    explicit RegistryKeyHandle(HKEY h = nullptr) noexcept : m_key(h) {}
    ~RegistryKeyHandle() { if (m_key) RegCloseKey(m_key); }

    RegistryKeyHandle(const RegistryKeyHandle&) = delete;
    RegistryKeyHandle& operator=(const RegistryKeyHandle&) = delete;

    RegistryKeyHandle(RegistryKeyHandle&& other) noexcept : m_key(other.m_key) {
        other.m_key = nullptr;
    }

    RegistryKeyHandle& operator=(RegistryKeyHandle&& other) noexcept {
        if (this != &other) {
            if (m_key) RegCloseKey(m_key);
            m_key = other.m_key;
            other.m_key = nullptr;
        }
        return *this;
    }

    HKEY Get() const noexcept { return m_key; }
    HKEY* Put() noexcept { return &m_key; }
    explicit operator bool() const noexcept { return m_key != nullptr; }

private:
    HKEY m_key;
};

// Resolve the system directory at runtime via GetSystemDirectoryW.
// Never hardcode "C:\\" or "C:\\Windows" — Windows may be installed on any drive.
static std::wstring GetNtoskrnlPath()
{
    // First call with size 0 to get required buffer length (includes null terminator)
    const UINT requiredLen = GetSystemDirectoryW(nullptr, 0);
    if (requiredLen == 0) {
        const DWORD dwError = GetLastError();
        LogError("[-] OsVersionInfo: GetSystemDirectoryW sizing call failed, error: " + std::to_string(dwError));
        return {};
    }

    std::wstring systemDir(static_cast<size_t>(requiredLen), L'\0');
    const UINT copiedLen = GetSystemDirectoryW(systemDir.data(), requiredLen);

    if (copiedLen == 0 || copiedLen >= requiredLen) {
        const DWORD dwError = GetLastError();
        LogError("[-] OsVersionInfo: GetSystemDirectoryW failed, error: " + std::to_string(dwError));
        return {};
    }

    // copiedLen does NOT include the null terminator
    systemDir.resize(static_cast<size_t>(copiedLen));

    return systemDir + L"\\ntoskrnl.exe";
}

// Read the file version by loading the PE as a data file and extracting the
// RT_VERSION resource directly from the mapped image.
//
// WHY NOT GetFileVersionInfo / GetFileVersionInfoEx?
///   Both the non-Ex and Ex variants (even with FILE_VER_GET_NEUTRAL) route
///   through a compatibility layer in version.dll that inspects the calling
///   process's manifest. Without a <supportedOS> GUID declaring Windows 8.1+
///   or 10+ support, the returned VS_FIXEDFILEINFO.dwFileVersionMS is
///   rewritten to 0x00060002 (version 6.2 = Windows 8). FILE_VER_GET_NEUTRAL
///   only controls MUI DLL selection — it does NOT bypass this version shim.
///
/// WHY CAN'T WE READ THE KERNEL'S IN-MEMORY COPY?
///   ntoskrnl.exe is loaded by the boot loader into kernel address space
///   (above the user/kernel boundary). User-mode processes cannot read
///   kernel memory — any such attempt would trigger an access violation.
///   We must create our own user-mode mapping of the file on disk.
///
/// BYPASS APPROACH:
///   1. LoadLibraryExW with LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
///      LOAD_LIBRARY_AS_IMAGE_RESOURCE — creates a read-only file mapping
///      in user-mode address space. No DllMain, no imports, no shims.
///   2. FindResourceW(RT_VERSION) → LoadResource → LockResource to get
///      a pointer to the raw version resource bytes.
///   3. Copy the resource into a writable buffer (the mapped pages are
///      read-only) and use VerQueryValueW to parse VS_FIXEDFILEINFO.
///      VerQueryValueW is a pure in-memory parser — the version shim
///      lives in GetFileVersionInfo[Ex]W's loader, not in VerQueryValueW.
///
/// This is the same technique PowerShell's (Get-Item).VersionInfo and
/// Windows Explorer's file properties dialog use internally.
///
/// Privilege requirement: Standard read access (no elevation needed).
/// Security: ntoskrnl.exe is protected by WRP/TrustedInstaller ACLs.
///           The file mapping is atomic — no TOCTOU risk.
///
/// Returns a version string like "10.0.26100.7623" or empty on failure.
static std::wstring ReadFileVersion(const std::wstring& filePath)
{
    if (filePath.empty()) {
        return {};
    }

    // Map the PE into user-mode address space as raw data.
    // LOAD_LIBRARY_AS_IMAGE_RESOURCE: map sections at virtual addresses so
    //   FindResourceW can walk the resource directory tree correctly.
    // LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE: prevent concurrent modification
    //   of the mapping while we read from it.
    // No DllMain execution, no import resolution, no compatibility shims.
    ModuleHandle hModule(LoadLibraryExW(
        filePath.c_str(),
        nullptr,
        LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE));

    if (!hModule) {
        const DWORD dwError = GetLastError();
        LogError("[-] OsVersionInfo: LoadLibraryExW(DATAFILE) failed for " +
                 WideToUtf8(filePath) + ", error: " + std::to_string(dwError));
        return {};
    }

    // Locate the RT_VERSION resource (resource type 16, resource ID 1)
    const HRSRC hResInfo = FindResourceW(hModule.Get(), MAKEINTRESOURCEW(1), RT_VERSION);
    if (hResInfo == nullptr) {
        const DWORD dwError = GetLastError();
        LogError("[-] OsVersionInfo: FindResourceW(RT_VERSION) failed for " +
                 WideToUtf8(filePath) + ", error: " + std::to_string(dwError));
        return {};
    }

    const DWORD resSize = SizeofResource(hModule.Get(), hResInfo);
    if (resSize == 0) {
        LogError("[-] OsVersionInfo: SizeofResource returned 0 for " + WideToUtf8(filePath));
        return {};
    }

    const HGLOBAL hResData = LoadResource(hModule.Get(), hResInfo);
    if (hResData == nullptr) {
        const DWORD dwError = GetLastError();
        LogError("[-] OsVersionInfo: LoadResource failed for " +
                 WideToUtf8(filePath) + ", error: " + std::to_string(dwError));
        return {};
    }

    const void* pRawData = LockResource(hResData);
    if (pRawData == nullptr) {
        LogError("[-] OsVersionInfo: LockResource returned null for " + WideToUtf8(filePath));
        return {};
    }

    // Copy into a writable buffer. The mapped resource pages are read-only,
    // and VerQueryValueW may write alignment fixups into the buffer.
    // Heap allocation is justified — resource block size is variable.
    std::vector<BYTE> versionData(
        static_cast<const BYTE*>(pRawData),
        static_cast<const BYTE*>(pRawData) + resSize);

    // Parse VS_FIXEDFILEINFO from the raw resource data.
    // VerQueryValueW is a pure in-memory parser — no version shim applied here.
    VS_FIXEDFILEINFO* pFixedInfo = nullptr;
    UINT fixedInfoSize = 0;

    if (!VerQueryValueW(versionData.data(), L"\\",
                        reinterpret_cast<LPVOID*>(&pFixedInfo), &fixedInfoSize)) {
        LogError("[-] OsVersionInfo: VerQueryValueW failed for root block of " + WideToUtf8(filePath));
        return {};
    }

    if (pFixedInfo == nullptr || fixedInfoSize < sizeof(VS_FIXEDFILEINFO)) {
        LogError("[-] OsVersionInfo: VS_FIXEDFILEINFO is null or undersized for " + WideToUtf8(filePath));
        return {};
    }

    // Validate the magic signature (0xFEEF04BD)
    if (pFixedInfo->dwSignature != VS_FFI_SIGNATURE) {
        LogError("[-] OsVersionInfo: VS_FIXEDFILEINFO signature mismatch for " + WideToUtf8(filePath));
        return {};
    }

    // Extract Major.Minor.Build.Revision from dwFileVersionMS / dwFileVersionLS
    const DWORD major    = HIWORD(pFixedInfo->dwFileVersionMS);
    const DWORD minor    = LOWORD(pFixedInfo->dwFileVersionMS);
    const DWORD build    = HIWORD(pFixedInfo->dwFileVersionLS);
    const DWORD revision = LOWORD(pFixedInfo->dwFileVersionLS);

    const std::wstring version = std::to_wstring(major) + L"." +
                                 std::to_wstring(minor) + L"." +
                                 std::to_wstring(build) + L"." +
                                 std::to_wstring(revision);

    return version;
}

// Resolve the native processor architecture string using GetNativeSystemInfo.
// GetNativeSystemInfo is used instead of GetSystemInfo so that WOW64 (32-bit)
// builds still report the true hardware architecture.
static std::wstring ResolveProcessorArchitecture()
{
    SYSTEM_INFO si = {};
    GetNativeSystemInfo(&si);

    switch (si.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64:  return L"x64";
    case PROCESSOR_ARCHITECTURE_INTEL:  return L"x86";
    case PROCESSOR_ARCHITECTURE_ARM:    return L"ARM";
    case PROCESSOR_ARCHITECTURE_ARM64:  return L"ARM64";
    default:
        return L"Unknown (" + std::to_wstring(si.wProcessorArchitecture) + L")";
    }
}

// Determine the actual OS bitness (32-bit vs 64-bit Windows installation).
// This is distinct from processor architecture — a 64-bit CPU can run a 32-bit OS.
//
// Detection logic:
//   - 64-bit build (#ifdef _WIN64)     → OS must be 64-bit (can't run x64 EXE on 32-bit OS)
//   - 32-bit build + WOW64 == TRUE     → OS is 64-bit (32-bit app running on 64-bit Windows)
//   - 32-bit build + WOW64 == FALSE    → OS is 32-bit (native 32-bit Windows)
static std::wstring ResolveOsBitness()
{
#ifdef _WIN64
    // A 64-bit binary can only execute on a 64-bit OS
    return L"64-bit";
#else
    // 32-bit build: check if the OS itself is 64-bit via WOW64
    typedef BOOL (WINAPI *LPFN_ISWOW64PROCESS)(HANDLE, PBOOL);
    const auto fnIsWow64Process = reinterpret_cast<LPFN_ISWOW64PROCESS>(
        GetProcAddress(GetModuleHandleW(L"kernel32"), "IsWow64Process"));

    if (fnIsWow64Process != nullptr) {
        BOOL isWow64 = FALSE;
        if (fnIsWow64Process(GetCurrentProcess(), &isWow64) && isWow64) {
            // 32-bit process running under WOW64 → OS is 64-bit
            return L"64-bit";
        }
    }

    // Native 32-bit process on 32-bit Windows
    return L"32-bit";
#endif
}

// Read the OS build number using RtlGetVersion (not GetVersionEx, which is shimmed).
// Returns 0 on failure.
static DWORD GetOsBuildNumber()
{
    typedef LONG (WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
    const auto fnRtlGetVersion = reinterpret_cast<RtlGetVersionPtr>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));

    if (fnRtlGetVersion == nullptr) {
        LogError("[!] OsVersionInfo: RtlGetVersion not found in ntdll.dll");
        return 0;
    }

    RTL_OSVERSIONINFOW osvi = {};
    osvi.dwOSVersionInfoSize = sizeof(osvi);

    if (fnRtlGetVersion(&osvi) != 0) {
        LogError("[!] OsVersionInfo: RtlGetVersion failed");
        return 0;
    }

    return osvi.dwBuildNumber;
}

// Build the human-readable OS display name (e.g., "Microsoft Windows 11 Pro 64-bit").
//
// Reads ProductName from HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion, but applies
// a critical correction: on Windows 11 (build 22000+) the registry ProductName is often
// still "Windows 10 Pro" because Microsoft never updated it. We patch the string by
// replacing "Windows 10" with "Windows 11" when the build number confirms Windows 11.
//
// OS bitness is determined from the actual Windows installation, not the CPU architecture.
// Privilege requirement: Standard read access (KEY_READ on HKLM).
static std::wstring BuildOsDisplayName()
{
    std::wstring productName;

    // RAII wrapper ensures RegCloseKey on all exit paths
    RegistryKeyHandle hKey;
    const LSTATUS openStatus = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
        0,
        KEY_READ,
        hKey.Put());

    if (openStatus == ERROR_SUCCESS && hKey) {
        WCHAR buffer[256] = {};
        DWORD bufferSize = sizeof(buffer);
        DWORD type = 0;

        const LSTATUS queryStatus = RegQueryValueExW(
            hKey.Get(), L"ProductName", nullptr, &type,
            reinterpret_cast<LPBYTE>(buffer), &bufferSize);

        if (queryStatus == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) {
            productName = buffer;
        }
        else {
            LogError("[!] OsVersionInfo: Failed to read ProductName from registry, error: " +
                     std::to_string(queryStatus));
        }
    }
    else {
        LogError("[!] OsVersionInfo: Failed to open CurrentVersion registry key, error: " +
                 std::to_string(openStatus));
    }

    if (productName.empty()) {
        productName = L"Windows";
    }

    // Windows 11 correction: Microsoft never updated the ProductName registry value
    // for Windows 11 — it still reads "Windows 10 Pro" (or similar) on many builds.
    // Windows 11 is defined as build 22000 and later. Use RtlGetVersion (unshimmed)
    // to get the true build number and patch the string accordingly.
    const DWORD buildNumber = GetOsBuildNumber();
    if (buildNumber >= 22000) {
        const std::wstring win10Token = L"Windows 10";
        const size_t pos = productName.find(win10Token);
        if (pos != std::wstring::npos) {
            productName.replace(pos, win10Token.length(), L"Windows 11");
            LogError("[+] OsVersionInfo: Corrected ProductName from 'Windows 10' to 'Windows 11' "
                     "(build " + std::to_string(buildNumber) + " >= 22000)");
        }
    }

    // Determine OS bitness from the actual Windows installation, not the CPU
    const std::wstring bitness = ResolveOsBitness();

    // Compose: "Microsoft <ProductName> <bitness>"
    return L"Microsoft " + productName + L" " + bitness;
}

OsVersionInfo GetOsVersionInfo()
{
    OsVersionInfo info = {};

    LogError("[+] OsVersionInfo: Collecting OS version and ntoskrnl.exe file version...");

    // 1. Processor architecture (reports the hardware CPU, not the OS or binary bitness)
    info.processorArchitecture = ResolveProcessorArchitecture();
    LogWideStringAsUtf8("[+] OsVersionInfo: Processor architecture: ", info.processorArchitecture);

    // 2. Build ntoskrnl.exe path from the actual system directory
    info.ntoskrnlPath = GetNtoskrnlPath();
    if (info.ntoskrnlPath.empty()) {
        LogError("[-] OsVersionInfo: Could not determine ntoskrnl.exe path");
    }
    else {
        LogWideStringAsUtf8("[+] OsVersionInfo: ntoskrnl.exe path: ", info.ntoskrnlPath);

        // 3. Read file version from the PE RT_VERSION resource directly
        info.ntoskrnlVersion = ReadFileVersion(info.ntoskrnlPath);
        if (info.ntoskrnlVersion.empty()) {
            LogError("[-] OsVersionInfo: Could not read ntoskrnl.exe file version");
        }
        else {
            LogWideStringAsUtf8("[+] OsVersionInfo: ntoskrnl.exe version: ", info.ntoskrnlVersion);
        }
    }

    // 4. Build human-readable OS name from registry ProductName + actual OS bitness
    info.osDisplayName = BuildOsDisplayName();
    LogWideStringAsUtf8("[+] OsVersionInfo: OS: ", info.osDisplayName);

    LogError("[+] OsVersionInfo: Collection complete");
    return info;
}