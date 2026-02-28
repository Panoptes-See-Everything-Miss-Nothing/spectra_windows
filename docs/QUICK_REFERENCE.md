# Panoptes Spectra — Quick Reference Card

## Product Information

| Property | Value |
|----------|-------|
| **Product Name** | Panoptes Spectra Windows Agent |
| **Internal Service Name** | `PanoptesSpectra` (no spaces — prevents unquoted path attacks) |
| **Display Name** | Panoptes Spectra Windows Agent |
| **Version** | 1.0.0 |
| **Service Account** | LocalSystem (NT AUTHORITY\SYSTEM) |

---

## Installation

```powershell
# Install service (requires Administrator)
.\Panoptes-Spectra.exe /install

# Start service
sc start PanoptesSpectra

# Verify status
sc query PanoptesSpectra
```

---

## Uninstallation

```powershell
# Must be run from the installed location
"C:\Program Files\Panoptes\Spectra\Panoptes-Spectra.exe" /uninstall

# Optional: remove residual data (if any files were locked)
Remove-Item "C:\ProgramData\Panoptes" -Recurse -Force
```

---

## Security Features

| Feature | Details |
|---------|---------|
| **Unquoted Path Protection** | Service path always quoted at install time |
| **Tamper Protection** | Config changes and deletion blocked for all users (DENY ACE) |
| **Service SID** | `SERVICE_SID_TYPE_UNRESTRICTED` — per-service SID for fine-grained ACLs |
| **Declared Privileges** | SE_BACKUP_NAME, SE_RESTORE_NAME, SE_SYSTEM_PROFILE_NAME (all others stripped) |
| **Auto-Restart** | 2 retries with backoff (1 min → 2 min → none) |

---

## Service Control

```powershell
# Start (requires Administrator + UAC)
sc start PanoptesSpectra

# Stop (requires Administrator + UAC)
sc stop PanoptesSpectra

# Query status (any user)
sc query PanoptesSpectra

# View detailed config
sc qc PanoptesSpectra
```

---

## File Locations

```
Installation:
  C:\Program Files\Panoptes\Spectra\Panoptes-Spectra.exe

Runtime Data:
  C:\ProgramData\Panoptes\Spectra\Output\      (inventory.json, processes.json)
  C:\ProgramData\Panoptes\Spectra\Logs\        (spectra_log.txt)
  C:\ProgramData\Panoptes\Spectra\Config\      (Future: encrypted config)
  C:\ProgramData\Panoptes\Spectra\Temp\        (Temporary files — auto-cleaned)
```

---

## Testing Modes

```powershell
# Test mode (one-time collection, no service install)
.\Panoptes-Spectra.exe /test

# Console mode (requires SYSTEM for full functionality)
psexec -s -i Panoptes-Spectra.exe /console

# Show help
.\Panoptes-Spectra.exe /?
```

---

## Data Collection

| Property | Value |
|----------|-------|
| **Default Interval** | 24 hours (86400 seconds) |
| **Valid Range** | 1 hour – 7 days (3600–604800 seconds) |
| **Output Format** | JSON |
| **Inventory File** | `inventory.json` |
| **Process File** | `processes.json` |

---

## Registry Configuration

All configuration is under `HKLM\SOFTWARE\Panoptes\Spectra`:

| Value | Type | Default | Description |
|-------|------|---------|-------------|
| `SpectraMachineID` | `REG_SZ` | *(auto-generated)* | Unique machine identifier (do not modify) |
| `CollectionIntervalSeconds` | `REG_DWORD` | `86400` | Collection interval (3600–604800) |
| `OutputDirectory` | `REG_SZ` | `C:\ProgramData\Panoptes\Spectra\Output` | Output directory |
| `ServerUrl` | `REG_SZ` | *(empty)* | Future: data upload endpoint |
| `EnableDetailedLogging` | `REG_DWORD` | `0` | Verbose logging (0=off, 1=on) |
| `EnableProcessTracking` | `REG_DWORD` | `1` | ETW process tracking (0=off, 1=on) |

---

## Troubleshooting

### Service Won't Start

```powershell
# Check operational log
Get-Content "C:\ProgramData\Panoptes\Spectra\Logs\spectra_log.txt" -Tail 50

# Check Event Viewer
Get-EventLog -LogName Application -Source "PanoptesSpectra" -Newest 10
```

### Verify Administrator + UAC Elevation

```powershell
([Security.Principal.WindowsPrincipal]::new(
    [Security.Principal.WindowsIdentity]::GetCurrent()
)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
```

---

## Security Verification

```powershell
# Verify service DACL
sc sdshow PanoptesSpectra

# Verify service account
sc qc PanoptesSpectra | Select-String "SERVICE_START_NAME"
# Expected: LocalSystem

# Verify declared privileges
sc qprivs PanoptesSpectra
```

---

## Log Prefixes

| Prefix | Meaning |
|--------|---------|
| `[+]` | Success |
| `[-]` | Error |
| `[!]` | Warning |

---

## Important Notes

1. **Service name contains no spaces** — internal name is `PanoptesSpectra` to prevent unquoted service path attacks.
2. **Service account is LocalSystem** — required for SE_BACKUP, SE_RESTORE, and kernel ETW sessions. Administrator is NOT sufficient.
3. **Tamper protection is always enabled** — even administrators cannot modify or delete the service configuration. To uninstall, use `/uninstall` (removes protection first).
4. **UAC elevation required** — starting and stopping requires Administrator with UAC. Non-elevated administrators cannot control the service.
5. **Uninstall path enforcement** — `/uninstall` must be run from the installed location (`C:\Program Files\Panoptes\Spectra\`).

---

## Diagnostic Commands

```powershell
# Export service configuration
sc qc PanoptesSpectra > service_config.txt

# Export recent logs
Get-Content "C:\ProgramData\Panoptes\Spectra\Logs\spectra_log.txt" -Tail 100 > diagnostic.log

# Check required privileges
sc qprivs PanoptesSpectra

# Quick health check
$svc = Get-Service -Name "PanoptesSpectra" -ErrorAction SilentlyContinue
if ($svc) {
    Write-Host "Status: $($svc.Status)"
    Write-Host "Start Type: $($svc.StartType)"
} else {
    Write-Host "Service not installed"
}
