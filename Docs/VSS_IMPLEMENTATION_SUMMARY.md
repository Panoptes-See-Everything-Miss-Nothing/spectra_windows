# VSS-Based User App Enumeration Implementation

## Overview
Replaced the direct NTUSER.DAT loading approach with a VSS (Volume Shadow Copy Service) snapshot-based implementation for enumerating installed applications for both logged-in and non-logged-in users.

## Changes Made

### New Files Created

1. **src\WindowsEnum\VSSSnapshot.h / VSSSnapshot.cpp**
   - `VSSSnapshot` class: RAII wrapper for creating and managing VSS snapshots
   - `SecureTempDirectory` class: Creates and secures temporary directories (accessible only by current user)
   - Helper functions for copying files from snapshots and mounting snapshot directories

2. **src\WindowsEnum\RegistryHiveLoader.h / RegistryHiveLoader.cpp**
   - `RegistryHiveLoader` class: RAII-safe registry hive loading and unloading
   - Automatically enables/disables required privileges (SE_BACKUP_NAME, SE_RESTORE_NAME)
   - Ensures proper cleanup even if exceptions occur

### Modified Files

3. **src\WindowsEnum\Win32Apps.cpp**
   - Updated `GetUserInstalledApps()` function to use two different approaches:
     - **Logged-in users**: Direct registry access via HKEY_USERS\{SID}
     - **Non-logged-in users**: VSS snapshot approach

## VSS Approach Workflow (for non-logged-in users)

1. **Create VSS Snapshot**: Creates a shadow copy of the volume containing the user profile (e.g., C:\)
2. **Create Secure Temp Directory**: Creates `C:\SpectraVM_Temp` with ACLs restricting access to only the current user
3. **Mount VSS Snapshot**: Creates a symbolic link at `C:\SpectraVM_Temp\VSS_Snapshot` pointing to the snapshot device path
4. **Copy NTUSER.DAT**: Copies the user's NTUSER.DAT from the snapshot to the temp directory
5. **Load Registry Hive**: Loads the copied hive as read-only under `HKEY_USERS\SpectraVM_Hive_{username}`
6. **Enumerate Apps**: Reads installed applications from `Software\Microsoft\Windows\CurrentVersion\Uninstall`
7. **Cleanup**: RAII classes automatically:
   - Unload the registry hive
   - Unmount the snapshot symbolic link
   - Delete the VSS snapshot
   - Remove the temporary directory and all contents

## Security Features

- **Restricted Access**: `C:\SpectraVM_Temp` is secured with DACL to allow only the current user
- **No Drive Letters**: Snapshot mounted to directory path (not exposed as drive letter) to comply with strict network policies
- **Read-Only Access**: Registry hives are loaded in read-only mode
- **RAII Safety**: All resources are automatically cleaned up, even on failure

## Benefits

1. **No File Locking Issues**: VSS provides a point-in-time snapshot, avoiding conflicts with active NTUSER.DAT files
2. **Consistent State**: Snapshot ensures data consistency during enumeration
3. **Proper Cleanup**: RAII pattern guarantees resource cleanup
4. **Network Policy Compliance**: No temporary drive letters created
5. **Security**: Temporary files accessible only to the executing user

## Requirements

- Windows Vista or later (for VSS support)
- Administrator privileges (required for VSS operations)
- VssApi.lib (automatically linked via `#pragma comment`)

## Project Files

The following files need to be added to the Visual Studio project:
- src\WindowsEnum\VSSSnapshot.h
- src\WindowsEnum\VSSSnapshot.cpp
- src\WindowsEnum\RegistryHiveLoader.h
- src\WindowsEnum\RegistryHiveLoader.cpp

## Testing Notes

The implementation should be tested with:
1. Currently logged-in users
2. Users who have logged in previously but are not currently logged in
3. Edge cases like missing NTUSER.DAT files
4. Permission scenarios (administrator vs. non-administrator)
