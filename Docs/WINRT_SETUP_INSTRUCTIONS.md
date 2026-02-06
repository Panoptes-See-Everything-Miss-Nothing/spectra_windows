# C++/WinRT Setup Instructions

## Current Status

The new `WinAppXPackages.h/cpp` files have been created but require C++/WinRT support to compile.

## Files Created

? `src\WindowsEnum\WinAppXPackages.h` - Header with modern app package structures and functions  
? `src\WindowsEnum\WinAppXPackages.cpp` - Implementation using Windows Package Manager API  
? `PACKAGE_MANAGER_API_MIGRATION.md` - Complete documentation

## Setup Options

### Option 1: Install C++/WinRT NuGet Package (Recommended)

1. Open Visual Studio
2. Right-click on project ? Manage NuGet Packages
3. Browse for `Microsoft.Windows.CppWinRT`
4. Install the latest version (2.0.x)
5. Rebuild solution

OR using Package Manager Console:
```powershell
Install-Package Microsoft.Windows.CppWinRT
```

### Option 2: Manual vcpkg Installation

```cmd
vcpkg install cppwinrt:x64-windows
vcpkg integrate install
```

### Option 3: Use Windows SDK C++/WinRT (if SDK 10.0.17134.0+)

Add to project file in all `<ItemDefinitionGroup>`:

```xml
<ClCompile>
  <AdditionalOptions>/await:strict %(AdditionalOptions)</AdditionalOptions>
  <AdditionalUsingDirectories>$(WindowsSDK_MetadataPath)</AdditionalUsingDirectories>
</ClCompile>
```

## Add Files to Project

Once C++/WinRT is configured, add to `Windows-Info-Gathering.vcxproj`:

### In `<ItemGroup>` for ClCompile:
```xml
<ClCompile Include="src\WindowsEnum\WinAppXPackages.cpp" />
```

### In `<ItemGroup>` for ClInclude:
```xml
<ClInclude Include="src\WindowsEnum\WinAppXPackages.h" />
```

## Update Main.cpp

Replace old registry-based enumeration with new API-based:

### Old Code:
```cpp
#include "AppXPackages.h"

auto packages = EnumerateAppXPackages();  // Registry-based, incomplete
```

### New Code:
```cpp
#include "WinAppXPackages.h"

auto packages = EnumerateAllModernAppPackages();  // API-based, complete
```

## Verify Build

After setup, verify by building:

```
Build ? Rebuild Solution
```

Expected output:
```
Build succeeded
0 Errors
0 Warnings
```

## Testing

Run the application and verify all Store apps are now detected:

```
[+] Initializing Windows Package Manager...
[+] Enumerating all modern app packages...
[+] Successfully enumerated XXX modern app packages
```

## Troubleshooting

### Error: Cannot open include file 'winrt/Windows.Management.Deployment.h'

**Solution:** C++/WinRT not installed. Follow Option 1 above.

### Error: C2039 or C3779 related to WinRT types

**Solution:** Ensure Windows SDK 10.0.17763.0 or later is installed.

### Error: LNK2019 unresolved external symbol

**Solution:** Add `WindowsApp.lib` to linker input (already in code via pragma comment).

## Benefits After Setup

Once configured, you'll have:

? **Complete Coverage** - All Store apps, sideloaded apps, provisioned packages  
? **Reliable Detection** - Authoritative Windows API  
? **Per-User Visibility** - See packages for any user  
? **Rich Metadata** - Full manifest data access  
? **Future-Proof** - Supported API that won't break  

## Next Steps

1. Install C++/WinRT via NuGet (Option 1)
2. Add .cpp/.h files to project
3. Update Main.cpp to use new API
4. Build and test
5. Verify ChatGPT and other Store apps are detected

## Documentation

See `PACKAGE_MANAGER_API_MIGRATION.md` for:
- Complete API documentation
- Usage examples
- Migration guide from registry method
- Performance considerations
- Security features
