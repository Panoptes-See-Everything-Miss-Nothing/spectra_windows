# Quick Start Guide - Panoptes Spectra Machine ID

## ? Implementation Complete

All changes have been successfully implemented and the project **builds without errors**.

## What Was Added

### New Features
1. **Unique Machine ID** - `SPECTRA-{GUID}` format stored in registry
2. **Registry Configuration** - Runtime configuration without recompilation  
3. **Enhanced JSON** - Includes machine ID, timestamp, and version

### New Files
- ? `src/Service/MachineId.h` - Machine ID interface
- ? `src/Service/MachineId.cpp` - Machine ID implementation
- ? `src/Service/ServiceConfig.cpp` - Registry configuration reader
- ? `MACHINE_ID_IMPLEMENTATION.md` - Detailed documentation
- ? `IMPLEMENTATION_SUMMARY.md` - Feature summary

### Modified Files
- ? `src/Service/ServiceConfig.h` - Added registry constants
- ? `src/Service/ServiceMain.cpp` - Uses dynamic config
- ? `src/Utils/Utils.cpp` - Adds machine ID to JSON
- ? `Windows-Info-Gathering.vcxproj` - Includes new files
- ? `Windows-Info-Gathering.vcxproj.filters` - Updated filters

## Quick Test (5 minutes)

### 1. Build the Project
```cmd
cd D:\spectra\WindowsEnum
msbuild Windows-Info-Gathering.sln /p:Configuration=Release /p:Platform=x64
```

### 2. Run in Console Mode
```cmd
cd bin\x64\Release
Panoptes-Spectra.exe /console
```

### 3. Check Machine ID
```cmd
# View in JSON
type inventory.json | findstr "spectraMachineId"

# View in Registry
reg query "HKLM\SOFTWARE\Panoptes\Spectra" /v SpectraMachineID
```

### 4. Expected Output
```json
{
  "spectraMachineId": "SPECTRA-12345678-ABCD-EF01-2345-6789ABCDEF01",
  "collectionTimestamp": "2024-01-15T14:30:45",
  "agentVersion": "1.0.0",
  ...
}
```

## Configuration Examples

### Change Collection Interval to 30 Minutes
```cmd
reg add "HKLM\SOFTWARE\Panoptes\Spectra" /v CollectionIntervalSeconds /t REG_DWORD /d 1800 /f
```

### Change Output Directory
```cmd
reg add "HKLM\SOFTWARE\Panoptes\Spectra" /v OutputDirectory /t REG_SZ /d "D:\Spectra\Output" /f
```

### Set Server URL (for future use)
```cmd
reg add "HKLM\SOFTWARE\Panoptes\Spectra" /v ServerUrl /t REG_SZ /d "https://panoptes.corp.local" /f
```

## Install as Service

```cmd
# Install
Panoptes-Spectra.exe /install

# Start
sc start PanoptesSpectra

# Check status
sc query PanoptesSpectra

# View output
dir "C:\ProgramData\Panoptes\Spectra\Output"
type "C:\ProgramData\Panoptes\Spectra\Output\inventory_latest.json"
```

## Verify Machine ID Persistence

```cmd
# First run - generates ID
Panoptes-Spectra.exe /console
reg query "HKLM\SOFTWARE\Panoptes\Spectra" /v SpectraMachineID

# Second run - reuses same ID
Panoptes-Spectra.exe /console
reg query "HKLM\SOFTWARE\Panoptes\Spectra" /v SpectraMachineID

# Both should show the SAME Machine ID
```

## Registry Location

All configuration is under:
```
HKLM\SOFTWARE\Panoptes\Spectra
```

**Auto-Generated:**
- `SpectraMachineID` - Unique machine identifier (don't modify)

**Optional Configuration:**
- `CollectionIntervalSeconds` - Default: 3600 (Range: 60-86400)
- `OutputDirectory` - Default: `C:\ProgramData\Panoptes\Spectra\Output`
- `ServerUrl` - Default: empty (for future use)
- `EnableDetailedLogging` - Default: 0 (0=off, 1=on)

## Log Files

**Development/Console Mode:**
- `spectra_log.txt` - In current directory

**Service Mode:**
- `C:\ProgramData\Panoptes\Spectra\Logs\spectra_log.txt`

## Check Logs for Machine ID
```cmd
type spectra_log.txt | findstr /i "machine"
```

Expected log entries:
```
[2024-01-15 14:30:45] [+] Generated new Spectra Machine ID: SPECTRA-12345678-...
[2024-01-15 14:30:45] [+] Stored Spectra Machine ID in registry
```

Or if already exists:
```
[2024-01-15 14:30:45] [+] Retrieved existing Spectra Machine ID: SPECTRA-12345678-...
```

## Troubleshooting

### "Access Denied" Creating Registry Key
**Solution:** Run as Administrator
```cmd
# Right-click Command Prompt ? Run as Administrator
Panoptes-Spectra.exe /console
```

### "Failed to generate Machine ID"
**Solution:** Check Windows RPC service is running
```cmd
sc query RpcSs
```

### Machine ID Changes on Every Run
**Solution:** Check registry permissions
```cmd
# Verify registry key exists and is readable
reg query "HKLM\SOFTWARE\Panoptes\Spectra"
```

## Enterprise Deployment

### Silent Installation via PowerShell
```powershell
# Copy executable
Copy-Item "Panoptes-Spectra.exe" "C:\Program Files\Panoptes\Spectra\" -Force

# Configure via registry
New-Item -Path "HKLM:\SOFTWARE\Panoptes\Spectra" -Force
Set-ItemProperty -Path "HKLM:\SOFTWARE\Panoptes\Spectra" `
    -Name "CollectionIntervalSeconds" -Value 3600 -Type DWord

# Install and start service
& "C:\Program Files\Panoptes\Spectra\Panoptes-Spectra.exe" /install
Start-Service -Name "PanoptesSpectra"
```

### Deploy to Multiple Machines
```powershell
$computers = @("SERVER01", "SERVER02", "SERVER03")

foreach ($computer in $computers) {
    # Copy files
    Copy-Item "Panoptes-Spectra.exe" "\\$computer\C$\Program Files\Panoptes\Spectra\" -Force
    
    # Install remotely
    Invoke-Command -ComputerName $computer -ScriptBlock {
        & "C:\Program Files\Panoptes\Spectra\Panoptes-Spectra.exe" /install
        Start-Service -Name "PanoptesSpectra"
    }
}
```

## Next Steps

1. ? **Test** - Verify Machine ID generation works
2. ? **Validate** - Confirm ID persists across runs
3. ? **Deploy** - Test on a few machines
4. ?? **MSI Installer** - Create WiX installer (Phase 2)
5. ?? **GPO Deployment** - Configure Group Policy (Phase 3)

## Documentation

- `MACHINE_ID_IMPLEMENTATION.md` - Complete technical documentation
- `IMPLEMENTATION_SUMMARY.md` - Feature summary and examples
- `README_RUN_AS_SYSTEM.md` - Running as SYSTEM account

## Support Commands

```cmd
# View service status
sc query PanoptesSpectra

# View service configuration
sc qc PanoptesSpectra

# View logs
type spectra_log.txt

# View registry configuration
reg query "HKLM\SOFTWARE\Panoptes\Spectra" /s

# Uninstall service
Panoptes-Spectra.exe /uninstall
```

---

**Status:** ? Ready for Testing
**Build:** ? Successful
**Documentation:** ? Complete
