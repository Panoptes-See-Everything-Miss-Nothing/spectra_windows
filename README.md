<p align="center">
  <img src="assets/branding/panoptes-logo.png" alt="Panoptes Logo" width="200">
</p>

<h1 align="center">Panoptes</h1>
<p align="center"><em>See Everything. Miss Nothing.</em></p>

<p align="center">
  <a href="https://en.cppreference.com/w/cpp/20"><img src="https://img.shields.io/badge/C%2B%2B-20-blue.svg" alt="C++20"></a>
  <a href="https://www.microsoft.com/windows"><img src="https://img.shields.io/badge/platform-Windows%2010%2B-0078d4.svg" alt="Platform"></a>
  <a href="#build"><img src="https://img.shields.io/badge/arch-x64%20%7C%20x86-green.svg" alt="Architecture"></a>
  <a href="#license"><img src="https://img.shields.io/badge/license-GPLv3-lightgrey.svg" alt="License"></a>
</p>

---

<p align="center">
  Spectra Sensor for Windows is a part of the <strong>Panoptes Platform</strong>.<br>
  🔎 Check the <a href="https://github.com/Panoptes-See-Everything-Miss-Nothing">Panoptes homepage here</a>.
</p>

# Introduction

**Panoptes** is a community-driven vulnerability management platform built to eliminate blind spots that traditional scanners leave behind.

If you've ever asked:

> *"How did this vulnerability exist on the machine when the scanner said it was clean?"*

Panoptes exists because of that question.

---

## Project Status

- **Spectra Windows sensor:** Production-capable and actively maintained  
- **Iris backend:** Under active development  
- **Linux/macOS sensors:** Planned  

---

# Table of Contents

- [Why Panoptes?](#why-panoptes)
- [Architecture Overview](#architecture-overview)
- [Features](#features)
- [System Requirements](#system-requirements)
- [Build](#build)
- [Usage](#usage)
- [Installation](#installation)
- [Configuration](#configuration)
- [Output](#output)
- [Security](#security)
- [Project Structure](#project-structure)
- [Documentation](#documentation)
- [Core Contributors](#core-contributors)
- [Contributing](#contributing)
- [License](#license)

---

# Why Panoptes?

Most vulnerability scanners depend heavily on:

- Command execution  
- Pattern-based detection  
- Signature-per-CVE models  

These approaches introduce:

- False negatives  
- Detection gaps  
- Rule maintenance overhead  
- Security review burden  

Panoptes takes a fundamentally different approach:

> **Inventory first. Correlate intelligently. Minimise blind spots.**

---

# Architecture Overview

Panoptes is modular:

- **Spectra** → Sensor layer (endpoint inventory collection)  
- **Iris** → Backend correlation and intelligence engine  
- *(Future)* Web UI / API / Database components  

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
│                            ┌───────▼───────┐             │
│                            │    Iris     │             │
│                            │  (backend)  │             │
│                            │  NVD match  │             │
│                            └───────┬───────┘             │
│                                   │                    │
│                            ┌───────▼───────┐             │
│                            │  Dashboard  │             │
│                            │  (frontend) │             │
│                            └─────────────┘             │
│                                                         │
└─────────────────────────────────────────────────────────┘

**Spectra Sensor for Windows**

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
   │  │ InstalledUpdates │  │ WindowsServices   │   │
   │  │ OsVersion   │  │ ProcessTracker    │   │
   │  │ MachineInfo ... │  │ UserProfiles ...     │   │
   │  └────────────┘  └───────────────────┘   │
   └──────────────────┬───────────────────────┘
                      │
              ┌───────▼────────────┐
              │  JSON Output        │
              │ inventory.json      │
              │ processes.json      │
              │ mspt_inventory.json │
              └─────────────────────┘
```
---

This repository currently contains:

> **Spectra Sensor for Windows**

### Spectra Sensor for Windows

#### Features

- Deep system inventory (Win32/MSI/AppX, processes, services, updates)  
- Artefact collection even when patch data is missing  
- ETW-based process tracking  
- JSON output for ingestion into SIEM, data lakes, or analytics pipelines  
- Supports querying and CVE correlation (once Iris backend is ready)
- The application is a single native C++ executable (`Panoptes-Spectra.exe`) that can run as a **Windows Service** (periodic collection) or in **console mode** (one-shot collection).
- Uses Volume Shadow Copy (VSS) to safely mount and inspect offline registry hives to perform full per-user inventory even for inactive or logged-out accounts.

#### Using Spectra Today

> **Note:** Iris is currently under active development. In the meantime, the structured JSON inventory produced by Spectra can be ingested into your existing SIEM, data lake, CMDB, or analytics pipeline.  
> This allows you to immediately query your environment for affected versions, analyse CVE exposure, and build custom correlation logic — even before the full Panoptes backend is deployed.

---


### 4. Artefact Collection Even When Patch Data Is Missing

If patch information cannot be determined:

Spectra still collects artefacts answering:

- On which systems is this application present?  
- How many instances exist?  
- Where is it running?  
- What signals are available?  

These artefacts can be used in multiple ways:

- Community members (or in-house teams) can create reusable detection rules based on them.  
- Security teams can run their own queries against inventory data — either within existing data sources (by ingesting Spectra JSON) or, once Iris backend is available, via Iris.

---

# Iris (Backend Correlation Engine)

**Iris** correlates Spectra inventory data against:

- NVD  
- Vendor advisories  
- Patch Tuesday releases  
- Other vulnerability sources  

Instead of signature-per-CVE, Iris:

- Correlates versions automatically  
- Maps inventory against vulnerability ranges  
- Reduces rule-per-CVE detection  

> One intelligent rule per application, not one rule per CVE.

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
4. The output binary is located at `bin\x64\Release\Panoptes-Spectra.exe`.

### Building from the command line

```powershell
msbuild Panoptes.sln /p:Configuration=Release /p:Platform=x64
```

> **Note:** The 32-bit build will refuse to run on 64-bit Windows. Always match the build architecture to the target OS.

---

## Usage

```powershell
Panoptes-Spectra.exe [option]

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
psexec -s -i .\Panoptes-Spectra.exe /console
```

---

## Installation

### Install as a Windows service

```powershell
# Run from an elevated (Administrator) prompt
.\Panoptes-Spectra.exe /install
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
.\Panoptes-Spectra.exe /uninstall   # Uninstall
```

### Upgrade in-place

```powershell
.\Panoptes-Spectra.exe /upgrade
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
| `mspt_inventory.json` | *(work in progress)* Inventory of Microsoft-supplied software and patches collected via the Microsoft Product and Service Tag (MPST) API. |

### Sample Output

Real Spectra output from two endpoints is included in [`spectra_inventory_samples/`](spectra_inventory_samples/):

| Endpoint | `inventory.json` | `processes.json` | `mspt_inventory.json` |
|---|---|---|---|
| **pc1** (physical workstation) | [view](spectra_inventory_samples/pc1/inventory.json) | [view](spectra_inventory_samples/pc1/processes.json) | [view](spectra_inventory_samples/pc1/mspt_inventory.json) |
| **vm1** (virtual machine) | [view](spectra_inventory_samples/vm1/inventory.json) | [view](spectra_inventory_samples/vm1/processes.json) | [view](spectra_inventory_samples/vm1/mspt_inventory.json) |

---

### Logging

Spectra writes operational logs to `spectra_log.txt`:

| Mode | Log location |
|---|---|
| **Service** | `C:\ProgramData\Panoptes\Spectra\Logs\spectra_log.txt` |
| **Console** | Current working directory |

Logs include timestamped entries for collection cycle events, API errors, module failures, and ETW session diagnostics. Enable verbose logging for additional detail:

```powershell
reg add "HKLM\SOFTWARE\Panoptes\Spectra" /v EnableDetailedLogging /t REG_DWORD /d 1 /f
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
├── Panoptes-Spectra-Windows.vcxproj.filters  # Solution Explorer filter layout
├── README.md
├── .gitignore
│
├── .github/
│   └── copilot-instructions.md          # Copilot coding standards and project rules
│
├── assets/
│   ├── branding/
│   │   └── panoptes-logo.png            # Platform logo (README, docs, GitHub social preview)
│   ├── icons/
│   │   ├── spectra.ico                  # Multi-res icon embedded in Panoptes-Spectra.exe
│   │   └── iris.ico                     # Iris backend icon (parked until Iris repo exists)
│   └── icons-macos/
│       └── spectra.icns                 # macOS icon (parked until macOS sensor repo exists)
│
├── src/
│   ├── Main.cpp                 # Entry point: CLI parsing, console/service dispatch
│   ├── WindowsEnum.h            # Aggregate include for all data-collection modules
│   │
│   ├── Resources/
│   │   ├── Panoptes.rc          # Win32 resource script (application icon, version info)
│   │   └── Version.h            # Single-source version and product metadata defines
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
│   ├── MACHINE_ID_IMPLEMENTATION.md
│   ├── PACKAGE_MANAGER_API_MIGRATION.md
│   ├── QUICK_REFERENCE.md
│   ├── SECURE_TEMP_DIRECTORY_IMPROVEMENTS.md
│   ├── VSS_IMPLEMENTATION_SUMMARY.md
│   └── WINDOWS_SERVICE_GUIDE.md
│
├── spectra_inventory_samples/       # Sample Spectra output from real endpoints
│   ├── pc1/
│   │   ├── inventory.json
│   │   ├── mspt_inventory.json
│   │   └── processes.json
│   └── vm1/
│       ├── inventory.json
│       ├── mspt_inventory.json
│       └── processes.json
│
├── bin/                          # Build output (git-ignored)

```

---

## Documentation

Detailed guides are available in the [`docs/`](docs/) directory:

| Document | Description |
|---|---|
| [WINDOWS_SERVICE_GUIDE.md](docs/WINDOWS_SERVICE_GUIDE.md) | Full service implementation guide, access-control matrix, and troubleshooting |
| [DEPLOYMENT_CHECKLIST.md](docs/DEPLOYMENT_CHECKLIST.md) | Step-by-step deployment and verification checklist |
| [QUICK_REFERENCE.md](docs/QUICK_REFERENCE.md) | Command cheat sheet |
| [MACHINE_ID_IMPLEMENTATION.md](docs/MACHINE_ID_IMPLEMENTATION.md) | Machine ID design and implementation details |
| [VSS_IMPLEMENTATION_SUMMARY.md](docs/VSS_IMPLEMENTATION_SUMMARY.md) | Volume Shadow Copy integration summary |
| [SECURE_TEMP_DIRECTORY_IMPROVEMENTS.md](docs/SECURE_TEMP_DIRECTORY_IMPROVEMENTS.md) | Secure temporary directory design |
| [PACKAGE_MANAGER_API_MIGRATION.md](docs/PACKAGE_MANAGER_API_MIGRATION.md) | Migration from registry-based to WinRT Package Manager API |

---

# Core Contributors

## Vaibhav Kakade
- 💼 [![LinkedIn](https://img.shields.io/badge/LinkedIn-Vaibhav%20Kakade-0A66C2?logo=linkedin&logoColor=white)](https://www.linkedin.com/in/vgkakade/)
- 𝕏 [![X](https://img.shields.io/badge/X-@vk_appledore-000000?logo=x&logoColor=white)](https://x.com/vk_appledore)
- 🧑‍💻 [![GitHub](https://img.shields.io/badge/GitHub-vkappledore-181717?logo=github&logoColor=white)](https://github.com/vkappledore/)

## Sanoop Thomas
- 💼 [![LinkedIn](https://img.shields.io/badge/LinkedIn-s4n7h0-0A66C2?logo=linkedin&logoColor=white)](https://www.linkedin.com/in/s4n7h0/)
- 𝕏 [![X](https://img.shields.io/badge/X-@s4n7h0-000000?logo=x&logoColor=white)](https://x.com/s4n7h0)
- 🧑‍💻 [![GitHub](https://img.shields.io/badge/GitHub-s4n7h0-181717?logo=github&logoColor=white)](https://github.com/s4n7h0/)

## Narendra Shinde
- 💼 [![LinkedIn](https://img.shields.io/badge/LinkedIn-narendrashinde-0A66C2?logo=linkedin&logoColor=white)](https://www.linkedin.com/in/narendrashinde/)
- 𝕏 [![X](https://img.shields.io/badge/X-@nushinde-000000?logo=x&logoColor=white)](https://x.com/nushinde)
- 🧑‍💻 [![GitHub](https://img.shields.io/badge/GitHub-Nushinde-181717?logo=github&logoColor=white)](https://github.com/Nushinde)

## Kapil Khot
- 💼 [![LinkedIn](https://img.shields.io/badge/LinkedIn-Kapil%20Khot-0A66C2?logo=linkedin&logoColor=white)](https://www.linkedin.com/in/kapil-khot-50466952/)
- 𝕏 [![X](https://img.shields.io/badge/X-@kapil_khot-000000?logo=x&logoColor=white)](https://x.com/kapil_khot)
- 🧑‍💻 [![GitHub](https://img.shields.io/badge/GitHub-SlidingWindow-181717?logo=github&logoColor=white)](https://github.com/SlidingWindow)
---

# Contributing

Community contributions are welcome.

If you have:

- Detection artefacts  
- Version mapping improvements  
- Edge-case installation samples  
- Performance optimisations  
- API improvements
- Test bed and/or test cases

Open an issue or submit a pull request.

Let’s build something that actually sees everything.


---

# Licensing

Panoptes is licensed under the **GNU General Public License v3 (GPLv3)**.

This means:

- You are free to **use, modify, and distribute** Panoptes.
- Any modified or derivative works must also be licensed under **GPLv3**.
- See the [`LICENSE`](LICENSE) file for full terms.

For more details on GPLv3, visit: [https://www.gnu.org/licenses/gpl-3.0.en.html](https://www.gnu.org/licenses/gpl-3.0.en.html)

---
