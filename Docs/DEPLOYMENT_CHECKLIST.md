# Deployment Checklist - Panoptes Spectra Windows Agent

## ? Pre-Deployment Verification

### **Build Verification**
- [ ] Solution builds successfully (x64 and x86)
- [ ] No compiler warnings
- [ ] Control Flow Guard enabled
- [ ] All service files included in build

### **Security Verification**
- [ ] Service name contains NO SPACES (`PanoptesSpectra`)
- [ ] Executable path will be QUOTED during installation
- [ ] Tamper protection code reviewed
- [ ] No hardcoded credentials or API keys

### **Documentation Review**
- [ ] `WINDOWS_SERVICE_GUIDE.md` reviewed and accurate
- [ ] `QUICK_REFERENCE.md` created
- [ ] `IMPLEMENTATION_COMPLETE.md` reviewed

---

## ?? Deployment Steps

### **Step 1: Build Release Binary**

```powershell
# Build Release configuration
msbuild WindowsEnum.sln /p:Configuration=Release /p:Platform=x64

# Verify output
Test-Path ".\bin\x64\Release\Spectra.exe"
```

### **Step 2: Optional - Code Signing (Recommended)**

```powershell
# Sign the executable (if certificate available)
signtool sign /f "certificate.pfx" /p "password" /t "http://timestamp.digicert.com" ".\bin\x64\Release\Spectra.exe"

# Verify signature
signtool verify /pa ".\bin\x64\Release\Spectra.exe"
```

### **Step 3: Package for Distribution**

```powershell
# Create distribution directory
New-Item -Path ".\dist" -ItemType Directory -Force

# Copy executable
Copy-Item ".\bin\x64\Release\Spectra.exe" -Destination ".\dist\"

# Copy documentation
Copy-Item "WINDOWS_SERVICE_GUIDE.md", "QUICK_REFERENCE.md", "LICENSE" -Destination ".\dist\"

# Create README
@"
Panoptes Spectra Windows Agent v1.0.0

INSTALLATION:
1. Run as Administrator: Spectra.exe /install
2. Start service: sc start PanoptesSpectra

DOCUMENTATION:
- WINDOWS_SERVICE_GUIDE.md - Complete documentation
- QUICK_REFERENCE.md - Quick reference card

SUPPORT:
For assistance, contact Panoptes Security support.
"@ | Out-File ".\dist\README.txt"
```

### **Step 4: Create ZIP Archive**

```powershell
# Create timestamped archive
$timestamp = Get-Date -Format "yyyyMMdd"
Compress-Archive -Path ".\dist\*" -DestinationPath ".\Panoptes_Spectra_v1.0.0_x64_$timestamp.zip"
```

---

## ?? Deployment Options

### **Option A: Manual Deployment**

1. Copy `Spectra.exe` to target machine
2. Run as Administrator: `Spectra.exe /install`
3. Start service: `sc start PanoptesSpectra`
4. Verify: `sc query PanoptesSpectra`

### **Option B: Group Policy Deployment**

```powershell
# Create GPO startup script
$script = @"
# Install Panoptes Spectra Service
if (-not (Get-Service -Name "PanoptesSpectra" -ErrorAction SilentlyContinue)) {
    Start-Process -FilePath "C:\Deployment\Spectra.exe" -ArgumentList "/install" -Wait -NoNewWindow
    Start-Service -Name "PanoptesSpectra"
}
"@

# Save to NETLOGON share
$script | Out-File "\\domain.local\NETLOGON\Install-Spectra.ps1"
```

### **Option C: SCCM/Intune Deployment**

**Install Command:**
```cmd
Spectra.exe /install
```

**Uninstall Command:**
```cmd
Spectra.exe /uninstall
```

**Detection Method:**
```powershell
# Registry detection
Test-Path "HKLM:\SYSTEM\CurrentControlSet\Services\PanoptesSpectra"

# Service detection
Get-Service -Name "PanoptesSpectra" -ErrorAction SilentlyContinue
```

---

## ?? Post-Deployment Validation

### **Immediate Checks (First 5 Minutes)**

```powershell
# Check 1: Service installed
Get-Service -Name "PanoptesSpectra" | Select-Object Name, Status, StartType

# Check 2: Service running
sc query PanoptesSpectra

# Check 3: Directories created
Test-Path "C:\ProgramData\Panoptes\Spectra\Output"
Test-Path "C:\ProgramData\Panoptes\Spectra\Logs"

# Check 4: Initial log entry
Get-Content "C:\ProgramData\Panoptes\Spectra\Logs\spectra.log" -Tail 10

# Check 5: Tamper protection applied
sc sdshow PanoptesSpectra | Select-String "WD"  # Should contain DENY ACEs
```

### **Short-Term Validation (First Hour)**

```powershell
# Check 6: Data collection executed
Test-Path "C:\ProgramData\Panoptes\Spectra\Output\inventory_latest.json"

# Check 7: JSON is valid
$json = Get-Content "C:\ProgramData\Panoptes\Spectra\Output\inventory_latest.json" | ConvertFrom-Json
Write-Host "Machine Name: $($json.machineNetBiosName)"
Write-Host "Applications Collected: $(($json.installedAppsByUser | Measure-Object).Count)"

# Check 8: No errors in log
Select-String -Path "C:\ProgramData\Panoptes\Spectra\Logs\spectra.log" -Pattern "\[-\]" -CaseSensitive

# Check 9: Service uptime
$service = Get-WmiObject -Class Win32_Service -Filter "Name='PanoptesSpectra'"
Write-Host "Service State: $($service.State)"
Write-Host "Process ID: $($service.ProcessId)"

# Check 10: Event Log entries
Get-EventLog -LogName Application -Source "PanoptesSpectra" -Newest 5 -ErrorAction SilentlyContinue
```

### **Long-Term Monitoring (First Week)**

- [ ] Service remains running (no unexpected restarts)
- [ ] Data collection occurs on schedule
- [ ] Log files rotate properly
- [ ] No disk space issues
- [ ] No performance degradation

---

## ?? Rollback Procedure

### **If Deployment Fails:**

```powershell
# Stop and remove service
.\Spectra.exe /uninstall

# Verify removal
Get-Service -Name "PanoptesSpectra" -ErrorAction SilentlyContinue
# Should return nothing

# Clean up directories (optional)
Remove-Item "C:\ProgramData\Panoptes" -Recurse -Force -ErrorAction SilentlyContinue

# Verify clean state
Get-Service -Name "Panoptes*"
Test-Path "C:\ProgramData\Panoptes"
```

---

## ?? Success Criteria

### **Deployment is Successful When:**

? Service appears in `services.msc`  
? Service status is "Running"  
? Service account is "Local System"  
? Startup type is "Automatic"  
? First data collection completed within 5 minutes  
? JSON output file created  
? No errors in operational log  
? Tamper protection verified (DENY ACEs present)  
? Non-admin cannot stop service (Access Denied expected)  
? Admin with UAC can stop service  

---

## ?? Support & Escalation

### **Collect Diagnostic Information**

```powershell
# Create diagnostic package
$diagPath = "C:\Temp\Spectra_Diagnostics_$(Get-Date -Format 'yyyyMMdd_HHmmss')"
New-Item -Path $diagPath -ItemType Directory -Force

# Export service configuration
sc qc PanoptesSpectra > "$diagPath\service_config.txt"
sc query PanoptesSpectra > "$diagPath\service_status.txt"
sc sdshow PanoptesSpectra > "$diagPath\service_dacl.txt"

# Copy logs
Copy-Item "C:\ProgramData\Panoptes\Spectra\Logs\*" -Destination $diagPath -ErrorAction SilentlyContinue

# Export last JSON (if exists)
Copy-Item "C:\ProgramData\Panoptes\Spectra\Output\inventory_latest.json" -Destination $diagPath -ErrorAction SilentlyContinue

# System information
Get-ComputerInfo | Out-File "$diagPath\system_info.txt"
Get-WindowsVersion | Out-File "$diagPath\windows_version.txt"

# Event logs
Get-EventLog -LogName Application -Source "PanoptesSpectra" -Newest 50 -ErrorAction SilentlyContinue | Out-File "$diagPath\event_log.txt"

# Create archive
Compress-Archive -Path "$diagPath\*" -DestinationPath "$diagPath.zip"

Write-Host "Diagnostic package created: $diagPath.zip" -ForegroundColor Green
```

---

## ?? Security Considerations

### **Pre-Deployment Security Review**

- [ ] Code signing certificate obtained (if required)
- [ ] Executable will be deployed to `C:\Program Files\Panoptes\Spectra\`
- [ ] No plaintext credentials in configuration
- [ ] Firewall rules configured (if network communication added)
- [ ] Antivirus exclusions documented (if needed)

### **Runtime Security Posture**

- [ ] Service runs as LocalSystem (required)
- [ ] No listening network ports (confirmed via `netstat -ano`)
- [ ] File system permissions verified (SYSTEM:Full, Admins:Read, Users:None)
- [ ] Tamper protection prevents unauthorized modifications
- [ ] Service cannot be deleted without explicit uninstall

---

## ? Final Deployment Checklist

Before marking deployment as complete:

- [ ] Service installed on all target machines
- [ ] Service running on all target machines
- [ ] Data collection verified on sample machines (10%)
- [ ] Log files created and rotating properly
- [ ] No unexpected errors in logs
- [ ] Security validation passed (tamper protection, ACLs)
- [ ] Documentation distributed to IT team
- [ ] Support team trained on troubleshooting
- [ ] Escalation procedure documented
- [ ] Monitoring/alerting configured (if applicable)

---

## ?? Deployment Timeline Example

| Phase | Duration | Activities |
|-------|----------|------------|
| **Pilot** | Week 1 | Deploy to 10 test machines, validate functionality |
| **Validation** | Week 2 | Monitor pilot deployment, collect feedback, fix issues |
| **Staged Rollout** | Weeks 3-4 | Deploy to 25% ? 50% ? 75% ? 100% of target machines |
| **Stabilization** | Week 5 | Monitor, troubleshoot, optimize |
| **Production** | Week 6+ | Ongoing monitoring and maintenance |

---

**Document Version:** 1.0  
**Last Updated:** December 2024  
**Prepared by:** Panoptes Security Engineering Team
