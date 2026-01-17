# Windows Package Manager API Implementation

## Overview

Replaced the unreliable registry-based AppX package enumeration with the **Windows Package Manager API** (C++/WinRT), which provides complete visibility into all modern apps including Store apps, sideloaded apps, and provisioned packages.

## Why Registry Approach Was Failing

### Problems with Registry Method:
1. **Incomplete Coverage** - Registry doesn't capture all package installations
2. **Store Apps Missing** - Microsoft Store apps often not properly registered in the enumerated registry keys
3. **User Context Issues** - Can't reliably get packages for users not currently logged in
4. **Timing Issues** - Registry may not be updated immediately after installation
5. **No API Support** - Not the intended way to enumerate packages per Microsoft

## Windows Package Manager API Solution

### Key APIs Used:
```cpp
Windows::Management::Deployment::PackageManager
- FindPackages()              // All packages on system
- FindPackagesForUser(sid)    // Packages for specific user
- FindUsers(packageName)      // Users who have a package
```

### Complete Coverage:
? **Microsoft Store apps** - Fully supported  
? **Sideloaded apps** - MSIX/APPX from any source  
? **Provisioned packages** - Pre-installed OEM/enterprise apps  
? **Framework packages** - Dependencies and frameworks  
? **All users** - Works for logged-in and non-logged-in users  
? **Complete metadata** - Full manifest data access  

## Files Created

### 1. WinAppXPackages.h
- `ModernAppPackage` struct with comprehensive package information
- `ComInitializer` RAII class for COM management
- `EnumerateAllModernAppPackages()` - Gets all packages system-wide
- `GetModernAppPackagesForUser(sid)` - Gets packages for specific user

### 2. WinAppXPackages.cpp
- Full implementation using Windows::Management::Deployment APIs
- Proper error handling with try-catch for each package
- RAII-based COM initialization
- Efficient memory management (no unnecessary heap allocations)

## Key Features

### RAII Design
```cpp
class ComInitializer
{
public:
    ComInitializer();   // Initializes COM
    ~ComInitializer();  // Uninitializes COM
    // Copy/move deleted for safety
};
```

### Comprehensive Package Information
```cpp
struct ModernAppPackage
{
    std::wstring packageFullName;        // Full package identifier
    std::wstring packageFamilyName;      // Family name
    std::wstring displayName;            // User-friendly name
    std::wstring publisher;              // Publisher cert info
    std::wstring version;                // Version string
    std::wstring architecture;           // x86, x64, ARM, ARM64
    std::wstring installLocation;        // Install path
    std::wstring publisherDisplayName;   // Publisher display name
    std::wstring description;            // App description
    bool isFramework;                    // Framework vs app
    bool isBundle;                       // Bundle package
    bool isResourcePackage;              // Resource pack
    bool isDevelopmentMode;              // Dev mode install
    std::vector<std::wstring> users;     // SIDs of users with this package
};
```

### Security Features
- ? No unsafe string operations
- ? Exception handling for each package (one failure doesn't stop enumeration)
- ? RAII for resource management
- ? No raw pointers
- ? Move semantics for efficiency

## Project Configuration Required

### 1. Add C++/WinRT Support

Edit `Windows-Info-Gathering.vcxproj` and add to each `<ItemDefinitionGroup>`:

```xml
<ClCompile>
  <AdditionalOptions>/await:strict %(AdditionalOptions)</AdditionalOptions>
  <AdditionalUsingDirectories>$(WindowsSDK_MetadataPath)</AdditionalUsingDirectories>
</ClCompile>
```

### 2. Add NuGet Package

Install C++/WinRT NuGet package:
```
Install-Package Microsoft.Windows.CppWinRT
```

Or manually add to project:
```xml
<ItemGroup>
  <PackageReference Include="Microsoft.Windows.CppWinRT" Version="2.0.230706.1" />
</ItemGroup>
```

### 3. Add Source Files to Project

Add to `<ItemGroup>` for ClCompile:
```xml
<ClCompile Include="src\WindowsEnum\WinAppXPackages.cpp" />
```

Add to `<ItemGroup>` for ClInclude:
```xml
<ClInclude Include="src\WindowsEnum\WinAppXPackages.h" />
```

### 4. Update Target Platform Version

Ensure minimum Windows 10 SDK (10.0.17763.0 or later):
```xml
<WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion>
```

## Usage Example

### Get All Packages System-Wide
```cpp
#include "WinAppXPackages.h"

auto packages = EnumerateAllModernAppPackages();

for (const auto& pkg : packages)
{
    std::wcout << L"Package: " << pkg.displayName << L"\n";
    std::wcout << L"Version: " << pkg.version << L"\n";
    std::wcout << L"Location: " << pkg.installLocation << L"\n";
    
    // Show users who have this package
    for (const auto& userSid : pkg.users)
    {
        std::wcout << L"  User: " << userSid << L"\n";
    }
}
```

### Get Packages for Specific User
```cpp
std::wstring userSid = L"S-1-5-21-...";
auto userPackages = GetModernAppPackagesForUser(userSid);

std::wcout << L"Found " << userPackages.size() 
           << L" packages for user\n";
```

## Advantages Over Registry Method

| Feature | Registry Method | Package Manager API |
|---------|----------------|---------------------|
| **Store Apps** | ? Often missing | ? Complete |
| **All Users** | ? Complex | ? Built-in |
| **Metadata** | ? Limited | ? Complete |
| **Reliability** | ? Timing issues | ? Authoritative |
| **Performance** | ?? Multiple registry opens | ? Optimized |
| **Future-proof** | ? Registry can change | ? Supported API |
| **Permissions** | ?? Need admin | ? Works as user |

## Error Handling

The implementation includes comprehensive error handling:

1. **COM Initialization** - Graceful failure with logging
2. **WinRT Initialization** - Exception handling with details
3. **Per-Package Errors** - One package failure doesn't stop enumeration
4. **Missing Metadata** - Fallback to basic info if manifest unavailable
5. **Access Denied** - Continues with packages that are accessible

## Performance Considerations

- **No Unnecessary Allocations** - Uses move semantics
- **Efficient Iteration** - Direct iteration over WinRT collections
- **Lazy Evaluation** - Only gets data when needed
- **Exception Safety** - RAII ensures cleanup

## Testing Recommendations

1. **Standard User** - Verify works without admin
2. **Multiple Users** - Test with various user accounts
3. **Store Apps** - Install app from Store and verify detection
4. **Sideloaded Apps** - Test with .appx sideload
5. **Framework Packages** - Verify frameworks enumerated
6. **Offline Users** - Test with users not currently logged in

## Migration Path

### Old Code (Registry-based):
```cpp
auto packages = EnumerateAppXPackages();  // Registry method
```

### New Code (Package Manager API):
```cpp
auto packages = EnumerateAllModernAppPackages();  // API method
```

The new API provides a superset of information, so existing code can be updated gradually.

## Build Instructions

1. Install C++/WinRT NuGet package (if not using vcpkg)
2. Add new .cpp and .h files to project
3. Build solution
4. Run with normal user or admin privileges

## Known Limitations

- Requires Windows 10 or later
- Some package metadata may be inaccessible for protected system packages
- Package Manager API requires Windows SDK 10.0.17763.0+

## Future Enhancements

Consider adding:
- Package dependencies enumeration
- App capabilities and permissions
- Package state (running, suspended, etc.)
- Installation source tracking
- Package size information
