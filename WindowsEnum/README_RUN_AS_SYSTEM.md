# Running Windows Info Gathering as SYSTEM

## Why SYSTEM Account is Required

This application needs to:
1. Load registry hives (`NTUSER.DAT`) for offline user profiles
2. Access `SeRestorePrivilege` and `SeBackupPrivilege`
3. Read per-user AppX packages from all user SIDs

**Running as Administrator is NOT sufficient** - these privileges are only available to the SYSTEM account.

---

## Option 1: Using PsExec (Recommended for Testing)

### Download PsExec
https://learn.microsoft.com/en-us/sysinternals/downloads/psexec

### Run the Application
```powershell
# Open PowerShell/CMD as Administrator
cd "D:\spectra\Windows-Info-Gathering\bin\x64\Release"

# Run with PsExec
psexec -s -i Windows-Info-Gathering.exe
```

**Flags:**
- `-s` = Run as SYSTEM account
- `-i` = Interactive (shows console output)

---

## Option 2: Task Scheduler (Recommended for Production/Automation)

### Create Scheduled Task

```powershell
# Open Task Scheduler as Administrator
taskschd.msc
```

**Steps:**
1. **Action** ? Create Task (not "Create Basic Task")
2. **General tab:**
   - Name: `Windows Info Gathering`
   - Select: `Run whether user is logged on or not`
   - Check: `Run with highest privileges`
   - Configure for: `Windows 10` (or your Windows version)

3. **Triggers tab:**
   - Click `New...`
   - Begin the task: `On a schedule` (or `At startup`, depending on your needs)
   - Set your schedule

4. **Actions tab:**
   - Click `New...`
   - Action: `Start a program`
   - Program/script: `D:\spectra\Windows-Info-Gathering\bin\x64\Release\Windows-Info-Gathering.exe`
   - Start in: `D:\spectra\Windows-Info-Gathering\bin\x64\Release\`

5. **Conditions tab:**
   - Uncheck `Start the task only if the computer is on AC power` (if laptop)

6. **Settings tab:**
   - Check: `Allow task to be run on demand`

7. Click **OK**
   - Enter your Windows credentials when prompted

### Run the Task Manually (for testing)
```powershell
schtasks /run /tn "Windows Info Gathering"
```

---

## Option 3: PowerShell Script (Alternative)

Create `run_as_system.ps1`:

```powershell
# Requires Administrator elevation first
if (-NOT ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole] "Administrator"))
{
    Write-Warning "Please run as Administrator!"
    exit
}

$action = New-ScheduledTaskAction -Execute "D:\spectra\Windows-Info-Gathering\bin\x64\Release\Windows-Info-Gathering.exe" -WorkingDirectory "D:\spectra\Windows-Info-Gathering\bin\x64\Release\"
$principal = New-ScheduledTaskPrincipal -UserId "NT AUTHORITY\SYSTEM" -LogonType ServiceAccount -RunLevel Highest
$task = New-ScheduledTask -Action $action -Principal $principal

Register-ScheduledTask -TaskName "Windows_Info_Gathering_OneTime" -InputObject $task -Force
Start-ScheduledTask -TaskName "Windows_Info_Gathering_OneTime"

# Wait for completion
Start-Sleep -Seconds 5

# Clean up temporary task
Unregister-ScheduledTask -TaskName "Windows_Info_Gathering_OneTime" -Confirm:$false
```

Run with:
```powershell
powershell -ExecutionPolicy Bypass -File run_as_system.ps1
```

---

## Verify SYSTEM Execution

Check `spectra_log.txt`:

**If running as SYSTEM (correct):**
```
[+] Successfully enabled privilege: SeRestorePrivilege
[+] Loaded registry hive for user: kapil
[+] Found X apps for user: kapil
```

**If NOT running as SYSTEM (incorrect - Admin only):**
```
[-] Privilege 'SeRestorePrivilege' not assigned to current token
[-] Failed to enable privileges for loading user hive: kapil
```

---

## Expected Output

After running as SYSTEM, `inventory.json` should contain:

```json
{
  "installedAppsByUser": [
    {
      "user": "SYSTEM",
      "userSID": "S-1-5-18",
      "applications": [...]
    },
    {
      "user": "admin",
      "userSID": "S-1-5-21-...-1006",
      "applications": [...],
      "appxPackages": [...]
    },
    {
      "user": "kapil",  // ? Now included!
      "userSID": "S-1-5-21-...-1002",
      "applications": [...],
      "appxPackages": [...]
    }
  ]
}
```

---

## Troubleshooting

### "Access Denied" when loading registry hive
- Ensure running as **SYSTEM**, not just Administrator
- Check antivirus isn't blocking registry access

### "kapil" user still missing
- Check `spectra_log.txt` for specific error messages
- Verify `C:\Users\kapil\NTUSER.DAT` file exists and isn't locked
- Try rebooting (releases file locks)

### No AppX packages found
- Normal if user hasn't installed any MS Store apps
- System-wide AppX packages should still appear

---

## Security Note

Running as SYSTEM gives full access to the system. Only use on:
- Development/test machines
- Controlled production environments
- With proper security review

This tool is designed for **inventory collection only** and makes no system modifications.
