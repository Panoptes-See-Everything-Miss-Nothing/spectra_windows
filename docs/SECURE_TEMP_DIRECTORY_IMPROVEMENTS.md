# Secure Temporary Directory Design

## Overview

The `SecureTempDirectory` class (in `src/VSSSnapshot.h` / `src/VSSSnapshot.cpp`) provides RAII-managed temporary directories with per-instance uniqueness and restricted ACLs. It is used by the VSS snapshot workflow to safely stage copies of offline registry hives.

## Directory Naming

Each instance creates a unique directory using the process ID and a high-resolution timestamp:

```
{basePath}_{ProcessID}_{HighResolutionTimestamp}
```

**Examples:**
- `C:\SpectraVM_Temp_8432_132845729183456789`
- `C:\SpectraVM_Temp_9120_132845730298765432`

### Why This Is Unique

1. **Process ID** — unique per running process
2. **High-resolution timestamp** — nanosecond precision since epoch
3. **Combination** — collision is astronomically unlikely even with simultaneous starts

## Security Properties

### Race Condition Elimination

Each instance gets its own directory. There is no shared state between runs, so no cleanup of existing contents is needed and no TOCTOU window exists.

### No Attacker File Pre-Placement

The directory is created fresh. An attacker cannot pre-create files with open handles inside it because the directory name is unpredictable.

### Parent Directory Validation

If the parent path exists, it is validated with `IsDirectorySafeToUse()` before proceeding:
- Must be a real directory (not a file)
- Must not be a reparse point (symlink or junction)
- Must be owned by SYSTEM or the current user

### Restricted ACLs

The directory is created with a security descriptor that grants access **only to the current user**:

```cpp
SECURITY_ATTRIBUTES sa = {};
sa.lpSecurityDescriptor = pSD;  // Only current user has access
sa.bInheritHandle = FALSE;

CreateDirectoryW(m_path.c_str(), &sa);
```

## RAII Lifecycle

```
Constructor:
  1. Generate unique path: {base}_{PID}_{timestamp}
  2. Validate parent directory (if it exists)
  3. Create directory with restricted security descriptor
  4. Verify security settings
  5. Set m_valid = true

Destructor:
  1. If m_valid and m_path is not empty:
     Remove the unique directory and all contents
  2. Parent directory is NOT removed (may be used by other instances)
```

## Usage Pattern

```cpp
SecureTempDirectory tempDir(L"C:\\SpectraVM_Temp");

if (tempDir.IsValid())
{
    // Use tempDir.GetPath() for staging files
    // e.g., "C:\SpectraVM_Temp_5000_132845729183456789"
}
// Destructor cleans up automatically
```

## Scenarios

### Single Instance

```
Run 1: Creates C:\SpectraVM_Temp_5000_132845729183456789
       Used for VSS operations, cleaned up on exit.

Run 2: Creates C:\SpectraVM_Temp_5100_132845730298765432
       Completely independent from Run 1.
```

### Multiple Simultaneous Instances

```
Instance A (PID 5000): C:\SpectraVM_Temp_5000_132845729183456789
Instance B (PID 5100): C:\SpectraVM_Temp_5100_132845729183467890
Instance C (PID 5200): C:\SpectraVM_Temp_5200_132845729183478901

All run independently, no interference.
```

### Crash Recovery

```
Run 1: Creates C:\SpectraVM_Temp_5000_132845729183456789
       Process crashes — directory left behind.

Run 2: Creates C:\SpectraVM_Temp_5100_132845730298765432
       Works normally. The leftover from Run 1 can be cleaned up manually.
