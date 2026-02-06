# Panoptes Spectra Windows Agent - Service Implementation Guide

## Overview

**Panoptes Spectra Windows Agent** is an enterprise-grade Windows service that collects comprehensive system inventory data for vulnerability management. The service runs as `LocalSystem` and uses native Windows APIs to enumerate installed applications, user profiles, and system configuration.

## ?? Security Features

### **Critical: Unquoted Service Path Protection**
- ? Service executable path is **ALWAYS quoted** during installation
- ? Protects against privilege escalation via unquoted service path attacks
- ? Service name contains NO spaces (`PanoptesSpectra` instead of `Panoptes Spectra`)

### **Tamper Protection**
The service implements multi-layer tamper protection:

| Layer | Protection Mechanism | Details |
|-------|----------------------|---------|
| **Service DACL** | Restrictive ACL | Only SYSTEM has full control |
| **Administrator Access** | Limited permissions | Admins can start/stop with UAC, but CANNOT modify config |
| **Delete Protection** | DENY ACE | Service deletion blocked for ALL users (including admins) |
| **Config Protection** | DENY ACE | Service configuration locked against modification |

#### **Access Control Matrix**

| User/Group | Query Status | Start/Stop | Modify Config | Delete Service |
|------------|--------------|------------|---------------|----------------|
| **SYSTEM** | ? Yes | ? Yes | ? Yes | ? Yes |
| **Administrators (with UAC)** | ? Yes | ? Yes | ? DENIED | ? DENIED |
| **Administrators (no UAC)** | ? Yes | ? No | ? DENIED | ? DENIED |
| **Users** | ? Yes | ? No | ? No | ? No |

### **Service Hardening**
- Service SID restriction (`SERVICE_SID_TYPE_RESTRICTED`)
- Declared required privileges (`SE_BACKUP_NAME`, `SE_RESTORE_NAME`)
- Automatic restart on failure (2 retries with backoff)
- 24-hour failure count reset

---

## ?? System Requirements

- **Operating System:** Windows 10 / Windows Server 2016 or later
- **Architecture:** 64-bit (x64) or 32-bit (x86)
- **Privileges:** Administrator for installation; LocalSystem for runtime
- **Disk Space:** 50 MB for application + variable for data collection

---

## ?? Installation

### **Option 1: Interactive Installation**

```powershell
# Run as Administrator
.\Spectra.exe /install
```

Expected Output:
```
[+] ==========================================================
[+] Installing Panoptes Spectra Windows Agent
[+] ==========================================================
[+] Created directory: C:\ProgramData\Panoptes\Spectra\Output
[+] Created directory: C:\ProgramData\Panoptes\Spectra\Logs
[+] Service executable: "C:\Program Files\Panoptes\Spectra\Spectra.exe"
[+] Service created successfully
[+] Service SID restriction applied
[+] Required privileges declared: SE_BACKUP_NAME, SE_RESTORE_NAME
[+] Failure recovery configured (auto-restart)
[+] Service tamper protection applied successfully
[!] Only Administrators with UAC can start/stop the service
[+] ==========================================================
[+] Service installed successfully!
[+] Service Name: PanoptesSpectra
[+] Display Name: Panoptes Spectra Windows Agent
[+] ==========================================================
```

### **Option 2: Silent Installation (for deployment tools)**

```powershell
Start-Process -FilePath "Spectra.exe" -ArgumentList "/install" -Verb RunAs -Wait
```

---

## ?? Service Control

### **Start the Service**

```powershell
# Method 1: Using sc.exe
sc start PanoptesSpectra

# Method 2: Using net command
net start "Panoptes Spectra Windows Agent"

# Method 3: Using PowerShell
Start-Service -Name "PanoptesSpectra"
```

### **Stop the Service**

```powershell
# Requires Administrator with UAC elevation!
sc stop PanoptesSpectra
```

### **Query Service Status**

```powershell
sc query PanoptesSpectra
```

Expected Output:
```
SERVICE_NAME: PanoptesSpectra
TYPE               : 10  WIN32_OWN_PROCESS
STATE              : 4  RUNNING
WIN32_EXIT_CODE    : 0  (0x0)
SERVICE_EXIT_CODE  : 0  (0x0)
CHECKPOINT         : 0x0
WAIT_HINT          : 0x0
```

---

## ??? Uninstallation

```powershell
# Run as Administrator
.\Spectra.exe /uninstall
```

**Note:** Runtime data in `C:\ProgramData\Panoptes\Spectra\` is NOT automatically deleted. To remove all data:

```powershell
Remove-Item -Path "C:\ProgramData\Panoptes" -Recurse -Force
```

---

## ?? Directory Structure

```
C:\Program Files\Panoptes\Spectra\          (Installation directory)
??? Spectra.exe                              (Service executable - QUOTED path)
??? (runtime dependencies)

C:\ProgramData\Panoptes\Spectra\             (Runtime data - hidden from users)
??? Config\
?   ??? (future: encrypted configuration files)
??? Logs\
?   ??? spectra.log                          (Operational logs)
?   ??? spectra.log.1                        (Rotated logs)
?   ??? spectra.log.2
??? Output\
?   ??? inventory_20240101_120000.json       (Timestamped inventories)
?   ??? inventory_latest.json                (Latest collection)
??? Temp\
    ??? (VSS snapshots, temporary hive files - auto-cleaned)
```

---

## ?? Configuration

### **Data Collection Interval**

By default, the service collects data every **1 hour** (3600 seconds). To change this, modify `ServiceConfig.h`:

```cpp
constexpr DWORD COLLECTION_INTERVAL_MS = 3600000; // 1 hour in milliseconds
```

Rebuild and reinstall the service after modification.

---

## ?? Testing & Validation

### **Test Mode (One-Time Collection)**

```powershell
.\Spectra.exe /test
```

This runs a single data collection cycle without installing the service.

### **Console Mode (Manual Execution)**

```powershell
# Run as SYSTEM using PsExec
psexec -s -i Spectra.exe /console
```

**Important:** Console mode requires `SYSTEM` privileges for full functionality.

### **Verify Service Security**

```powershell
# Check service tamper protection
sc sdshow PanoptesSpectra
```

Expected SDDL (simplified):
```
D:(A;;CCLCSWRPWPDTLOCRRC;;;SY)    # SYSTEM: Full control
(A;;CCDCLCSWRPLOCRRC;;;BA)        # Admins: Start/Stop/Query
(A;;CCLCSWLOCRRC;;;BU)            # Users: Query only
(D;;WPDTSD;;;WD)                  # DENY: Delete/ChangeConfig for Everyone
```

---

## ?? Output Data Format

The service generates JSON inventory files:

```json
{
  "machineNetBiosName": "DESKTOP-ABC123",
  "machineDnsName": "DESKTOP-ABC123.domain.local",
  "ipAddresses": ["192.168.1.100", "fe80::1234:5678"],
  "installedAppsByUser": [
    {
      "user": "SYSTEM",
      "userSID": "S-1-5-18",
      "applications": [
        {
          "displayName": "7-Zip",
          "displayVersion": "23.01",
          "publisher": "Igor Pavlov",
          "installLocation": "C:\\Program Files\\7-Zip\\"
        }
      ],
      "msiProducts": [...],
      "modernAppPackages": [...]
    }
  ]
}
```

---

## ??? Security Best Practices

### **1. Privilege Least Principle**
- Service runs as `LocalSystem` (required for SE_BACKUP/SE_RESTORE)
- Privileges are enabled **only when needed**
- Privileges are disabled immediately after use

### **2. File System Security**
- Output files written to `C:\ProgramData` (hidden from non-admins)
- Logs protected by NTFS ACLs (SYSTEM:Full, Admins:Read, Users:None)
- Temporary files securely deleted after use

### **3. Attack Surface Minimization**
- No network listening ports
- No remote administration interface
- No user input processing (defense in depth)

### **4. Code Integrity**
- **Future:** Code signing with Authenticode certificate
- **Future:** Windows Defender Application Control (WDAC) integration

---

## ?? Troubleshooting

### **Service Won't Start**

**Symptom:** Service starts and immediately stops

**Solution:**
1. Check Event Viewer ? Windows Logs ? Application
2. Review logs: `C:\ProgramData\Panoptes\Spectra\Logs\spectra.log`
3. Verify directory permissions

### **Access Denied During Installation**

**Symptom:** "Failed to open Service Control Manager"

**Solution:** Run installation as Administrator (UAC elevation required)

### **Service Won't Stop**

**Symptom:** `sc stop` fails with Access Denied

**Solution:** This is expected if not running as Administrator with UAC elevation

### **Missing Data for Specific Users**

**Symptom:** Some user profiles not enumerated

**Cause:** Service requires `SYSTEM` account to access all user hives

**Verification:**
```powershell
# Verify service is running as LocalSystem
sc qc PanoptesSpectra
```

Look for: `SERVICE_START_NAME  : LocalSystem`

---

## ?? Logging

### **Log Locations**

| Log Type | Location | Purpose |
|----------|----------|---------|
| **Operational Log** | `C:\ProgramData\Panoptes\Spectra\Logs\spectra.log` | Service operations, data collection |
| **Windows Event Log** | Event Viewer ? Application | Service start/stop, errors |

### **Log Rotation**

- Logs automatically rotate at 10 MB
- Up to 5 historical log files retained
- Oldest logs automatically deleted

### **Log Prefixes**

- `[+]` - Successful operations
- `[-]` - Errors or failures
- `[!]` - Warnings or important notices

---

## ?? Compliance & Auditing

### **Data Collected**

- Machine name (NetBIOS, DNS/FQDN)
- IP addresses (IPv4, IPv6)
- Installed applications (Win32, MSI, AppX/MSIX)
- User profiles (username, SID)
- Installation dates and versions

### **Data NOT Collected**

- ? Passwords or credentials
- ? User files or documents
- ? Browser history
- ? Personal identifiable information (PII)
- ? Network traffic

### **Audit Trail**

- All operations logged to `spectra.log`
- Service start/stop events in Windows Event Log
- No sensitive data exposed in logs

---

## ?? Additional Resources

### **Related Documentation**
- [Windows Service Control Manager (SCM) Documentation](https://learn.microsoft.com/en-us/windows/win32/services/services)
- [Service Security and Access Rights](https://learn.microsoft.com/en-us/windows/win32/services/service-security-and-access-rights)
- [Unquoted Service Path Vulnerability (CWE-428)](https://cwe.mitre.org/data/definitions/428.html)

### **Support**
For issues or questions, contact Panoptes Security support.

---

## ?? License

Copyright © 2024 Panoptes Security. All rights reserved.
