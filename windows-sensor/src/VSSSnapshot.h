#pragma once

#include "./Utils/Utils.h"
#include <windows.h>
#include <vss.h>
#include <vswriter.h>
#include <vsbackup.h>
#include <memory>
#include <string>

// Link required libraries for VSS
#pragma comment(lib, "VssApi.lib")
#pragma comment(lib, "Ole32.lib")

// Forward declaration
class VSSSnapshotImpl;

// RAII wrapper for VSS snapshot operations
class VSSSnapshot
{
public:
    VSSSnapshot();
    ~VSSSnapshot();

    // Delete copy constructor and assignment operator
    VSSSnapshot(const VSSSnapshot&) = delete;
    VSSSnapshot& operator=(const VSSSnapshot&) = delete;

    // Create a snapshot of the specified volume
    bool CreateSnapshot(const std::wstring& volumePath);

    // Get the snapshot device path
    std::wstring GetSnapshotPath() const;

    // Check if snapshot was successfully created
    bool IsValid() const;

private:
    std::unique_ptr<VSSSnapshotImpl> pImpl;
};

// Helper class for secure temporary directory management
class SecureTempDirectory
{
public:
    explicit SecureTempDirectory(const std::wstring& basePath);
    ~SecureTempDirectory();

    // Delete copy constructor and assignment operator
    SecureTempDirectory(const SecureTempDirectory&) = delete;
    SecureTempDirectory& operator=(const SecureTempDirectory&) = delete;

    // Get the temporary directory path
    std::wstring GetPath() const { return m_path; }

    // Check if directory was successfully created
    bool IsValid() const { return m_valid; }

private:
    std::wstring m_path;
    bool m_valid;

    // Secure the directory (only accessible by current user)
    bool SecureDirectory();
    
    // Create a security descriptor that grants access only to current user
    bool CreateSecurityDescriptorForCurrentUser(PSECURITY_DESCRIPTOR* ppSD);
};

// Helper: Copy a file from VSS snapshot to temporary location
bool CopyFileFromSnapshot(const std::wstring& snapshotPath, 
                          const std::wstring& relativeFilePath, 
                          const std::wstring& destinationPath);

// Helper: Mount VSS snapshot to a directory using symbolic link
bool MountSnapshotToDirectory(const std::wstring& snapshotDevicePath, 
                               const std::wstring& mountPoint);

// Helper: Unmount VSS snapshot directory
bool UnmountSnapshotDirectory(const std::wstring& mountPoint);
