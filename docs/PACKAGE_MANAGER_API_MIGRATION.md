# Windows Package Manager API Migration

## Overview

The `WinAppXPackages` module enumerates modern Windows applications (UWP, MSIX, Store apps, sideloaded apps) using the **Windows Package Manager API** (C++/WinRT). This replaced an earlier registry-based approach that had incomplete coverage.

## Source Files

| File                        | Purpose                                                                  |
|-----------------------------|--------------------------------------------------------------------------|
| `src/WinAppXPackages.h`    | `ModernAppPackage` struct, `ComInitializer` RAII class, enumeration functions |
| `src/WinAppXPackages.cpp`  | Implementation using `Windows::Management::Deployment::PackageManager`   |

## Why the Registry Approach Was Replaced

| Problem                 | Detail                                                            |
|-------------------------|------------------------------------------------------------------|
| **Incomplete coverage** | Registry keys do not capture all package installations            |
| **Store apps missing** | Microsoft Store apps often not in the enumerated registry keys   |
| **User context issues** | Cannot reliably get packages for users not currently logged in   |
| **Timing issues**      | Registry may not be updated immediately after installation        |
| **Not a supported approach** | Not the intended way to enumerate packages per Microsoft       |

## Package Manager API

### Key APIs

```cpp
Windows::Management::Deployment::PackageManager
    FindPackages()              // All packages on system
    FindPackagesForUser(sid)    // Packages for specific user
```

### Coverage

| Package Type                     | Supported |
|----------------------------------|-----------|
| Microsoft Store apps             | Yes       |
| Sideloaded apps (MSIX/APPX)     | Yes       |
| Provisioned packages (OEM/enterprise) | Yes       |
| Framework packages               | Yes       |
| All users (logged-in and offline) | Yes       |
| Full manifest metadata           | Yes       |

## `ModernAppPackage` Structure

```cpp
struct ModernAppPackage
{
    std::wstring packageFullName;
    std::wstring packageFamilyName;
    std::wstring displayName;
    std::wstring publisher;
    std::wstring publisherId;
    std::wstring version;
    std::wstring architecture;
    std::wstring installLocation;
    std::wstring resourceId;
    std::wstring publisherDisplayName;
    std::wstring description;
    std::wstring logo;
    bool isFramework;
    bool isBundle;
    bool isResourcePackage;
    bool isDevelopmentMode;
    std::vector<std::wstring> users;  // SIDs of users who have this package
};
```

## RAII Design

### COM Initialisation

```cpp
class ComInitializer
{
public:
    ComInitializer();   // Initialises COM
    ~ComInitializer();  // Uninitialises COM
    // Copy/move deleted for safety
};
```

## Public API

```cpp
// Enumerate all modern app packages on the system
std::vector<ModernAppPackage> EnumerateAllModernAppPackages();

// Get packages for a specific user (by SID, with optional username for logging)
std::vector<ModernAppPackage> GetModernAppPackagesForUser(
    const std::wstring& userSid,
    const std::wstring& username = L"");
```

## Error Handling

1. **COM initialisation** — graceful failure with logging
2. **WinRT initialisation** — exception handling with details
3. **Per-package errors** — one package failure does not stop enumeration
4. **Missing metadata** — fallback to basic info if manifest unavailable
5. **Access denied** — continues with packages that are accessible

## Build Requirements

- Windows 10 SDK (10.0.17763.0 or later)
- C++/WinRT NuGet package (`Microsoft.Windows.CppWinRT`)
- `WindowsApp.lib` (linked via `#pragma comment` in source)

## Comparison with Registry Method

| Feature            | Registry Method        | Package Manager API    |
|-------------------|-----------------------|------------------------|
| **Store Apps**    | Often missing         | Complete               |
| **All Users**     | Complex hive loading  | Built-in SID parameter  |
| **Metadata**      | Limited fields        | Full manifest data      |
| **Reliability**   | Timing-dependent      | Authoritative source    |
| **Future-proof**  | Registry layout may change | Supported API         |
