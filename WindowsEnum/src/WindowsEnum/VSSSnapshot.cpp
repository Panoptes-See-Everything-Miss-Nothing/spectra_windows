#include "VSSSnapshot.h"
#include "PrivMgmt.h"
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

    // Initialize COM
    hr = CoInitialize(nullptr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
    {
        LogError("[-] Failed to initialize COM, HRESULT: 0x" + 
                 std::to_string(static_cast<unsigned long>(hr)));
        return false;
    }

    // Enable backup privilege
    if (!EnablePrivilege(SE_BACKUP_NAME))
    {
        LogError("[-] Failed to enable SE_BACKUP_NAME privilege for VSS");
        CoUninitialize();
        return false;
    }

    // Create VSS backup components
    hr = CreateVssBackupComponents(&pImpl->pBackup);
    if (FAILED(hr))
    {
        LogError("[-] Failed to create VSS backup components, HRESULT: 0x" + 
                 std::to_string(static_cast<unsigned long>(hr)));
        DisablePrivilege(SE_BACKUP_NAME);
        CoUninitialize();
        return false;
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

    // Apply security settings to ensure they're correct
    if (!SecureDirectory())
    {
        LogError("[-] Failed to secure temp directory: " + WideToUtf8(m_path));
        // Try to remove the directory we just created
        RemoveDirectoryW(m_path.c_str());
        return;
    }

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
    // Get current user's SID
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

    // Create a new DACL with only the current user having full control
    EXPLICIT_ACCESSW ea = {};
    ea.grfAccessPermissions = GENERIC_ALL;
    ea.grfAccessMode = SET_ACCESS;
    ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_USER;
    ea.Trustee.ptstrName = reinterpret_cast<LPWSTR>(pTokenUser->User.Sid);

    PACL pNewDacl = nullptr;
    DWORD dwResult = SetEntriesInAclW(1, &ea, nullptr, &pNewDacl);
    
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
    // Get current user's SID
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

    // Create DACL with only current user
    EXPLICIT_ACCESSW ea = {};
    ea.grfAccessPermissions = GENERIC_ALL;
    ea.grfAccessMode = SET_ACCESS;
    ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_USER;
    ea.Trustee.ptstrName = reinterpret_cast<LPWSTR>(pTokenUser->User.Sid);

    PACL pDacl = nullptr;
    DWORD dwResult = SetEntriesInAclW(1, &ea, nullptr, &pDacl);
    
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

    if (!SetSecurityDescriptorDacl(*ppSD, TRUE, pDacl, FALSE))
    {
        LocalFree(pDacl);
        LocalFree(*ppSD);
        CloseHandle(hToken);
        LogError("[-] Failed to set DACL in security descriptor");
        return false;
    }

    LocalFree(pDacl);
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
