# Panoptes Spectra - Quick Reference Card

## ?? Product Information
- **Name:** Panoptes Spectra Windows Agent
- **Internal Service Name:** `PanoptesSpectra` (NO SPACES - prevents unquoted path attacks)
- **Display Name:** Panoptes Spectra Windows Agent
- **Version:** 1.0.0
- **Account:** LocalSystem (NT AUTHORITY\SYSTEM)

---

## ?? Installation

```powershell
# Install service (requires Administrator)
.\Spectra.exe /install

# Start service
sc start PanoptesSpectra

# Verify status
sc query PanoptesSpectra
```

---

## ??? Uninstallation

```powershell
# Stop and remove service
.\Spectra.exe /uninstall

# Optional: Remove all data
Remove-Item "C:\ProgramData\Panoptes" -Recurse -Force
```

---

## ?? Security Features

| Feature | Status | Details |
|---------|--------|---------|
| **Unquoted Path Protection** | ? Enabled | Service path ALWAYS quoted |
| **Tamper Protection** | ? Enabled | Config changes BLOCKED for ALL users |
| **Delete Protection** | ? Enabled | Service deletion BLOCKED (incl. admins) |
| **Service SID Restriction** | ? Enabled | Limited attack surface |
| **Auto-Restart** | ? Enabled | 2 retries with backoff |

---

## ?? Service Control

```powershell
# Start (requires Admin + UAC)
sc start PanoptesSpectra

# Stop (requires Admin + UAC)
sc stop PanoptesSpectra

# Query status (any user)
sc query PanoptesSpectra

# View detailed config
sc qc PanoptesSpectra
```

---

## ?? File Locations

```
Installation:
  C:\Program Files\Panoptes\Spectra\Spectra.exe

Runtime Data:
  C:\ProgramData\Panoptes\Spectra\Output\      (JSON inventories)
  C:\ProgramData\Panoptes\Spectra\Logs\        (Service logs)
  C:\ProgramData\Panoptes\Spectra\Config\      (Future: encrypted config)
  C:\ProgramData\Panoptes\Spectra\Temp\        (Temporary files)
```

---

## ?? Testing Modes

```powershell
# Test mode (one-time collection)
.\Spectra.exe /test

# Console mode (requires SYSTEM)
psexec -s -i Spectra.exe /console

# Show help
.\Spectra.exe /?
```

---

## ?? Data Collection

- **Interval:** Every 1 hour (configurable)
- **Output Format:** JSON
- **Latest File:** `C:\ProgramData\Panoptes\Spectra\Output\inventory_latest.json`
- **Timestamped Files:** `inventory_YYYYMMDD_HHMMSS.json`

---

## ?? Troubleshooting

### Service Won't Start
```powershell
# Check logs
Get-Content "C:\ProgramData\Panoptes\Spectra\Logs\spectra.log" -Tail 50

# Check Event Viewer
Get-EventLog -LogName Application -Source "PanoptesSpectra" -Newest 10
```

### Access Denied
```powershell
# Verify running as Administrator
[Security.Principal.WindowsIdentity]::GetCurrent().Groups -contains 'S-1-5-32-544'

# Check UAC elevation
[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
```

---

## ?? Security Verification

```powershell
# Verify service DACL
sc sdshow PanoptesSpectra

# Expected: SYSTEM (full), Admins (start/stop), Users (query)
# DENY ACEs for delete and config modification

# Verify service account
sc qc PanoptesSpectra | Select-String "SERVICE_START_NAME"
# Expected: LocalSystem
```

---

## ?? Logs

| Log Type | Location | Max Size | Rotation |
|----------|----------|----------|----------|
| Operational | `C:\ProgramData\...\Logs\spectra.log` | 10 MB | 5 files |
| Windows Event | Event Viewer ? Application | System managed | System managed |

**Log Prefixes:**
- `[+]` Success
- `[-]` Error
- `[!]` Warning

---

## ?? Important Notes

1. **Service Name Contains NO SPACES**
   - Internal name: `PanoptesSpectra`
   - This prevents unquoted service path attacks

2. **Service Account is LocalSystem**
   - Required for SE_BACKUP and SE_RESTORE privileges
   - Administrator is NOT sufficient

3. **Tamper Protection is ALWAYS ENABLED**
   - Even administrators cannot modify service config
   - To uninstall, use `/uninstall` (removes protection first)

4. **UAC Elevation Required**
   - Starting/stopping requires Administrator WITH UAC
   - Non-elevated administrators cannot control the service

---

## ?? Support Commands

```powershell
# Export service configuration
sc qc PanoptesSpectra > service_config.txt

# Export recent logs
Get-Content "C:\ProgramData\Panoptes\Spectra\Logs\spectra.log" -Tail 100 > diagnostic.log

# Check required privileges
sc qprivs PanoptesSpectra
```

---

## ? Quick Health Check

```powershell
# One-liner health check
$service = Get-Service -Name "PanoptesSpectra" -ErrorAction SilentlyContinue
if ($service) {
    Write-Host "Service Status: $($service.Status)" -ForegroundColor $(if ($service.Status -eq 'Running') {'Green'} else {'Red'})
    Write-Host "Startup Type: $($service.StartType)"
    Write-Host "Latest Output: $(Get-ChildItem 'C:\ProgramData\Panoptes\Spectra\Output\inventory_latest.json' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty LastWriteTime)"
} else {
    Write-Host "Service NOT installed" -ForegroundColor Red
}
```

---

**For detailed documentation, see:** `WINDOWS_SERVICE_GUIDE.md`
