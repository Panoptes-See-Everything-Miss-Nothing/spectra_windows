# VSS-Based User Application Enumeration

## Overview

Panoptes Spectra uses Volume Shadow Copy Service (VSS) snapshots to safely enumerate installed applications for users who are not currently logged in. This avoids file-locking issues with `NTUSER.DAT` and ensures a consistent point-in-time view of the registry hive.

## Source Files

| File | Purpose |
|------|---------|
| `src/VSSSnapshot.h` | `VSSSnapshot` RAII class and `SecureTempDirectory` helper |
| `src/VSSSnapshot.cpp` | VSS snapshot creation, mounting, and cleanup |
| `src/RegistryHiveLoader.h` | `RegistryHiveLoader` RAII class |
| `src/RegistryHiveLoader.cpp` | Offline hive loading with automatic privilege management |
| `src/Win32Apps.cpp` | `GetUserInstalledApps()` — dispatches between live and VSS-based enumeration |

## How It Works

For each user profile, `GetUserInstalledApps()` uses one of two approaches:

1. **Logged-in users:** Direct registry access via `HKEY_USERS\{SID}` (hive already loaded).
2. **Offline users:** VSS snapshot workflow (described below).

### VSS Workflow for Offline Users

```
1. Create VSS snapshot of the volume containing the user profile (e.g., C:\)
2. Create secure temporary directory (unique per instance, restricted ACLs)
3. Mount VSS snapshot to a directory path (no drive letter)
4. Copy user's NTUSER.DAT from the snapshot to the temp directory
5. Load the copied hive as read-only under HKEY_USERS\SpectraVM_Hive_{username}
6. Enumerate apps from Software\Microsoft\Windows\CurrentVersion\Uninstall
7. RAII cleanup: unload hive → unmount snapshot → delete snapshot → remove temp dir
```

## Security Features

| Feature | Detail |
|---------|--------|
| **Unique temp directory** | Each run creates `{base}_{PID}_{timestamp}` — no reuse, no race conditions |
| **Restricted ACLs** | Temp directory accessible only to the current user |
| **No drive letters** | Snapshot mounted to directory path (not exposed as a drive letter) |
| **Read-only hive access** | Registry hives are loaded in read-only mode |
| **RAII cleanup** | All resources (VSS snapshot, temp directory, loaded hive) are automatically cleaned up, even on failure |
| **Symlink validation** | Temp directory is validated against reparse point attacks |

## Secure Temporary Directory

Each instance creates a unique directory to prevent race conditions and file-handle attacks:

```
{basePath}_{ProcessID}_{HighResolutionTimestamp}
Example: C:\SpectraVM_Temp_8432_132845729183456789
```

- **Process ID** — unique per running process
- **High-resolution timestamp** — nanosecond precision since epoch
- Multiple simultaneous instances each get their own directory

The destructor removes the unique directory but does **not** remove the parent path (which may be used by other instances).

## Privilege Requirements

- **SE_BACKUP_NAME** — required to copy `NTUSER.DAT` and load the hive
- **SE_RESTORE_NAME** — required to load the hive via `RegLoadKeyW`
- `RegistryHiveLoader` enables these privileges on construction and disables them on destruction

The service runs as LocalSystem, which has these privileges available. Administrator accounts do not have SE_RESTORE_NAME by default.

## System Requirements

- Windows 10 / Windows Server 2016 or later
- SYSTEM or Administrator privileges (SYSTEM required for full per-user coverage)
- `VssApi.lib` (linked automatically via `#pragma comment`)

## Testing

| Scenario | Expected Result |
|----------|-----------------|
| Currently logged-in user | Apps enumerated via direct `HKEY_USERS` access |
| User with profile but not logged in | Apps enumerated via VSS snapshot |
| Missing `NTUSER.DAT` | Graceful skip with log warning |
| Insufficient privileges | Graceful failure with descriptive log message |
| Multiple simultaneous runs | Each gets unique temp directory, no interference |
