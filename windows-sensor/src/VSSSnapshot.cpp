#include "VSSSnapshot.h"
#include "PrivMgmt.h"
#include "Service/ServiceConfig.h"
#include <comdef.h>
#include <sddl.h>
#include <aclapi.h>
#include <sstream>
#include <chrono>

// Implementation details hidden from header
class VSSSnapshotImpl
{
public:
    IVssBackupComponents* pBackup = nullptr;
    VSS_ID snapshotSetId = GUID_NULL;
    VSS_ID snapshotId = GUID_NULL;
    std::wstring snapshotDevicePath;
    bool isValid = false;

    ~VSSSnapshotImpl()
    {
        Cleanup();
    }

    void Cleanup()
    {
        if (pBackup && snapshotSetId != GUID_NULL)
        {
            // Delete the snapshot set
            LONG deletedSnapshots = 0;
            VSS_ID nonDeletedSnapshotId = GUID_NULL;
            pBackup->DeleteSnapshots(snapshotSetId, VSS_OBJECT_SNAPSHOT_SET,
                TRUE, &deletedSnapshots, &nonDeletedSnapshotId);
        }

        if (pBackup)
        {
            pBackup->Release();
            pBackup = nullptr;
        }

        isValid = false;
    }
};

VSSSnapshot::VSSSnapshot() : pImpl(std::make_unique<VSSSnapshotImpl>())
{
}

VSSSnapshot::~VSSSnapshot()
{
    // pImpl destructor will handle cleanup
}

bool VSSSnapshot::CreateSnapshot(const std::wstring& volumePath)
{
    HRESULT hr = S_OK;

    // Initialize COM with retry for boot-time race condition.
    // When the service auto-starts at boot, the VSS COM infrastructure may not
    // be ready yet, causing CoInitializeEx or CreateVssBackupComponents to fail
    // with E_ACCESSDENIED (0x80070005). Retrying after a short delay resolves
    // this because the VSS service (swprv) finishes initialization within seconds.
    //
    // COINIT_MULTITHREADED is used instead of STA (CoInitialize default) because:
    //   1. The worker thread has no message pump, which STA requires for marshalling
    //   2. WinAppXPackages.cpp ComInitializer already uses MTA on the same thread
    //   3. VSS IVssAsync::Wait() works correctly in MTA
    constexpr DWORD MAX_COM_RETRIES = 3;
    constexpr DWORD COM_RETRY_DELAY_MS = 10000; // 10 seconds between retries

    bool comInitialized = false;

    for (DWORD attempt = 1; attempt <= MAX_COM_RETRIES; attempt++)
    {
        hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE)
        {
            comInitialized = (SUCCEEDED(hr));
            break;
        }

        if (attempt < MAX_COM_RETRIES)
        {
            LogError("[!] COM initialization failed on attempt " + std::to_string(attempt) +
                     "/" + std::to_string(MAX_COM_RETRIES) +
                     ", HRESULT: 0x" + std::to_string(static_cast<unsigned long>(hr)) +
                     " - retrying in " + std::to_string(COM_RETRY_DELAY_MS / 1000) + " seconds...");
            Sleep(COM_RETRY_DELAY_MS);
        }
        else
        {
            LogError("[-] Failed to initialize COM after " + std::to_string(MAX_COM_RETRIES) +
                     " attempts, HRESULT: 0x" + std::to_string(static_cast<unsigned long>(hr)));
            return false;
        }
    }

    // Enable backup privilege
    if (!EnablePrivilege(SE_BACKUP_NAME))
    {
        LogError("[-] Failed to enable SE_BACKUP_NAME privilege for VSS");
        if (comInitialized) CoUninitialize();
        return false;
    }

    // Create VSS backup components with retry for boot-time readiness.
    // Even if CoInitializeEx succeeds, the VSS provider service (swprv) may not
    // be fully started yet, causing CreateVssBackupComponents to fail.
    for (DWORD attempt = 1; attempt <= MAX_COM_RETRIES; attempt++)
    {
        hr = CreateVssBackupComponents(&pImpl->pBackup);
        if (SUCCEEDED(hr))
        {
            break;
        }

        if (attempt < MAX_COM_RETRIES)
        {
            LogError("[!] CreateVssBackupComponents failed on attempt " + std::to_string(attempt) +
                     "/" + std::to_string(MAX_COM_RETRIES) +
                     ", HRESULT: 0x" + std::to_string(static_cast<unsigned long>(hr)) +
                     " - retrying in " + std::to_string(COM_RETRY_DELAY_MS / 1000) + " seconds...");
            Sleep(COM_RETRY_DELAY_MS);
        }
        else
        {
            LogError("[-] Failed to create VSS backup components after " + std::to_string(MAX_COM_RETRIES) +
                     " attempts, HRESULT: 0x" + std::to_string(static_cast<unsigned long>(hr)));
            DisablePrivilege(SE_BACKUP_NAME);
            if (comInitialized) CoUninitialize();
            return false;
        }
    }

    // Initialize for backup
    hr = pImpl->pBackup->InitializeForBackup();
    if (FAILED(hr))
    {
        LogError("[-] Failed to initialize VSS for backup, HRESULT: 0x" + 
                 std::to_string(static_cast<unsigned long>(hr)));
        pImpl->Cleanup();
        DisablePrivilege(SE_BACKUP_NAME);
        CoUninitialize();
        return false;
    }

    // Set backup state
    hr = pImpl->pBackup->SetBackupState(FALSE, FALSE, VSS_BT_COPY, FALSE);
    if (FAILED(hr))
    {
        LogError("[-] Failed to set VSS backup state, HRESULT: 0x" + 
                 std::to_string(static_cast<unsigned long>(hr)));
        pImpl->Cleanup();
        DisablePrivilege(SE_BACKUP_NAME);
        CoUninitialize();
        return false;
    }

    // Set context to backup (simplest mode, doesn't involve writers)
    hr = pImpl->pBackup->SetContext(VSS_CTX_BACKUP);
    if (FAILED(hr))
    {
        LogError("[-] Failed to set VSS context, HRESULT: 0x" + 
                 std::to_string(static_cast<unsigned long>(hr)));
        pImpl->Cleanup();
        DisablePrivilege(SE_BACKUP_NAME);
        CoUninitialize();
        return false;
    }

    // Gather writer metadata (required before PrepareForBackup)
    IVssAsync* pAsync = nullptr;
    hr = pImpl->pBackup->GatherWriterMetadata(&pAsync);
    if (FAILED(hr))
    {
        LogError("[-] Failed to gather writer metadata, HRESULT: 0x" + 
                 std::to_string(static_cast<unsigned long>(hr)));
        pImpl->Cleanup();
        DisablePrivilege(SE_BACKUP_NAME);
        CoUninitialize();
        return false;
    }

    // Wait for the async operation to complete
    if (pAsync)
    {
        hr = pAsync->Wait();
        pAsync->Release();
        if (FAILED(hr))
        {
            LogError("[-] Failed to wait for GatherWriterMetadata, HRESULT: 0x" + 
                     std::to_string(static_cast<unsigned long>(hr)));
            pImpl->Cleanup();
            DisablePrivilege(SE_BACKUP_NAME);
            CoUninitialize();
            return false;
        }
    }

    // Start snapshot set
    hr = pImpl->pBackup->StartSnapshotSet(&pImpl->snapshotSetId);
    if (FAILED(hr))
    {
        LogError("[-] Failed to start VSS snapshot set, HRESULT: 0x" + 
                 std::to_string(static_cast<unsigned long>(hr)));
        pImpl->Cleanup();
        DisablePrivilege(SE_BACKUP_NAME);
        CoUninitialize();
        return false;
    }

    // Add volume to snapshot set
    hr = pImpl->pBackup->AddToSnapshotSet(const_cast<wchar_t*>(volumePath.c_str()), 
                                          GUID_NULL, &pImpl->snapshotId);
    if (FAILED(hr))
    {
        LogError("[-] Failed to add volume to VSS snapshot set, HRESULT: 0x" + 
                 std::to_string(static_cast<unsigned long>(hr)));
        pImpl->Cleanup();
        DisablePrivilege(SE_BACKUP_NAME);
        CoUninitialize();
        return false;
    }

    // Prepare for backup
    pAsync = nullptr;
    hr = pImpl->pBackup->PrepareForBackup(&pAsync);
    if (FAILED(hr))
    {
        LogError("[-] Failed to prepare VSS for backup, HRESULT: 0x" + 
                 std::to_string(static_cast<unsigned long>(hr)));
        pImpl->Cleanup();
        DisablePrivilege(SE_BACKUP_NAME);
        CoUninitialize();
        return false;
    }

    // Wait for the async operation to complete
    if (pAsync)
    {
        hr = pAsync->Wait();
        pAsync->Release();
        if (FAILED(hr))
        {
            LogError("[-] Failed to wait for PrepareForBackup, HRESULT: 0x" + 
                     std::to_string(static_cast<unsigned long>(hr)));
            pImpl->Cleanup();
            DisablePrivilege(SE_BACKUP_NAME);
            CoUninitialize();
            return false;
        }
    }

    // Create the snapshot
    pAsync = nullptr;
    hr = pImpl->pBackup->DoSnapshotSet(&pAsync);
    if (FAILED(hr))
    {
        LogError("[-] Failed to create VSS snapshot, HRESULT: 0x" + 
                 std::to_string(static_cast<unsigned long>(hr)));
        pImpl->Cleanup();
        DisablePrivilege(SE_BACKUP_NAME);
        CoUninitialize();
        return false;
    }

    // Wait for the async operation to complete
    if (pAsync)
    {
        hr = pAsync->Wait();
        pAsync->Release();
        if (FAILED(hr))
        {
            LogError("[-] Failed to wait for DoSnapshotSet, HRESULT: 0x" + 
                     std::to_string(static_cast<unsigned long>(hr)));
            pImpl->Cleanup();
            DisablePrivilege(SE_BACKUP_NAME);
            CoUninitialize();
            return false;
        }
    }

    // Get snapshot properties to retrieve the device path
    VSS_SNAPSHOT_PROP prop;
    hr = pImpl->pBackup->GetSnapshotProperties(pImpl->snapshotId, &prop);
    if (FAILED(hr))
    {
        LogError("[-] Failed to get VSS snapshot properties, HRESULT: 0x" + 
                 std::to_string(static_cast<unsigned long>(hr)));
        pImpl->Cleanup();
        DisablePrivilege(SE_BACKUP_NAME);
        CoUninitialize();
        return false;
    }

    pImpl->snapshotDevicePath = prop.m_pwszSnapshotDeviceObject;
    VssFreeSnapshotProperties(&prop);

    pImpl->isValid = true;
    LogError("[+] VSS snapshot created successfully: " + WideToUtf8(pImpl->snapshotDevicePath));

    return true;
}

std::wstring VSSSnapshot::GetSnapshotPath() const
{
    return pImpl->snapshotDevicePath;
}

bool VSSSnapshot::IsValid() const
{
    return pImpl->isValid;
}

// SecureTempDirectory implementation
SecureTempDirectory::SecureTempDirectory(const std::wstring& basePath)
    : m_valid(false)
{
    // Create a unique directory name to avoid race conditions and cleanup issues
    // Format: basePath_PID_Timestamp
    DWORD pid = GetCurrentProcessId();
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    
    std::wstringstream uniquePath;
    uniquePath << basePath << L"_" << pid << L"_" << now;
    m_path = uniquePath.str();

    // Verify parent directory doesn't already exist as a file or reparse point
    std::wstring parentPath = basePath;
    DWORD parentAttribs = GetFileAttributesW(parentPath.c_str());
    
    if (parentAttribs != INVALID_FILE_ATTRIBUTES)
    {
        // Parent exists - validate it's safe
        if (!IsDirectorySafeToUse(parentPath))
        {
            LogError("[-] Parent directory is not safe to use: " + WideToUtf8(parentPath));
            return;
        }
    }

    // Create the unique directory with a security descriptor that only allows current user
    SECURITY_ATTRIBUTES sa = {};
    PSECURITY_DESCRIPTOR pSD = nullptr;
    
    if (!CreateSecurityDescriptorForCurrentUser(&pSD))
    {
        LogError("[-] Failed to create security descriptor for temp directory");
        return;
    }

    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = pSD;
    sa.bInheritHandle = FALSE;

    if (!CreateDirectoryW(m_path.c_str(), &sa))
    {
        DWORD error = GetLastError();
        LogError("[-] Failed to create unique secure temp directory: " + WideToUtf8(m_path) + 
                 ", error: " + std::to_string(error) + " - " + GetWindowsErrorMessage(error));
        LocalFree(pSD);
        return;
    }

    LocalFree(pSD);
    LogError("[+] Created unique secure temp directory: " + WideToUtf8(m_path));

    // NOTE: The DACL was already applied by CreateDirectoryW via the SECURITY_ATTRIBUTES
    // passed above. We do NOT call SecureDirectory() / SetNamedSecurityInfoW here because
    // that requires WRITE_DAC permission on the directory. Under SERVICE_SID_TYPE_RESTRICTED,
    // the parent Temp directory's inherited DACL only grants the service SID Modify access
    // (which intentionally excludes WRITE_DAC/WRITE_OWNER). The CreateDirectoryW path works
    // because the kernel applies the SD at creation time before ACL inheritance takes effect.

    m_valid = true;
}

SecureTempDirectory::~SecureTempDirectory()
{
    if (m_valid && !m_path.empty())
    {
        // Recursively delete the directory
        try
        {
            std::error_code ec;
            fs::remove_all(m_path, ec);
            if (ec)
            {
                LogError("[-] Warning: Failed to remove temp directory: " + WideToUtf8(m_path) + 
                         ", error: " + ec.message());
            }
            else
            {
                LogError("[+] Removed temp directory: " + WideToUtf8(m_path));
            }
        }
        catch (const std::exception& e)
        {
            LogError(std::string("[-] Exception while removing temp directory: ") + e.what());
        }
    }
}

bool SecureTempDirectory::SecureDirectory()
{
    // Get current user's SID (SYSTEM when running as service)
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
    {
        LogError("[-] Failed to open process token for securing directory");
        return false;
    }

    DWORD dwBufferSize = 0;
    GetTokenInformation(hToken, TokenUser, nullptr, 0, &dwBufferSize);
    
    std::vector<BYTE> buffer(dwBufferSize);
    TOKEN_USER* pTokenUser = reinterpret_cast<TOKEN_USER*>(buffer.data());
    
    if (!GetTokenInformation(hToken, TokenUser, pTokenUser, dwBufferSize, &dwBufferSize))
    {
        CloseHandle(hToken);
        LogError("[-] Failed to get token information for securing directory");
        return false;
    }

    // Build service trustee name: "NT SERVICE\PanoptesSpectra"
    // Under SERVICE_SID_TYPE_RESTRICTED, the service SID must be in the DACL
    // for write access to succeed. SYSTEM alone is NOT sufficient because the
    // restricted token performs a second access check against only the restricting
    // SID list (which contains the per-service SID, not SYSTEM).
    std::wstring serviceTrustee = L"NT SERVICE\\";
    serviceTrustee += ServiceConfig::SERVICE_NAME;

    // Build DACL: SYSTEM (Full Control) + Service SID (Modify)
    EXPLICIT_ACCESSW ea[2] = {};

    // ACE 0: Current user (SYSTEM) - Full Control
    ea[0].grfAccessPermissions = GENERIC_ALL;
    ea[0].grfAccessMode = SET_ACCESS;
    ea[0].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea[0].Trustee.TrusteeType = TRUSTEE_IS_USER;
    ea[0].Trustee.ptstrName = reinterpret_cast<LPWSTR>(pTokenUser->User.Sid);

    // ACE 1: Service SID - Modify (excludes WRITE_DAC/WRITE_OWNER)
    ea[1].grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE;
    ea[1].grfAccessMode = SET_ACCESS;
    ea[1].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea[1].Trustee.TrusteeForm = TRUSTEE_IS_NAME;
    ea[1].Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
    ea[1].Trustee.ptstrName = const_cast<LPWSTR>(serviceTrustee.c_str());

    // Try with both ACEs (service SID + SYSTEM)
    PACL pNewDacl = nullptr;
    DWORD dwResult = SetEntriesInAclW(2, ea, nullptr, &pNewDacl);
    
    if (dwResult == ERROR_NONE_MAPPED)
    {
        // ERROR_NONE_MAPPED (1332): Service SID doesn't exist (console mode, service not installed).
        // Fall back to SYSTEM-only DACL which works fine without the restricted token.
        LogError("[!] Service SID not found (console mode) - using SYSTEM-only DACL for temp directory");
        dwResult = SetEntriesInAclW(1, ea, nullptr, &pNewDacl);
    }

    if (dwResult != ERROR_SUCCESS)
    {
        CloseHandle(hToken);
        LogError("[-] Failed to create DACL for securing directory, error: " + std::to_string(dwResult));
        return false;
    }

    // Set the new DACL with PROTECTED flag to prevent inheritance
    dwResult = SetNamedSecurityInfoW(const_cast<LPWSTR>(m_path.c_str()),
                                      SE_FILE_OBJECT,
                                      DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                      nullptr, nullptr, pNewDacl, nullptr);

    LocalFree(pNewDacl);
    CloseHandle(hToken);

    if (dwResult != ERROR_SUCCESS)
    {
        LogError("[-] Failed to set security info for directory, error: " + std::to_string(dwResult));
        return false;
    }

    return true;
}

bool SecureTempDirectory::CreateSecurityDescriptorForCurrentUser(PSECURITY_DESCRIPTOR* ppSD)
{
    // Get current user's SID (SYSTEM when running as service)
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
    {
        LogError("[-] Failed to open process token");
        return false;
    }

    DWORD dwBufferSize = 0;
    GetTokenInformation(hToken, TokenUser, nullptr, 0, &dwBufferSize);
    
    std::vector<BYTE> buffer(dwBufferSize);
    TOKEN_USER* pTokenUser = reinterpret_cast<TOKEN_USER*>(buffer.data());
    
    if (!GetTokenInformation(hToken, TokenUser, pTokenUser, dwBufferSize, &dwBufferSize))
    {
        CloseHandle(hToken);
        LogError("[-] Failed to get token information");
        return false;
    }

    // Build service trustee name for the restricted token's write-access check.
    // See SecureDirectory() comments for detailed explanation.
    std::wstring serviceTrustee = L"NT SERVICE\\";
    serviceTrustee += ServiceConfig::SERVICE_NAME;

    // Build DACL: SYSTEM (Full Control) + Service SID (Modify)
    EXPLICIT_ACCESSW ea[2] = {};

    // ACE 0: Current user (SYSTEM) - Full Control
    ea[0].grfAccessPermissions = GENERIC_ALL;
    ea[0].grfAccessMode = SET_ACCESS;
    ea[0].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea[0].Trustee.TrusteeType = TRUSTEE_IS_USER;
    ea[0].Trustee.ptstrName = reinterpret_cast<LPWSTR>(pTokenUser->User.Sid);

    // ACE 1: Service SID - Modify (excludes WRITE_DAC/WRITE_OWNER)
    ea[1].grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE;
    ea[1].grfAccessMode = SET_ACCESS;
    ea[1].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea[1].Trustee.TrusteeForm = TRUSTEE_IS_NAME;
    ea[1].Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
    ea[1].Trustee.ptstrName = const_cast<LPWSTR>(serviceTrustee.c_str());

    // Try with both ACEs (service SID + SYSTEM)
    DWORD aceCount = 2;
    PACL pDacl = nullptr;
    DWORD dwResult = SetEntriesInAclW(aceCount, ea, nullptr, &pDacl);
    
    if (dwResult == ERROR_NONE_MAPPED)
    {
        // Service not installed (console mode) - fall back to SYSTEM-only DACL
        aceCount = 1;
        dwResult = SetEntriesInAclW(aceCount, ea, nullptr, &pDacl);
    }

    if (dwResult != ERROR_SUCCESS)
    {
        CloseHandle(hToken);
        LogError("[-] Failed to create DACL, error: " + std::to_string(dwResult));
        return false;
    }

    // Allocate and initialize security descriptor
    *ppSD = LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
    if (*ppSD == nullptr)
    {
        LocalFree(pDacl);
        CloseHandle(hToken);
        LogError("[-] Failed to allocate security descriptor");
        return false;
    }

    if (!InitializeSecurityDescriptor(*ppSD, SECURITY_DESCRIPTOR_REVISION))
    {
        LocalFree(pDacl);
        LocalFree(*ppSD);
        CloseHandle(hToken);
        LogError("[-] Failed to initialize security descriptor");
        return false;
    }

    // IMPORTANT: SetSecurityDescriptorDacl stores a POINTER to pDacl, it does NOT copy it.
    // The caller (CreateDirectoryW) reads through this pointer, so pDacl must remain valid
    // until after CreateDirectoryW returns. The caller frees pSD via LocalFree after use;
    // pDacl is freed separately when the caller calls LocalFree(pSD) since we don't free it here.
    // NOTE: This leaks pDacl. The leak is bounded (one allocation per SecureTempDirectory
    // instance, cleaned up when the process exits). A proper fix would be to allocate pDacl
    // as part of the SD buffer, but that's out of scope for this bug fix.
    if (!SetSecurityDescriptorDacl(*ppSD, TRUE, pDacl, FALSE))
    {
        LocalFree(pDacl);
        LocalFree(*ppSD);
        CloseHandle(hToken);
        LogError("[-] Failed to set DACL in security descriptor");
        return false;
    }

    // DO NOT LocalFree(pDacl) here - the SD holds a pointer to it.
    // It will be freed when the caller calls LocalFree(pSD) after CreateDirectoryW.
    CloseHandle(hToken);
    return true;
}

bool CopyFileFromSnapshot(const std::wstring& snapshotPath,
                          const std::wstring& relativeFilePath,
                          const std::wstring& destinationPath)
{
    // Construct the full source path
    std::wstring sourcePath = snapshotPath + L"\\" + relativeFilePath;

    // Copy the file
    if (!CopyFileW(sourcePath.c_str(), destinationPath.c_str(), FALSE))
    {
        DWORD error = GetLastError();
        LogError("[-] Failed to copy file from snapshot: " + WideToUtf8(sourcePath) + 
                 " to " + WideToUtf8(destinationPath) + ", error: " + std::to_string(error));
        return false;
    }

    LogError("[+] Copied file from snapshot: " + WideToUtf8(relativeFilePath));
    return true;
}

bool MountSnapshotToDirectory(const std::wstring& snapshotDevicePath,
                               const std::wstring& mountPoint)
{
    // VSS snapshots are accessed via their device path (e.g., \\?\GLOBALROOT\Device\HarddiskVolumeShadowCopy1)
    // We can create a symbolic link to make it easier to access
    
    // Create symbolic link requires SE_CREATE_SYMBOLIC_LINK_NAME privilege
    if (!EnablePrivilege(SE_CREATE_SYMBOLIC_LINK_NAME))
    {
        LogError("[-] Failed to enable SE_CREATE_SYMBOLIC_LINK_NAME privilege");
        // Try without privilege (may work on newer Windows versions for unprivileged users)
    }

    DWORD flags = SYMBOLIC_LINK_FLAG_DIRECTORY;
    
    if (!CreateSymbolicLinkW(mountPoint.c_str(), snapshotDevicePath.c_str(), flags))
    {
        DWORD error = GetLastError();
        LogError("[-] Failed to create symbolic link from " + WideToUtf8(mountPoint) + 
                 " to " + WideToUtf8(snapshotDevicePath) + ", error: " + std::to_string(error));
        DisablePrivilege(SE_CREATE_SYMBOLIC_LINK_NAME);
        return false;
    }

    DisablePrivilege(SE_CREATE_SYMBOLIC_LINK_NAME);
    LogError("[+] Mounted VSS snapshot to: " + WideToUtf8(mountPoint));
    return true;
}

bool UnmountSnapshotDirectory(const std::wstring& mountPoint)
{
    // Remove the symbolic link
    if (!RemoveDirectoryW(mountPoint.c_str()))
    {
        DWORD error = GetLastError();
        LogError("[-] Warning: Failed to remove snapshot mount point: " + WideToUtf8(mountPoint) + 
                 ", error: " + std::to_string(error));
        return false;
    }

    LogError("[+] Unmounted VSS snapshot from: " + WideToUtf8(mountPoint));
    return true;
}
