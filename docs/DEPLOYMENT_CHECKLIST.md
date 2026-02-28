# Deployment Checklist — Panoptes Spectra Windows Agent

## Pre-Deployment Verification

### Build Verification
- [ ] Solution (`Panoptes.sln`) builds successfully for both x64 and x86
- [ ] No compiler warnings
- [ ] Control Flow Guard enabled
- [ ] All source files included in `Panoptes-Spectra-Windows.vcxproj`

### Security Verification
- [ ] Service name contains no spaces (`PanoptesSpectra`)
- [ ] Executable path is always quoted during installation
- [ ] Tamper protection DACL code reviewed
- [ ] No hardcoded credentials or API keys

### Documentation Review
- [ ] `WINDOWS_SERVICE_GUIDE.md` reviewed
- [ ] `QUICK_REFERENCE.md` reviewed

---

## Deployment Steps

### Step 1: Build Release Binary

```powershell
msbuild Panoptes.sln /p:Configuration=Release /p:Platform=x64

# Verify output (binary name includes version and arch)
Test-Path ".\bin\x64\Release\Panoptes-Spectra-v1.0.0-x64.exe"
```

### Step 2: Code Signing (Recommended)

```powershell
# Sign the executable
signtool sign /f "certificate.pfx" /p "password" /t "http://timestamp.digicert.com" `"`
    ".\bin\x64\Release\Panoptes-Spectra-v1.0.0-x64.exe"

# Verify signature
signtool verify /pa ".\bin\x64\Release\Panoptes-Spectra-v1.0.0-x64.exe"
```

### Step 3: Package for Distribution

```powershell
New-Item -Path ".\dist" -ItemType Directory -Force

# Copy executable (rename to Panoptes-Spectra.exe for deployment)
Copy-Item ".\bin\x64\Release\Panoptes-Spectra-v1.0.0-x64.exe" `"`
    -Destination ".\dist\Panoptes-Spectra.exe"

# Copy documentation
Copy-Item "docs\WINDOWS_SERVICE_GUIDE.md", "docs\QUICK_REFERENCE.md", "LICENSE" `"`
    -Destination ".\dist\"
```

### Step 4: Create Archive

```powershell
$timestamp = Get-Date -Format "yyyyMMdd"
Compress-Archive -Path ".\dist\*" `"`
    -DestinationPath ".\Panoptes-Spectra-v1.0.0-x64-$timestamp.zip"
```

---

## Deployment Options

### Option A: Manual Deployment

1. Copy `Panoptes-Spectra.exe` to target machine
2. Run as Administrator: `Panoptes-Spectra.exe /install`
3. Verify: `sc query PanoptesSpectra`

The installer copies the binary to `C:\Program Files\Panoptes\Spectra\`, registers the service, and starts it automatically.

### Option B: Group Policy Deployment

```powershell
$script = @"
if (-not (Get-Service -Name "PanoptesSpectra" -ErrorAction SilentlyContinue)) {
    Start-Process -FilePath "C:\Deployment\Panoptes-Spectra.exe" `
        -ArgumentList "/install" -Wait -NoNewWindow
}
"@

$script | Out-File "\\domain.local\NETLOGON\Install-Spectra.ps1"
```

### Option C: SCCM / Intune Deployment

**Install Command:**
```cmd
Panoptes-Spectra.exe /install
```

**Uninstall Command:**
```cmd
"C:\Program Files\Panoptes\Spectra\Panoptes-Spectra.exe" /uninstall
```

**Detection Method:**
```powershell
Test-Path "HKLM:\SYSTEM\CurrentControlSet\Services\PanoptesSpectra"
```

---

## Post-Deployment Validation

### Immediate Checks (First 5 Minutes)

```powershell
# 1. Service installed and running
Get-Service -Name "PanoptesSpectra" | Select-Object Name, Status, StartType

# 2. Directories created
Test-Path "C:\ProgramData\Panoptes\Spectra\Output"
Test-Path "C:\ProgramData\Panoptes\Spectra\Logs"

# 3. Log file present
Test-Path "C:\ProgramData\Panoptes\Spectra\Logs\spectra_log.txt"

# 4. Tamper protection applied
sc sdshow PanoptesSpectra  # Should contain DENY ACEs
```

### Short-Term Validation (After First Collection Cycle)

```powershell
# 5. Inventory file generated
Test-Path "C:\ProgramData\Panoptes\Spectra\Output\inventory.json"

# 6. JSON is valid
$json = Get-Content "C:\ProgramData\Panoptes\Spectra\Output\inventory.json" | ConvertFrom-Json
Write-Host "Machine ID: $($json.spectraMachineId)"
Write-Host "Machine Name: $($json.machineNetBiosName)"

# 7. Process tracking file generated
Test-Path "C:\ProgramData\Panoptes\Spectra\Output\processes.json"

# 8. Check for errors in log
Select-String -Path "C:\ProgramData\Panoptes\Spectra\Logs\spectra_log.txt" -Pattern "\[-\]"
```

### Long-Term Monitoring (First Week)

- [ ] Service remains running (no unexpected restarts)
- [ ] Data collection occurs on schedule (default: every 24 hours)
- [ ] No disk space issues
- [ ] No performance degradation

---

## Rollback Procedure

```powershell
# Uninstall from installed location
& "C:\Program Files\Panoptes\Spectra\Panoptes-Spectra.exe" /uninstall

# Verify removal
Get-Service -Name "PanoptesSpectra" -ErrorAction SilentlyContinue
# Should return nothing

# If any artifacts remain after reboot
Remove-Item "C:\ProgramData\Panoptes" -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item "C:\Program Files\Panoptes" -Recurse -Force -ErrorAction SilentlyContinue
