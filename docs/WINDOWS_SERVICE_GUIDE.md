# Panoptes Spectra Windows Agent — Service Implementation Guide

## Overview

**Panoptes Spectra Windows Agent** is an enterprise-grade Windows service that collects comprehensive system inventory data for vulnerability management. The service runs as `LocalSystem` and uses native Windows APIs to enumerate installed applications, user profiles, and system configuration.

## Security Features

### Unquoted Service Path Protection
- Service executable path is **always quoted** during installation
- Protects against privilege escalation via unquoted service path attacks
- Service name contains no spaces (`PanoptesSpectra`)

### Tamper Protection
The service implements multi-layer tamper protection:

| Layer | Protection Mechanism | Details |
|-------|----------------------|---------|
| **Service DACL** | Restrictive ACL | Only SYSTEM has full control |
| **Administrator Access** | Limited permissions | Admins can start/stop with UAC, but cannot modify config |
| **Delete Protection** | DENY ACE | Service deletion blocked for all users (including admins) |
| **Config Protection** | DENY ACE | Service configuration locked against modification |

#### Access Control Matrix

| User/Group | Query Status | Start/Stop | Modify Config | Delete Service |
|------------|--------------|------------|---------------|----------------|
| **SYSTEM** | Yes | Yes | Yes | Yes |
| **Administrators (with UAC)** | Yes | Yes | DENIED | DENIED |
| **Administrators (no UAC)** | Yes | No | DENIED | DENIED |
| **Users** | Yes | No | No | No |

### Service Hardening
- Service SID type (`SERVICE_SID_TYPE_UNRESTRICTED`) — adds per-service SID `NT SERVICE\PanoptesSpectra` to the process token for fine-grained ACLs on directories and registry keys
- Declared required privileges (`SE_BACKUP_NAME`, `SE_RESTORE_NAME`, `SE_SYSTEM_PROFILE_NAME`) — all other privileges are stripped by the SCM
- Automatic restart on failure (2 retries with backoff: 1 min then 2 min then none)
- 24-hour failure count reset

---

## System Requirements

- **Operating System:** Windows 10 / Windows Server 2016 or later
- **Architecture:** x64 (primary) or x86 (32-bit on 32-bit OS only)
- **Privileges:** Administrator for installation; LocalSystem for runtime
- **Disk Space:** ~50 MB for application plus variable space for output data

---

## Installation

### Interactive Installation

```powershell
# Run as Administrator
.\Panoptes-Spectra.exe /install
```

The installer will:

1. Copy the executable to `C:\Program Files\Panoptes\Spectra\Panoptes-Spectra.exe`.
2. Register the `PanoptesSpectra` service with `SERVICE_AUTO_START`.
3. Create secure data directories under `C:\ProgramData\Panoptes\Spectra\`.
4. Write default registry configuration to `HKLM\SOFTWARE\Panoptes\Spectra`.
5. Apply service hardening (SID, declared privileges, failure actions).
6. Apply directory and registry key ACLs for the per-service SID.
7. Apply tamper protection DACL.
8. Start the service automatically.

If the service already exists, `/install` automatically performs an in-place upgrade (equivalent to `/upgrade`).

### Silent Installation (for deployment tools)

```powershell
Start-Process -FilePath "Panoptes-Spectra.exe" -ArgumentList "/install" -Verb RunAs -Wait
```

---

## Service Control

### Start the Service

```powershell
# Using sc.exe (requires Administrator + UAC)
sc start PanoptesSpectra

# Using PowerShell
Start-Service -Name "PanoptesSpectra"
```

### Stop the Service

```powershell
# Requires Administrator with UAC elevation
sc stop PanoptesSpectra
```

### Query Service Status

```powershell
sc query PanoptesSpectra
```

---

## Upgrade

```powershell
# In-place upgrade (preserves Machine ID, config, logs, ACLs)
.\Panoptes-Spectra.exe /upgrade
```

The upgrade sequence:
1. Removes tamper protection (needed for reconfiguration).
2. Stops the running service (120-second timeout).
3. Waits for the service process to exit.
4. Copies the new binary over the installed location.
5. Re-applies service hardening, directory ACLs, and registry ACLs.
6. Re-applies tamper protection.
7. Starts the upgraded service.

---

## Uninstallation

```powershell
# Must be run from the installed location
# (C:\Program Files\Panoptes\Spectra\Panoptes-Spectra.exe /uninstall)
.\Panoptes-Spectra.exe /uninstall
```

The uninstaller:
1. Verifies it is running from the registered service binary path (prevents rogue uninstallation).
2. Removes tamper protection, stops the service, deletes it from SCM.
3. Removes all artifacts: `C:\Program Files\Panoptes\`, `C:\ProgramData\Panoptes\`, `HKLM\SOFTWARE\Panoptes\`.
4. Files locked during uninstall are scheduled for deletion on reboot.

---

## Directory Structure

```
C:\Program Files\Panoptes\Spectra\          (Installation directory)
??? Panoptes-Spectra.exe                    (Service executable — quoted path)

C:\ProgramData\Panoptes\Spectra\            (Runtime data)
??? Config\                                 (Future: encrypted configuration files)
??? Logs\
?   ??? spectra_log.txt                     (Operational log)
??? Output\
?   ??? inventory.json                      (System inventory data)
?   ??? processes.json                      (Process tracking data)
??? Temp\                                   (VSS snapshots, temporary hive files — auto-cleaned)
```

---

## Configuration

Runtime configuration is stored in the registry at `HKLM\SOFTWARE\Panoptes\Spectra` and can be modified without recompilation (e.g., via GPO or MSI custom actions). Configuration is reloaded from the registry at the start of each collection cycle — changes take effect without restarting the service.

| Value Name | Type | Default | Range | Description |
|---|---|---|---|---|
| `CollectionIntervalSeconds` | `REG_DWORD` | `86400` (24 h) | 3600–604800 (1 h – 7 d) | Interval between data collections |
| `OutputDirectory` | `REG_SZ` | `C:\ProgramData\Panoptes\Spectra\Output` | — | Directory for JSON output files |
| `ServerUrl` | `REG_SZ` | *(empty)* | — | Future: endpoint for data upload |
| `EnableDetailedLogging` | `REG_DWORD` | `0` | 0 or 1 | Enable verbose logging |
| `EnableProcessTracking` | `REG_DWORD` | `1` | 0 or 1 | Enable ETW-based process tracking |

### Examples

```powershell
# Set collection interval to 2 hours
reg add "HKLM\SOFTWARE\Panoptes\Spectra" /v CollectionIntervalSeconds /t REG_DWORD /d 7200 /f

# Change output directory
reg add "HKLM\SOFTWARE\Panoptes\Spectra" /v OutputDirectory /t REG_SZ /d "D:\Spectra\Output" /f
```

---

## Testing & Validation

### Test Mode (One-Time Collection)

```powershell
.\Panoptes-Spectra.exe /test
```

Runs a single data collection cycle without installing the service.

### Console Mode (Manual Execution)

```powershell
# Run as SYSTEM using PsExec for full functionality
psexec -s -i Panoptes-Spectra.exe /console
```

### Verify Service Security

```powershell
# Check service DACL
sc sdshow PanoptesSpectra

# Expected: SYSTEM full control, Admins start/stop/query, Users query only,
# DENY delete/change-config for Everyone
```

---

## Output Data Format

The service generates two JSON files per collection cycle in the configured output directory:

| File | Contents |
|---|---|
| `inventory.json` | Machine identity, network info, per-user application inventory (Win32/MSI/AppX), OS version, installed updates with MSRC metadata, Windows services |
| `processes.json` | Non-service processes observed via real-time ETW monitoring + point-in-time snapshot, with image path, command line, user, parent process, PE file version, and tracker diagnostics |

### Sample Inventory JSON

```json
{
  "spectraMachineId": "SPECTRA-A1B2C3D4-E5F6-7890-ABCD-EF1234567890",
  "collectionTimestamp": "2025-01-15T14:30:45",
  "agentVersion": "1.0.0",
  "machineNetBiosName": "DESKTOP-ABC123",
  "machineDnsName": "DESKTOP-ABC123.domain.local",
  "ipAddresses": ["192.168.1.100", "fe80::1234:5678"],
  "installedAppsByUser": [
    {
      "user": "SYSTEM",
      "userSID": "S-1-5-18",
      "applications": [...],
      "msiProducts": [...],
      "modernAppPackages": [...]
    }
  ],
  "windowsServices": [...],
  "installedUpdates": [...]
}
```

---

## Security Best Practices

### Privilege Least Principle
- Service runs as `LocalSystem` (required for SE_BACKUP/SE_RESTORE and kernel ETW sessions)
- Privileges are enabled **only when needed** and disabled immediately after use

### File System Security
- Output files written to `C:\ProgramData` (protected by directory ACLs)
- Temporary files securely deleted after use

### Attack Surface Minimisation
- No network listening ports
- No remote administration interface
- No user input processing (defence in depth)

---

## Troubleshooting

### Service Won't Start

1. Check logs: `C:\ProgramData\Panoptes\Spectra\Logs\spectra_log.txt`
2. Check Event Viewer ? Windows Logs ? Application
3. Verify directory permissions

### Access Denied During Installation

Run installation as Administrator (UAC elevation required).

### Service Won't Stop

Expected if not running as Administrator with UAC elevation.

### Missing Data for Specific Users

Service requires `SYSTEM` account to access all user hives. Verify the service is running as LocalSystem:

```powershell
sc qc PanoptesSpectra
# Look for: SERVICE_START_NAME : LocalSystem
```

---

## Logging

### Log Locations

| Log Type | Location |
|----------|----------|
| **Operational Log** | `C:\ProgramData\Panoptes\Spectra\Logs\spectra_log.txt` |
| **Windows Event Log** | Event Viewer ? Application |
| **Diagnostic Trace** | `C:\ProgramData\Panoptes\Spectra\Logs\trace.txt` |

### Log Prefixes

- `[+]` — Successful operations
- `[-]` — Errors or failures
- `[!]` — Warnings or important notices

---

## Compliance & Auditing

### Data Collected

- Machine name (NetBIOS, DNS/FQDN)
- IP addresses (IPv4, IPv6)
- Installed applications (Win32, MSI, AppX/MSIX)
- User profiles (username, SID)
- Installation dates and versions
- Windows services (name, state, binary path, account)
- Installed updates (KB articles, MSRC severity)
- Running processes (image path, command line, user)

### Data NOT Collected

- Passwords or credentials
- User files or documents
- Browser history
- Personal identifiable information (PII)
- Network traffic

---

## Additional Resources

- [Windows Service Control Manager (SCM) Documentation](https://learn.microsoft.com/en-us/windows/win32/services/services)
- [Service Security and Access Rights](https://learn.microsoft.com/en-us/windows/win32/services/service-security-and-access-rights)
- [Unquoted Service Path Vulnerability (CWE-428)](https://cwe.mitre.org/data/definitions/428.html)

---

## License

Copyright © 2025 Panoptes Project. Licensed under GPLv3.
