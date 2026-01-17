# Quick Fix - Stop Service Before Rebuild

## The Problem
The service executable is locked because the Windows service is still running from the previous installation.

## Solution - Run These Commands

```cmd
REM Open Command Prompt as Administrator

REM Stop the service
sc stop PanoptesSpectra

REM Wait a moment
timeout /t 2

REM Uninstall the service (using OLD executable location)
"D:\spectra\WindowsEnum\bin\x64\Release\Panoptes-Spectra.exe" /uninstall

REM Now rebuild
cd D:\spectra\WindowsEnum
msbuild Windows-Info-Gathering.sln /p:Configuration=Release /p:Platform=x64 /t:Rebuild

REM Install with NEW code (will copy to Program Files and auto-start)
cd bin\x64\Release
Panoptes-Spectra.exe /install

REM Check status
sc query PanoptesSpectra

REM View output
dir "C:\ProgramData\Panoptes\Spectra\Output"
type "C:\ProgramData\Panoptes\Spectra\Output\inventory_latest.json"
```

## What Changed

The new installer will:
1. ? Copy executable to `C:\Program Files\Panoptes\Spectra\`
2. ? Start the service automatically after installation
3. ? Begin data collection immediately
4. ? Create output files in `C:\ProgramData\Panoptes\Spectra\Output\`

## After Rebuild

The new installation will show:
```
[+] Installation directory: C:\Program Files\Panoptes\Spectra
[+] Executable copied successfully
[+] Executable installed to: C:\Program Files\Panoptes\Spectra\Panoptes-Spectra.exe
[+] Service created successfully
[+] Starting service...
[+] Service started successfully!
[+] Service is now RUNNING
[+] Installation completed successfully!
```

## Verify Installation

```cmd
REM Check service is running
sc query PanoptesSpectra

REM Check executable location
sc qc PanoptesSpectra

REM Check output files
dir "C:\ProgramData\Panoptes\Spectra\Output"

REM View latest inventory
type "C:\ProgramData\Panoptes\Spectra\Output\inventory_latest.json" | findstr "spectraMachineId"
```
