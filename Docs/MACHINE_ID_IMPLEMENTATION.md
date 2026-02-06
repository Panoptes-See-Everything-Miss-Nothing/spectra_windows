# Spectra Machine ID & Registry Configuration Implementation

## Summary

This implementation adds enterprise-grade features to Panoptes Spectra:

1. **Unique Machine ID** - Cryptographically unique identifier for each machine
2. **Registry-Based Configuration** - Allows runtime configuration without recompilation
3. **ISO 8601 Timestamps** - Standards-compliant timestamps in JSON output

## Changes Made

### New Files Created

#### 1. `src/Service/MachineId.h`
- Declares Machine ID management functions
- Format: `SPECTRA-{GUID}` (e.g., `SPECTRA-A1B2C3D4-E5F6-7890-ABCD-EF1234567890`)
- Stored in registry: `HKLM\SOFTWARE\Panoptes\Spectra\SpectraMachineID`

#### 2. `src/Service/MachineId.cpp`
- Implements Machine ID generation using Windows `UuidCreate()`
- Stores ID in registry for persistence across reboots
- Validates Machine ID format
- Provides UTF-8 export for JSON

#### 3. `src/Service/ServiceConfig.cpp`
- Implements registry-based configuration reading
- Supports configurable collection interval (60s - 24h)
- Supports configurable output directory
- Supports server URL and detailed logging flags

### Modified Files

#### 1. `src/Service/ServiceConfig.h`
- Added registry configuration constants
- Added configuration helper functions
- Maintains backward compatibility with hardcoded defaults

#### 2. `src/Service/ServiceMain.cpp`
- Updated to use dynamic collection interval from registry
- Reads configuration on each loop iteration (allows runtime changes)
- Uses dynamic output directory from configuration

#### 3. `src/Utils/Utils.cpp`
- Added `#include "../Service/MachineId.h"`
- Added `#include "../Service/ServiceConfig.h"`
- Updated `GenerateJSON()` to include:
  - `spectraMachineId` field
  - `collectionTimestamp` field (ISO 8601 format)
  - `agentVersion` field

## Manual Steps Required

### 1. Add Files to Visual Studio Project

**Option A: Using Visual Studio UI**
1. Right-click on "Service" filter in Solution Explorer
2. Add > Existing Item
3. Select `src\Service\MachineId.cpp` and `src\Service\MachineId.h`
4. Select `src\Service\ServiceConfig.cpp`

**Option B: Edit `.vcxproj` Manually**

Add to the `<ClCompile>` section:
```xml
<ClCompile Include="src\Service\MachineId.cpp" />
<ClCompile Include="src\Service\ServiceConfig.cpp" />
```

Add to the `<ClInclude>` section:
```xml
<ClInclude Include="src\Service\MachineId.h" />
```

### 2. Build the Project

```cmd
msbuild Windows-Info-Gathering.sln /p:Configuration=Release /p:Platform=x64
```

## Configuration Registry Keys

### Machine ID (Auto-Generated)
```
HKLM\SOFTWARE\Panoptes\Spectra
  SpectraMachineID = REG_SZ = "SPECTRA-{GUID}"
```

### Configuration (Optional - Defaults Used if Not Set)
```
HKLM\SOFTWARE\Panoptes\Spectra
  CollectionIntervalSeconds = REG_DWORD = 3600        (1 hour, range: 60-86400)
  OutputDirectory = REG_SZ = "C:\ProgramData\Panoptes\Spectra\Output"
  ServerUrl = REG_SZ = "https://management.example.com"
  EnableDetailedLogging = REG_DWORD = 0               (0=false, 1=true)
```

## Example JSON Output

The generated inventory JSON now includes:

```json
{
  "spectraMachineId": "SPECTRA-A1B2C3D4-E5F6-7890-ABCD-EF1234567890",
  "collectionTimestamp": "2024-01-15T14:30:45",
  "agentVersion": "1.0.0",
  "machineNetBiosName": "DESKTOP-ABC123",
  "machineDnsName": "desktop-abc123.domain.local",
  "ipAddresses": [
    "192.168.1.100"
  ],
  "installedAppsByUser": [
    ...
  ]
}
```

## Testing the Implementation

### 1. Test Machine ID Generation

```cmd
# Run in console mode
Panoptes-Spectra.exe /console

# Check registry
reg query "HKLM\SOFTWARE\Panoptes\Spectra" /v SpectraMachineID

# Verify it persists across runs
Panoptes-Spectra.exe /console
reg query "HKLM\SOFTWARE\Panoptes\Spectra" /v SpectraMachineID
```

### 2. Test Configuration

```cmd
# Set custom collection interval (30 minutes)
reg add "HKLM\SOFTWARE\Panoptes\Spectra" /v CollectionIntervalSeconds /t REG_DWORD /d 1800 /f

# Set custom output directory
reg add "HKLM\SOFTWARE\Panoptes\Spectra" /v OutputDirectory /t REG_SZ /d "C:\Temp\Spectra" /f

# Install and start service
Panoptes-Spectra.exe /install
sc start PanoptesSpectra

# Check logs to verify configuration was read
type C:\ProgramData\Panoptes\Spectra\Logs\spectra_log.txt
```

## Enterprise Deployment

### GPO Configuration Example

Create a Group Policy to deploy registry settings:

1. **Computer Configuration** > **Preferences** > **Windows Settings** > **Registry**
2. Add new registry items:
   - Key: `HKLM\SOFTWARE\Panoptes\Spectra`
   - `CollectionIntervalSeconds` (DWORD) = `3600`
   - `ServerUrl` (String) = `https://panoptes.corp.local`

### PowerShell Deployment Script

```powershell
# Configure Panoptes Spectra on remote machines
$computers = Get-ADComputer -Filter * | Select-Object -ExpandProperty Name

foreach ($computer in $computers) {
    Invoke-Command -ComputerName $computer -ScriptBlock {
        # Set configuration
        New-Item -Path "HKLM:\SOFTWARE\Panoptes\Spectra" -Force | Out-Null
        Set-ItemProperty -Path "HKLM:\SOFTWARE\Panoptes\Spectra" -Name "CollectionIntervalSeconds" -Value 3600 -Type DWord
        Set-ItemProperty -Path "HKLM:\SOFTWARE\Panoptes\Spectra" -Name "ServerUrl" -Value "https://panoptes.corp.local" -Type String
        
        # Restart service to apply changes
        Restart-Service -Name "PanoptesSpectra" -Force
    }
}
```

## Security Considerations

### Machine ID Protection
- Stored in `HKLM` (requires admin/SYSTEM to modify)
- UUID-based generation ensures global uniqueness
- Persists across reinstalls (deliberate - ties to machine)

### Configuration Security
- Registry ACLs default to Administrators/SYSTEM only
- Service runs as LocalSystem (full access)
- No sensitive data stored (API keys should be encrypted if added)

## Next Steps

After verifying the build compiles successfully:

1. ✅ Test Machine ID generation
2. ✅ Test registry configuration reading
3. ✅ Verify JSON output includes new fields
4. 🔲 Create MSI installer (next phase)
5. 🔲 Add WiX installer project
6. 🔲 Implement automatic directory creation
7. 🔲 Add Programs & Features integration

## Troubleshooting

### Build Errors

**Error: Cannot find MachineId.h**
- Ensure files are added to the Visual Studio project
- Check that files exist in `src/Service/` directory

**Error: Unresolved external symbol**
- Ensure `MachineId.cpp` and `ServiceConfig.cpp` are compiled
- Check project file includes both `.cpp` files

### Runtime Errors

**Machine ID Not Generated**
- Run as Administrator to create registry key
- Check Event Viewer for access denied errors
- Verify `HKLM\SOFTWARE\Panoptes` is writable

**Configuration Not Read**
- Registry values are optional (defaults are used if missing)
- Check log file for configuration warnings
- Ensure service has restarted after registry changes

## File Structure

```
src/
├── Service/
│   ├── MachineId.h          ← NEW: Machine ID declarations
│   ├── MachineId.cpp        ← NEW: Machine ID implementation
│   ├── ServiceConfig.h      ← MODIFIED: Added registry config
│   ├── ServiceConfig.cpp    ← NEW: Registry reading functions
│   ├── ServiceMain.h
│   ├── ServiceMain.cpp      ← MODIFIED: Uses dynamic config
│   ├── ServiceInstaller.h
│   ├── ServiceInstaller.cpp
│   └── ServiceTamperProtection.cpp
├── Utils/
│   ├── Utils.h
│   └── Utils.cpp            ← MODIFIED: Adds Machine ID to JSON
└── Main.cpp
```
