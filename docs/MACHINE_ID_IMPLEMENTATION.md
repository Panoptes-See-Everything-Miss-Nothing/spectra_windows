# Spectra Machine ID Implementation

## Overview

Each Panoptes Spectra agent instance is assigned a cryptographically unique **Machine ID** that persists across reboots and reinstalls. This identifier is used to correlate inventory data from the same machine over time.

**Format:** `SPECTRA-{GUID}` (e.g., `SPECTRA-A1B2C3D4-E5F6-7890-ABCD-EF1234567890`)

**Storage:** Registry at `HKLM\SOFTWARE\Panoptes\Spectra\SpectraMachineID`

**Total length:** 44 characters (`SPECTRA-` prefix + 36-character GUID)

## Source Files

| File | Purpose |
|------|---------|
| `src/Service/MachineId.h` | Machine ID interface and constants |
| `src/Service/MachineId.cpp` | Machine ID generation, storage, and validation |
| `src/Service/ServiceConfig.h` | Registry key path constants |
| `src/Service/ServiceConfig.cpp` | Registry configuration reader |

## Generation Algorithm

1. Check registry for existing Machine ID at `HKLM\SOFTWARE\Panoptes\Spectra\SpectraMachineID`.
2. If found, validate the format and return it.
3. If not found, generate a new UUID using Windows `UuidCreate()`.
4. Convert to uppercase string, prefix with `SPECTRA-`.
5. Store in registry for persistence.

### Why `UuidCreate()`

- Uses the Windows RPC runtime UUID generator
- Combines hardware identity, high-resolution timestamp, and cryptographic randomness
- Globally unique across millions of machines
- No external dependencies

## Registry Configuration

### Auto-Generated (Do Not Modify)

| Value | Type | Description |
|-------|------|-------------|
| `SpectraMachineID` | `REG_SZ` | Unique machine identifier |

### User-Configurable (Optional)

| Value | Type | Default | Range | Description |
|-------|------|---------|-------|-------------|
| `CollectionIntervalSeconds` | `REG_DWORD` | `86400` (24 h) | 3600–604800 | Collection interval |
| `OutputDirectory` | `REG_SZ` | `C:\ProgramData\Panoptes\Spectra\Output` | — | JSON output directory |
| `ServerUrl` | `REG_SZ` | *(empty)* | — | Future: data upload endpoint |
| `EnableDetailedLogging` | `REG_DWORD` | `0` | 0 or 1 | Verbose logging |
| `EnableProcessTracking` | `REG_DWORD` | `1` | 0 or 1 | ETW process tracking |

## JSON Output

The Machine ID, collection timestamp, and agent version are included in every inventory JSON:

```json
{
  "spectraMachineId": "SPECTRA-A1B2C3D4-E5F6-7890-ABCD-EF1234567890",
  "collectionTimestamp": "2025-01-15T14:30:45",
  "agentVersion": "1.0.0",
  "machineNetBiosName": "DESKTOP-ABC123",
  ...
}
```

## Testing

### Verify Machine ID Generation

```cmd
REM Run in console mode (as SYSTEM for full functionality)
psexec -s -i Panoptes-Spectra.exe /console

REM Check registry
reg query "HKLM\SOFTWARE\Panoptes\Spectra" /v SpectraMachineID
```

### Verify Persistence

```cmd
REM Run twice — both runs should show the same Machine ID
psexec -s -i Panoptes-Spectra.exe /console
reg query "HKLM\SOFTWARE\Panoptes\Spectra" /v SpectraMachineID

psexec -s -i Panoptes-Spectra.exe /console
reg query "HKLM\SOFTWARE\Panoptes\Spectra" /v SpectraMachineID
```

### Verify in JSON Output

```cmd
type inventory.json | findstr "spectraMachineId"
```

## Security Considerations

- Stored in `HKLM` (requires Administrator or SYSTEM to modify)
- UUID-based generation ensures global uniqueness
- Persists across reinstalls (tied to the machine, not the installation)
- The service installer applies an ACL to the registry key granting write access to the per-service SID (`NT SERVICE\PanoptesSpectra`)

## Enterprise Deployment

### GPO Configuration

1. **Computer Configuration** → **Preferences** → **Windows Settings** → **Registry**
2. Hive: `HKEY_LOCAL_MACHINE`, Key Path: `SOFTWARE\Panoptes\Spectra`
3. Add values:
   - `CollectionIntervalSeconds` (DWORD): `86400`
   - `ServerUrl` (String): `https://panoptes.corp.local`

### PowerShell Remote Configuration

```powershell
$computers = Get-ADComputer -Filter {OperatingSystem -like "*Windows*"} |
             Select-Object -ExpandProperty Name

foreach ($computer in $computers) {
    Invoke-Command -ComputerName $computer -ScriptBlock {
        $regPath = "HKLM:\SOFTWARE\Panoptes\Spectra"
        New-Item -Path $regPath -Force | Out-Null
        Set-ItemProperty -Path $regPath -Name "CollectionIntervalSeconds" `
                         -Value 86400 -Type DWord
        Set-ItemProperty -Path $regPath -Name "ServerUrl" `
                         -Value "https://panoptes.corp.local" -Type String
        Restart-Service -Name "PanoptesSpectra" -ErrorAction SilentlyContinue
    }
}
```

## Troubleshooting

### "Machine ID generation failed"

**Cause:** Insufficient permissions to create or write to the registry key.  
**Solution:** Ensure the service is running as LocalSystem, or run in console mode as SYSTEM via `psexec -s`.

### Machine ID regenerated after upgrade

This should not happen. The upgrade process (`/upgrade` or `/install` on an existing service) preserves all registry state including the Machine ID. If the ID changes, check whether `/uninstall` was run first (which deletes `HKLM\SOFTWARE\Panoptes`).
