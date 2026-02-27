<p align="center">
  <img src="assets/branding/panoptes-logo.png" alt="Panoptes Logo" width="200">
</p>

<h1 align="center">Panoptes</h1>
<p align="center"><em>See Everything. Miss Nothing.</em></p>

<p align="center">
  <a href="https://en.cppreference.com/w/cpp/20"><img src="https://img.shields.io/badge/C%2B%2B-20-blue.svg" alt="C++20"></a>
  <a href="https://www.microsoft.com/windows"><img src="https://img.shields.io/badge/platform-Windows%2010%2B-0078d4.svg" alt="Platform"></a>
  <a href="#build"><img src="https://img.shields.io/badge/arch-x64%20%7C%20x86-green.svg" alt="Architecture"></a>
  <a href="#license"><img src="https://img.shields.io/badge/license-Proprietary-lightgrey.svg" alt="License"></a>
</p>

**Panoptes** is a community-driven vulnerability management platform built to cover the blind spots that commercial offerings often leave behind. If you've ever wondered why your scanner keeps missing known vulnerabilities — false negatives hiding in plain sight — this project exists because of that.

**Spectra** is the sensor layer of Panoptes. This repository contains **Spectra Sensor for Windows** — an enterprise-grade, native Windows service that collects comprehensive system inventory data. It runs as `LocalSystem`, uses only native Win32/COM/WinRT APIs (no third-party dependencies), and outputs machine-readable JSON for downstream analysis by **Iris**, the Panoptes backend.

### The Panoptes Platform

```
┌─────────────────────────────────────────────────────────┐
│                    Panoptes Platform                    │
│           See Everything. Miss Nothing.                 │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │   Spectra    │  │   Spectra    │  │   Spectra    │  │
│  │   Windows    │  │    Linux     │  │    macOS     │  │
│  │  (this repo) │  │  (planned)   │  │  (planned)   │  │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘  │
│         │                 │                 │          │
│         └────────┬────────┴────────┬────────┘          │
│                  │                 │                    │
│           ┌──────▼──────┐  ┌──────▼──────┐             │
│           │  Ingestion  │  │  Database   │             │
│           │  Pipeline   │──▶│  (findings) │             │
│           └─────────────┘  └──────┬──────┘             │
│                                   │                    │
│                            ┌──────▼──────┐             │
│                            │    Iris     │             │
│                            │  (backend)  │             │
│                            │  NVD match  │             │
│                            └──────┬──────┘             │
│                                   │                    │
│                            ┌──────▼──────┐             │
│                            │  Dashboard  │             │
│                            │  (frontend) │             │
│                            └─────────────┘             │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

**How it works:** Spectra sensors collect detailed system inventory (installed software, versions, patches, services, etc.) from endpoints. Iris, the backend, correlates those findings against the NVD and other vulnerability data sources to identify what's actually vulnerable — no need to maintain per-CVE detection signatures for the vast majority of cases. This design keeps operational overhead low while maximising detection accuracy.

> **Status:** Spectra Sensor for Windows is in active development. Iris (backend) is a work in progress. Linux and macOS sensors are planned.

---

## Table of Contents

- [Why Panoptes?](#why-panoptes)
- [Features](#features)
- [Architecture](#architecture)
- [System Requirements](#system-requirements)
- [Build](#build)
- [Usage](#usage)
- [Installation](#installation)
- [Configuration](#configuration)
- [Output](#output)
- [Security](#security)
- [Project Structure](#project-structure)
- [Documentation](#documentation)
- [Contributing](#contributing)
- [License](#license)

---

## Why Panoptes?

Commercial vulnerability scanners are expensive, opaque, and — if you look closely — riddled with false negatives. They miss things. Sometimes because of architectural shortcuts, sometimes because maintaining detection rules for every CVE across every product is an enormous burden.

Panoptes takes a different approach:

- **Accurate inventory first.** Spectra sensors collect ground-truth data about what's actually installed — versions, patches, registry artefacts, package metadata — using native OS APIs, not heuristics.
- **Automated correlation.** Iris matches inventory data against the NVD's CPE and version-range data. No hand-written signatures needed for the common case.
- **Community-powered coverage.** Some applications need custom detection logic (specific registry keys, non-standard version storage, etc.). That's where community contributions matter — testing, security review, and gathering artefacts for the edge cases no single team can cover alone.
- **Low overhead.** The system is designed so that adding coverage for a new product doesn't require writing and maintaining a detection rule from scratch every time.

The goal is simple: give the security community a tool that's honest about what's on the machine and what's vulnerable — and make it sustainable to maintain.

---

## Features

| Category | Details |
|---|---|
| **Win32 / Registry Apps** | Enumerates `HKLM` and per-user `HKCU` Uninstall keys for all user profiles (loads offline hives via `RegLoadKey`). Results are grouped per-user with SID in the output. |
| **MSI Products** | Queries the Windows Installer API (`MsiEnumProductsEx`) for system-wide and per-user MSI packages, including product code, package code, install source, and assignment type. |
| **AppX / MSIX Packages** | Enumerates modern Store, sideloaded, and provisioned packages via the Windows Package Manager WinRT API. Captures publisher ID, resource ID, framework/bundle/dev-mode flags, and per-user ownership. |
| **Installed Updates (KBs)** | Collects installed Windows updates with MSRC severity, KB article IDs, categories, and install dates via the Windows Update Agent COM API — the data needed to assess Patch Tuesday compliance. |
| **Windows Services** | Lists all installed Win32 services with start type, running state, binary path, and service account. |
| **OS Version Info** | Reads the `ntoskrnl.exe` file-version resource for accurate build/UBR data (maps directly to specific Patch Tuesday cumulative updates); detects processor architecture. |
| **Machine & Network Info** | Collects NetBIOS name, DNS/FQDN, and local IP addresses (IPv4 + IPv6). |
| **User Profiles** | Enumerates all local user profiles (SID, profile path, hive-loaded state). |
| **Process Tracking** | Real-time process execution monitoring via ETW (`Microsoft-Windows-Kernel-Process`) with automatic Sysmon event-log fallback. Captures image path, command line, user SID/username, parent process (PID + image path), and PE file version. Includes point-in-time snapshot (`CreateToolhelp32Snapshot`) to catch processes running before the ETW session started. Service processes are excluded (by SCM PID, `services.exe`/`svchost.exe` parentage, and managed-path prefixes). Deduplication, per-event diagnostics, and shutdown flush of buffered data before service stop. |
| **Persistent Machine ID** | Generates a cryptographically unique `SPECTRA-{GUID}` identifier stored in the registry, surviving reboots and reinstalls. |
| **VSS Snapshots** | RAII-managed Volume Shadow Copy snapshots for safe, consistent reads of locked system files. |

### Patch Tuesday Coverage

Spectra already collects the data needed to evaluate Patch Tuesday compliance on every Windows endpoint:

- **OS build and UBR** from the `ntoskrnl.exe` file-version resource — this maps 1:1 to specific cumulative updates, making it possible to determine exactly which monthly rollup is installed without relying on KB enumeration alone.
- **All installed KBs** with MSRC severity ratings (`Critical`, `Important`, etc.), update categories, and installation timestamps — queried directly from the local Windows Update Agent datastore, no WSUS or network connectivity required.
- **Update history enrichment** — cross-references installed updates with the WUA history to capture precise install dates and operation result codes.

Iris uses this data to correlate against Microsoft's published Patch Tuesday advisories and the NVD, identifying which security updates are missing and what CVEs are exposed as a result.

---

## Architecture

### Spectra Sensor for Windows

```
┌─────────────────────────────────────────────────────┐
│                  Main.cpp (Entry Point)             │
│   /install · /upgrade · /uninstall · /console · SCM │
└────────┬────────────────────────────┬───────────────┘
         │                            │
   ┌─────▼──────┐            ┌───────▼────────┐
   │  Service    │            │  Console Mode  │
   │  Framework  │            │  (one-shot)    │
   └─────┬──────┘            └───────┬────────┘
         │                            │
   ┌─────▼────────────────────────────▼───────┐
   │          Data Collection Engine           │
   │  ┌────────────┐  ┌───────────────────┐   │
   │  │ Win32Apps   │  │ WinAppXPackages   │   │
   │  │ MsiApps     │  │ AppXPackages      │   │
   │  │ InstalledUp │  │ WindowsServices   │   │
   │  │ OsVersion   │  │ ProcessTracker    │   │
   │  │ MachineInfo │  │ UserProfiles      │   │
   │  └────────────┘  └───────────────────┘   │
   └──────────────────┬───────────────────────┘
                      │
              ┌───────▼───────┐
              │  JSON Output  │
              │ inventory.json│
              │ processes.json│
              └───────────────┘
```

The application is a single native C++ executable (`Panoptes.exe`) that can run as a **Windows Service** (periodic collection) or in **console mode** (one-shot collection).

---

## System Requirements

| Requirement | Details |
|---|---|
| **OS** | Windows 10 / Windows Server 2016 or later |
| **Architecture** | x64 (primary) or x86 (32-bit on 32-bit OS only) |
| **Runtime privileges** | `LocalSystem` (NT AUTHORITY\SYSTEM) — required for `SE_BACKUP_NAME`, `SE_RESTORE_NAME`, and kernel ETW sessions |
| **Installation privileges** | Local Administrator |
| **Disk space** | ~50 MB for the application plus variable space for output data |
| **Dependencies** | None — all functionality is implemented using native Windows APIs |

---

## Build

### Prerequisites

- **Visual Studio 2025** (v145 platform toolset) or later
- **Windows 10 SDK** (10.0 or later)
- **C++20** language standard

### Building from Visual Studio

1. Open `Panoptes.sln`.
2. Select the desired configuration (`Release|x64` recommended).
3. Build the solution (**Ctrl+Shift+B**).
4. The output binary is located at `bin\x64\Release\Panoptes.exe`.

### Building from the command line

```powershell
msbuild Panoptes.sln /p:Configuration=Release /p:Platform=x64
```

> **Note:** The 32-bit build will refuse to run on 64-bit Windows. Always match the build architecture to the target OS.

---

## Usage

```
Panoptes.exe [option]

Options:
  /install      Install as a Windows service (auto-upgrades if already installed)
  /upgrade      Upgrade the service in-place (preserves state and Machine ID)
  /uninstall    Remove the Windows service and all artifacts
  /console      Run a one-time data collection in the console
  /test         Run data collection and display a summary
  /? -? --help /help  Show usage information
  (no args)     Launched by the Service Control Manager (SCM)
```

### Quick test (no installation required)

```powershell
# Run as SYSTEM for full visibility (Administrator is NOT sufficient)
psexec -s -i .\Panoptes.exe /console
```

---

## Installation

### Install as a Windows service

```powershell
# Run from an elevated (Administrator) prompt
.\Panoptes.exe /install
```

This will:

1. Copy the executable to `C:\Program Files\Panoptes\Spectra\`.
2. Register the `PanoptesSpectra` service with `SERVICE_AUTO_START`.
3. Apply service hardening (SID restriction, declared privileges, tamper-protection DACL).
4. Create secure data directories under `C:\ProgramData\Panoptes\Spectra\`.
5. Generate a persistent Machine ID in the registry.
6. Start the service automatically.

### Manage the service

```powershell
sc start PanoptesSpectra      # Start
sc stop  PanoptesSpectra      # Stop
sc query PanoptesSpectra      # Check status
.\Panoptes.exe /uninstall   # Uninstall
```

### Upgrade in-place

```powershell
.\Panoptes.exe /upgrade
```

Replaces the binary and re-applies hardening while preserving the Machine ID, registry configuration, logs, and output data.

---

## Configuration

Runtime configuration is stored in the registry at `HKLM\SOFTWARE\Panoptes\Spectra` and can be modified without recompilation (e.g., via GPO or MSI custom actions). Configuration is reloaded from the registry at the start of each collection cycle — changes take effect without restarting the service.

| Value Name | Type | Default | Description |
|---|---|---|---|
| `CollectionIntervalSeconds` | `REG_DWORD` | `86400` (24 h) | Interval between data collections |
| `OutputDirectory` | `REG_SZ` | `C:\ProgramData\Panoptes\Spectra\Output` | Directory for JSON output files |
| `ServerUrl` | `REG_SZ` | *(empty)* | Future: endpoint for data upload |
| `EnableDetailedLogging` | `REG_DWORD` | `0` | Enable verbose logging |
| `EnableProcessTracking` | `REG_DWORD` | `1` | Enable ETW-based process tracking |

### Examples

```powershell
# Set collection interval to 30 minutes
reg add "HKLM\SOFTWARE\Panoptes\Spectra" /v CollectionIntervalSeconds /t REG_DWORD /d 1800 /f

# Change output directory
reg add "HKLM\SOFTWARE\Panoptes\Spectra" /v OutputDirectory /t REG_SZ /d "D:\Spectra\Output" /f
```

---

## Output

The service produces two JSON files per collection cycle in the configured output directory:

| File | Contents |
|---|---|
| `inventory.json` | Machine identity, network info, per-user application inventory (Win32/MSI/AppX grouped by user with SID), OS version (via `ntoskrnl.exe`), installed updates with MSRC metadata, Windows services, and process tracking summary. |
| `processes.json` | Non-service processes observed via real-time ETW monitoring + point-in-time snapshot, with image path, command line, user, parent process, PE file version, and full tracker diagnostics. |

### Sample `inventory.json` structure

```json
{
  "spectraMachineId": "SPECTRA-A1B2C3D4-E5F6-7890-ABCD-EF1234567890",
  "collectionTimestamp": "2025-01-15T14:30:45",
  "agentVersion": "1.0.0",
  "machineNetBiosName": "WORKSTATION01",
  "machineDnsName": "workstation01.corp.local",
  "ipAddresses": ["10.0.1.42", "fe80::1"],
  "installedAppsByUser": [
    {
      "user": "SYSTEM",
      "userSID": "S-1-5-18",
      "applications": [ { "displayName": "...", "displayVersion": "...", "publisher": "..." } ],
      "msiProducts": [ { "productCode": "...", "productName": "...", "productVersion": "..." } ],
      "modernAppPackages": [ { "packageFullName": "...", "version": "...", "isFramework": false } ]
    }
  ],
  "installedUpdates": {
    "osVersion": {
      "os": "Microsoft Windows 11 Pro 64-bit",
      "ntoskrnl.exeVersion": "10.0.26100.4351",
      "processorArchitecture": "x64"
    },
    "updates": [
      {
        "title": "2025-01 Cumulative Update for Windows 11...",
        "kbArticleIds": ["5050009"],
        "msrcSeverity": "Critical",
        "installedDate": "2025-01-15T03:00:00"
      }
    ]
  },
  "windowsServices": [ { "serviceName": "wuauserv", "displayName": "Windows Update", "startType": "Manual", "currentState": "Stopped" } ],
  "processTrackingSummary": { "activeSource": "EtwKernelProcess", "isActive": true, "totalEventsReceived": 1247 }
}
```

### Sample `processes.json` structure

```json
{
  "spectraMachineId": "SPECTRA-A1B2C3D4-E5F6-7890-ABCD-EF1234567890",
  "collectionTimestamp": "2025-01-15T14:30:45",
  "agentVersion": "1.0.0",
  "runningProcesses": [
    {
      "imagePath": "C:\\Users\\admin\\Downloads\\tool.exe",
      "commandLine": "tool.exe --scan",
      "userSid": "S-1-5-21-...",
      "username": "admin",
      "processId": 5678,
      "parentProcessId": 1234,
      "parentImagePath": "C:\\Windows\\explorer.exe",
      "firstSeenTimestamp": "2025-01-15T14:25:12",
      "fileVersion": "2.1.0.0"
    }
  ],
  "processTrackerDiagnostics": {
    "activeSource": "EtwKernelProcess",
    "totalEventsReceived": 1247,
    "eventsDeduplicatedOut": 83,
    "serviceProcessesExcluded": 412,
    "managedPathProcessesExcluded": 298
  }
}
```

---

## Security

Panoptes Spectra is designed with defence-in-depth for enterprise deployment:

| Layer | Mechanism |
|---|---|
| **Unquoted-path protection** | The executable path is always **quoted** at install time; the service name (`PanoptesSpectra`) contains no spaces. |
| **Service SID restriction** | `SERVICE_SID_TYPE_RESTRICTED` creates a write-restricted token — only explicitly allowed SIDs can write. |
| **Declared privileges** | Only `SE_BACKUP_NAME`, `SE_RESTORE_NAME`, and `SE_SYSTEM_PROFILE_NAME` are kept; all others are stripped by the SCM. |
| **Tamper-protection DACL** | Administrators can start/stop but **cannot modify or delete** the service. Only SYSTEM has full control. |
| **Control Flow Guard** | Enabled at compile time for all configurations. |
| **Secure temp directories** | Validated against symlink attacks and restricted with per-directory DACLs. |
| **Automatic failure recovery** | Restarts on crash (2 retries with backoff, 24-hour failure counter reset). |
| **Architecture enforcement** | 32-bit builds refuse to execute on 64-bit Windows to prevent WoW64 registry redirection issues. |

---

## Project Structure

```
├── Panoptes.sln                         # Visual Studio solution
├── Panoptes-Spectra-Windows.vcxproj     # C++ project (v145, C++20, Win10 SDK)
├── README.md
├── .gitignore
│
├── assets/
│   ├── branding/
│   │   └── panoptes-logo.png            # Platform logo (README, docs, GitHub social preview)
│   ├── icons/
│   │   ├── spectra.ico                  # Multi-res icon embedded in Panoptes.exe
│   │   ├── spectra-alt.ico              # Alternate Spectra icon variant
│   │   └── iris.ico                     # Iris backend icon (parked until Iris repo exists)
│   └── icons-macos/
│       └── spectra.icns                 # macOS icon (parked until macOS sensor repo exists)
│
├── src/
│   ├── Main.cpp                 # Entry point: CLI parsing, console/service dispatch
│   ├── WindowsEnum.h            # Aggregate include for all data-collection modules
│   │
│   ├── Resources/
│   │   └── Panoptes.rc          # Win32 resource script (application icon)
│   │
│   ├── MachineInfo.cpp/.h       # NetBIOS name, DNS/FQDN, IP addresses
│   ├── OsVersionInfo.cpp/.h     # OS display name, ntoskrnl version, CPU arch
│   ├── UserProfiles.cpp/.h      # User profile enumeration (SID, path, hive state)
│   ├── RegistryUtils.cpp/.h     # Safe registry string reads
│   ├── RegistryHiveLoader.cpp/.h # RAII offline registry hive loading
│   ├── PrivMgmt.cpp/.h          # Privilege enable/disable helpers
│   ├── Win32Apps.cpp/.h         # Registry-based installed applications
│   ├── MsiApps.cpp/.h           # Windows Installer (MSI) product enumeration
│   ├── AppXPackages.cpp/.h      # AppX/MSIX package enumeration
│   ├── WinAppXPackages.cpp/.h   # Modern app enumeration via WinRT Package Manager
│   ├── InstalledUpdates.cpp/.h  # Windows Update Agent (WUA) KB enumeration
│   ├── WindowsServices.cpp/.h   # SCM service enumeration
│   ├── ProcessTracker.cpp/.h    # ETW + Sysmon real-time process tracking
│   ├── VSSSnapshot.cpp/.h       # Volume Shadow Copy RAII wrapper
│   │
│   ├── Utils/
│   │   └── Utils.cpp/.h         # JSON generation, logging, UTF-8 helpers, security validation
│   │
│   └── Service/
│       ├── ServiceMain.cpp/.h           # SCM entry point, worker thread, periodic collection
│       ├── ServiceInstaller.cpp/.h      # Install/upgrade/uninstall with full hardening
│       ├── ServiceConfig.cpp/.h         # Registry-based runtime configuration
│       ├── ServiceTamperProtection.cpp/.h # Restrictive service DACL management
│       └── MachineId.cpp/.h             # Persistent SPECTRA-{GUID} machine identifier
│
├── docs/
│   ├── DEPLOYMENT_CHECKLIST.md
│   ├── IMPLEMENTATION_COMPLETE.md
│   ├── IMPLEMENTATION_SUMMARY.md
│   ├── MACHINE_ID_IMPLEMENTATION.md
│   ├── PACKAGE_MANAGER_API_MIGRATION.md
│   ├── QUICKSTART.md
│   ├── QUICK_REFERENCE.md
│   ├── REBUILD_INSTRUCTIONS.md
│   ├── SECURE_TEMP_DIRECTORY_IMPROVEMENTS.md
│   ├── VSS_IMPLEMENTATION_SUMMARY.md
│   ├── WINDOWS7_DELAY_LOAD_REQUIRED.md
│   ├── WINDOWS_SERVICE_GUIDE.md
│   ├── WINDOWS_VERSION_COMPATIBILITY.md
│   └── WINRT_SETUP_INSTRUCTIONS.md
│
├── bin/                          # Build output (git-ignored)
└── .github/
    └── workflows/
        └── request-pan-ticket.yml  # Issue-tracking ticket automation
```

---

## Documentation

Detailed guides are available in the [`docs/`](docs/) directory:

| Document | Description |
|---|---|
| [WINDOWS_SERVICE_GUIDE.md](docs/WINDOWS_SERVICE_GUIDE.md) | Full service implementation guide, access-control matrix, and troubleshooting |
| [DEPLOYMENT_CHECKLIST.md](docs/DEPLOYMENT_CHECKLIST.md) | Step-by-step deployment and verification checklist |
| [QUICKSTART.md](docs/QUICKSTART.md) | 5-minute quick-start guide |
| [QUICK_REFERENCE.md](docs/QUICK_REFERENCE.md) | Command cheat sheet |
| [REBUILD_INSTRUCTIONS.md](docs/REBUILD_INSTRUCTIONS.md) | How to rebuild while the service is running |
| [WINDOWS_VERSION_COMPATIBILITY.md](docs/WINDOWS_VERSION_COMPATIBILITY.md) | OS compatibility matrix for modern app enumeration |
| [MACHINE_ID_IMPLEMENTATION.md](docs/MACHINE_ID_IMPLEMENTATION.md) | Machine ID design and implementation details |
| [VSS_IMPLEMENTATION_SUMMARY.md](docs/VSS_IMPLEMENTATION_SUMMARY.md) | Volume Shadow Copy integration summary |
| [SECURE_TEMP_DIRECTORY_IMPROVEMENTS.md](docs/SECURE_TEMP_DIRECTORY_IMPROVEMENTS.md) | Secure temporary directory design |
| [IMPLEMENTATION_COMPLETE.md](docs/IMPLEMENTATION_COMPLETE.md) | Windows service implementation completion summary |
| [IMPLEMENTATION_SUMMARY.md](docs/IMPLEMENTATION_SUMMARY.md) | Machine ID, VSS, and feature implementation details |
| [PACKAGE_MANAGER_API_MIGRATION.md](docs/PACKAGE_MANAGER_API_MIGRATION.md) | Migration from registry-based to WinRT Package Manager API |
| [WINDOWS7_DELAY_LOAD_REQUIRED.md](docs/WINDOWS7_DELAY_LOAD_REQUIRED.md) | Delay-load configuration for Windows 7 compatibility |
| [WINRT_SETUP_INSTRUCTIONS.md](docs/WINRT_SETUP_INSTRUCTIONS.md) | C++/WinRT setup and build instructions |

---

## Contributing

Panoptes is a community-driven project. Contributions are welcome in many forms:

- **Code** — New collection modules, bug fixes, performance improvements.
- **Testing** — Run Spectra on diverse environments and report what it finds (or misses).
- **Security review** — Audit the codebase, suggest hardening improvements.
- **Application artefacts** — Help document where specific applications store their version and patch information (registry keys, file paths, installer metadata). This is the kind of work that scales only with community support — no single team can procure and reverse-engineer every application out there.
- **Documentation** — Improve guides, add examples, translate.

If you find this project useful and want to help close the gaps that commercial scanners leave open, we'd be glad to have you.

---

## License

Proprietary — © Panoptes Security. All rights reserved.
