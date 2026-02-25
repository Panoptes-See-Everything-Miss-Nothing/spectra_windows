# Panoptes Spectra — Patch Detection Strategy

**Document Version:** 1.0
**Date:** February 14, 2026
**Author:** Panoptes Engineering
**Status:** Design Proposal

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Problem Statement](#2-problem-statement)
3. [Current State — What Spectra Collects Today](#3-current-state--what-spectra-collects-today)
4. [Gap Analysis — What's Missing](#4-gap-analysis--whats-missing)
5. [Possible Solutions](#5-possible-solutions)
6. [Solution Comparison Matrix](#6-solution-comparison-matrix)
7. [Recommended Approach — Layered Detection](#7-recommended-approach--layered-detection)
8. [Architecture](#8-architecture)
9. [Implementation Plan](#9-implementation-plan)
10. [Security Considerations](#10-security-considerations)
11. [Appendix A — Data Source Reference](#appendix-a--data-source-reference)
12. [Appendix B — Spectra vs. Industry Comparison](#appendix-b--spectra-vs-industry-comparison)

---

## 1. Executive Summary

Panoptes is a vulnerability management platform. Spectra is its endpoint sensor — a hardened C++20 Windows service that collects system inventory data. The Panoptes server correlates that data against known vulnerabilities (CVEs) to produce per-endpoint risk assessments.

Spectra currently collects installed KBs and OS version information. This covers approximately 50–60% of a typical Microsoft Patch Tuesday. The remaining 30–40% — primarily Microsoft 365 Apps (Click-to-Run), .NET runtimes, and Edge — is invisible to the server because the sensor does not collect the data those components expose.

This document evaluates five approaches for closing that gap and recommends a phased, layered strategy that keeps Spectra lightweight and moves all vulnerability intelligence to the server.

---

## 2. Problem Statement

### 2.1 The Core Challenge

Enterprise vulnerability management requires answering one question per endpoint per CVE:

> **"Is this specific vulnerability present on this specific machine?"**

Answering that question requires two inputs:

| Input | Owner | Description |
|---|---|---|
| **Vulnerability intelligence** | Panoptes Server | CVE → affected product → minimum patched version / KB |
| **Endpoint inventory** | Spectra Sensor | What is actually installed, at what version, with which patches |

If the sensor does not collect a data point, the server cannot evaluate the corresponding CVEs. **Missing data creates blind spots, and blind spots create risk.**

### 2.2 Why KB Enumeration Alone Is Insufficient

Spectra currently queries the Windows Update Agent (WUA) COM API (IUpdateSearcher::Search("IsInstalled=1")) and collects every KB recorded by WUA. This is a strong foundation, but it has known limitations:

| Limitation | Impact |
|---|---|
| **Superseded KB confusion** — Cumulative updates supersede older KBs. Only the latest CU KB may appear, obscuring which individual fixes are present. | Server must maintain a complete KB supersedence chain to reason correctly. |
| **Partial / failed installs** — A KB can appear in WUA as "installed" even when the servicing stack rolled back specific binaries. | False negative: system appears patched but the vulnerable binary was never replaced. |
| **Click-to-Run Office** — Microsoft 365 Apps update on their own channel. They never produce a KB in WUA, Win32_QuickFixEngineering, or DISM. | **Complete blind spot** for ~30–40% of Patch Tuesday CVEs (Office/M365). |
| **.NET 6/7/8/9 runtimes** — Installed independently via the .NET SDK or hosting bundle. Not patched through Windows Update on all configurations. | Missed .NET CVEs on developer workstations and web servers. |
| **Edge (Chromium)** — Auto-updates independently. No WUA KB. | Missed browser CVEs. |
| **Manual file replacement** — An admin restores a DLL from backup, or an attacker downgrades a binary. The KB still shows "installed." | False negative: KB says patched, file says vulnerable. |

### 2.3 What This Means for Panoptes

Without additional data, the Panoptes server cannot:

- Detect unpatched Microsoft 365 Apps (Word, Excel, Outlook, etc.)
- Verify that a cumulative update actually replaced the target binaries
- Assess .NET runtime or Edge CVEs on endpoints where those components update outside Windows Update
- Provide ground-truth validation when KB data is ambiguous

---

## 3. Current State — What Spectra Collects Today

| Data Point | Collection Method | Source Code |
|---|---|---|
| **OS version + build + UBR** | RtlGetVersion via ntdll.dll | src/Main.cpp (version check) |
| **Installed KBs** (WUA) | IUpdateSearcher::Search("IsInstalled=1") | src/InstalledUpdates.cpp |
| **KB metadata** | Title, description, categories, MSRC severity, install date, result code | src/InstalledUpdates.cpp |
| **Win32 apps** (registry) | HKLM SOFTWARE Microsoft Windows CurrentVersion Uninstall (x64 + WOW6432Node) | src/Win32Apps.cpp |
| **MSI products** | MsiEnumProducts / MsiGetProductInfo | src/MsiApps.cpp |
| **Modern AppX/MSIX packages** | WinRT PackageManager | src/WinAppXPackages.cpp |
| **User profiles** | Registry ProfileList enumeration | src/UserProfiles.cpp |
| **Machine identity** | SPECTRA-{GUID} in HKLM registry | src/Service/MachineId.cpp |
| **Network identity** | NetBIOS name, DNS name, IP addresses | src/MachineInfo.cpp |

### What This Covers (Patch Tuesday perspective)

- All Windows OS CVEs (kernel, shell, drivers, Win32k, etc.)
- .NET Framework patches delivered via Windows Update
- MSXML, OLE DB, ODBC components serviced by OS cumulative updates
- Legacy IE components included in the OS CU

### What This Misses

- Microsoft 365 Apps / Office Click-to-Run (30–40% of Patch Tuesday)
- .NET 6/7/8/9 runtimes (installed independently)
- Edge Chromium (auto-updates on its own channel)
- SQL Server (own CU/GDR servicing model)
- Visual Studio (own update channel)
- Ground-truth file version verification

---

## 4. Gap Analysis — What's Missing

Expressed as a Patch Tuesday coverage estimate:

| Category | % of Patch Tuesday CVEs | Currently Collected | Gap |
|---|---|---|---|
| Windows OS (kernel, shell, drivers) | ~50–60% | OS build + KBs | None |
| Microsoft 365 Apps / Office C2R | ~25–35% | — | **Critical** |
| .NET Framework (via WU) | ~5% | Via KBs | None |
| .NET 6/7/8/9 runtimes | ~3–5% | — | Moderate |
| Edge Chromium | ~3–5% | — | Moderate |
| SQL Server | ~2–3% | — | Low (servers only) |
| Other (Visual Studio, etc.) | ~1–2% | — | Low |

**The single biggest gap is Microsoft 365 Apps (Click-to-Run).** This affects nearly every enterprise desktop and represents the largest category of undetectable CVEs.

---

## 5. Possible Solutions

### Solution A: Rely on KB Enumeration Only (Current State)

**Description:** Make no changes. The server correlates only against KB numbers from WUA.

| Pros | Cons |
|---|---|
| Zero development effort | 30–40% of Patch Tuesday CVEs invisible |
| Already implemented and tested | No ground-truth verification |
| Fast collection, minimal I/O | Cannot detect Office/Edge/runtime gaps |

**Verdict:** Insufficient for a production vulnerability management platform.

---

### Solution B: Full Filesystem Scan (Brute-Force File Versions)

**Description:** Recursively scan %SystemRoot%\Windows, %ProgramFiles%, and %ProgramFiles(x86)%, collecting GetFileVersionInfoW for every PE binary.

| Pros | Cons |
|---|---|
| Maximum coverage — every binary on disk | 100,000–330,000 file I/O operations per scan |
| No external intelligence needed | 30–80 MB JSON output per endpoint |
| Catches manual file replacement | 5–15 minutes on spinning disks; AV/EDR amplifies |
| | ~1–2 GB/month per endpoint at daily collection |
| | WinSxS hardlinks cause redundant scans |
| | Still misses files in user profiles and Windows Apps (ACL-blocked) |

**Verdict:** Impractical. The I/O cost, output size, and storage burden are disproportionate to the incremental coverage gained. No major VM vendor uses this approach.

---

### Solution C: OS Build Number Check Only

**Description:** Rely solely on the OS build number (already collected via RtlGetVersion) to determine patch status.

| Pros | Cons |
|---|---|
| Already collected — zero additional work | Only covers Windows OS cumulative updates |
| Definitive for OS-level CVEs | Office C2R: completely separate versioning |
| Maps 1:1 to a specific cumulative update | .NET runtimes: independent update channel |
| | Edge: auto-updates independently |
| | No ground-truth file verification |

**Verdict:** Excellent for OS CVEs (Layer 1), but covers only ~50–60% of Patch Tuesday. Not a complete solution alone.

---

### Solution D: Targeted Registry + Lightweight Probes

**Description:** Collect build/version information from well-known registry keys for the specific products that update outside Windows Update: Office C2R, .NET runtimes, Edge, SQL Server.

| Pros | Cons |
|---|---|
| Closes the biggest gap (Office C2R) immediately | Requires knowing the registry locations per product |
| Minimal I/O — a handful of registry reads | Does not verify actual binary on disk |
| Small JSON footprint (~1–2 KB additional) | New products require new registry paths |
| Fast — less than 100 ms total | |
| Spectra already has SYSTEM-level registry access | |

**Registry locations (well-documented, stable across versions):**

| Product | Registry Path | Value |
|---|---|---|
| M365 Apps / Office C2R | HKLM\SOFTWARE\Microsoft\Office\ClickToRun\Configuration | VersionToReport |
| Office C2R update channel | Same key | UpdateChannel / CDNBaseUrl |
| Office C2R platform | Same key | Platform (x86/x64) |
| .NET Framework 4.x | HKLM\SOFTWARE\Microsoft\NET Framework Setup\NDP\v4\Full | Release (DWORD) |
| .NET 6/7/8/9 runtimes | HKLM\SOFTWARE\dotnet\Setup\InstalledVersions\x64\sharedhost | Version |
| Edge Chromium | HKLM\SOFTWARE\Microsoft\Edge\BLBeacon | version |
| SQL Server instances | HKLM\SOFTWARE\Microsoft\Microsoft SQL Server\Instance Names\SQL | Per-instance version discovery |

**Verdict:** High value, low cost, low risk. This is the highest-ROI next step.

---

### Solution E: Server-Driven Targeted File Version Checks

**Description:** The Panoptes server pushes a JSON manifest to each Spectra sensor listing specific file paths and minimum patched versions. Spectra reads the manifest, calls GetFileVersionInfoW on only those files, and reports the results.

| Pros | Cons |
|---|---|
| Ground-truth verification — the file is either patched or not | Requires server infrastructure to build + distribute manifests |
| Typically 50–200 files per Patch Tuesday — fast and lightweight | Requires a CVE to file path + version mapping database |
| Catches KB-says-patched-but-file-says-vulnerable | Manifest must be updated monthly (after each Patch Tuesday) |
| Agent stays dumb — server owns all intelligence | Requires manifest delivery mechanism (file drop, HTTPS pull) |
| Industry-standard approach (Qualys, Tenable, Rapid7 all do this) | |

**Manifest delivery fits existing infrastructure:**
- ServiceConfig::CONFIG_DIRECTORY points to C:\ProgramData\Panoptes\Spectra\Config\
- ServiceConfig::REG_SERVER_URL is reserved for future HTTPS pull
- Config directory already has proper ACLs (service SID gets Modify)

**Verdict:** The gold standard for vulnerability verification. Should be implemented after Solution D, once the server-side correlation engine exists.

---

## 6. Solution Comparison Matrix

| Criteria | A: KB Only | B: Full Scan | C: OS Build | D: Registry Probes | E: Server Manifest |
|---|---|---|---|---|---|
| **Patch Tuesday coverage** | ~60% | ~80% | ~55% | ~90% | ~95%+ |
| **Office C2R detection** | No | Yes | No | Yes | Yes |
| **Ground-truth verification** | No | Yes | No | No | Yes |
| **Agent I/O cost** | Minimal | Very high | None | Minimal | Low |
| **JSON output increase** | 0 | 30–80 MB | 0 | ~1–2 KB | ~5–20 KB |
| **Collection time** | Seconds | 5–15 min | 0 | < 100 ms | 1–5 sec |
| **Development effort** | None | Medium | None | Low | High (server-side) |
| **Server dependency** | None | None | None | None | Requires manifest pipeline |
| **Maintenance burden** | None | None | None | Low (new products) | Monthly manifest updates |

---

## 7. Recommended Approach — Layered Detection

The recommended strategy implements detection in four layers, each building on the previous. This matches industry best practice (Qualys, Tenable, CrowdStrike all use layered approaches).

| Layer | What | How | Coverage | Priority |
|---|---|---|---|---|
| **Layer 1** | OS Build Number | RtlGetVersion | All Windows OS CVEs | **Done** |
| **Layer 2** | Installed KBs | WUA COM API | .NET Framework, MSXML, IE components | **Done** |
| **Layer 3** | Product Version Probes | Registry reads | Office C2R, .NET runtimes, Edge, SQL | **Next** |
| **Layer 4** | Targeted File Versions | Server-driven manifest + GetFileVersionInfoW | Ground-truth verification | **Future** |

### Why This Order

1. **Layers 1 + 2 are already shipping** — no work required.
2. **Layer 3 is the highest-ROI investment** — a few registry reads close the single biggest gap (Office C2R, ~30% of Patch Tuesday) for less than 100 ms of additional collection time.
3. **Layer 4 requires server infrastructure** — it depends on a manifest pipeline and a CVE-to-file mapping database. This is the right investment once the server-side correlation engine is operational.

### Design Principles

These principles are derived from the project's copilot instructions and apply to all layers:

- **Spectra collects facts; the server decides what they mean.** Zero vulnerability intelligence in the agent.
- **Minimize I/O, allocations, and system calls.** The sensor runs as a long-lived SYSTEM service — every byte matters.
- **Validate all external input at boundaries.** Registry data is hostile input. Manifest files are hostile input.
- **RAII for all resources.** Registry handles, file handles, COM objects.
- **Wide-character APIs exclusively.** RegQueryValueExW, not RegQueryValueExA.

---

## 8. Architecture

### Data Flow

    ┌─────────────────────────────────────────────────────────┐
    │                    Panoptes Server                       │
    │                                                         │
    │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
    │  │ MSRC CSAF API│  │  NVD Feed    │  │ OVAL/SCAP    │  │
    │  │  (free JSON) │  │  (free JSON) │  │ (optional)   │  │
    │  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘  │
    │         │                 │                  │          │
    │         └────────┬────────┘──────────────────┘          │
    │                  ▼                                       │
    │  ┌───────────────────────────────────────────────────┐  │
    │  │           Correlation Engine                       │  │
    │  │  CVE + inventory data → per-endpoint risk report   │  │
    │  └───────────────────────────┬───────────────────────┘  │
    │                              │                          │
    │              ┌───────────────┘ (future: push            │
    │              │                  check manifest)          │
    │              ▼                                           │
    └─────────────────────────────────────────────────────────┘
                   │
          ─ ─ ─ ─ ─ ─ ─ ─ ─  (network boundary)
                   │
    ┌─────────────────────────────────────────────────────────┐
    │                  Spectra Sensor                          │
    │                                                         │
    │  ┌────────────┐ ┌────────────┐ ┌────────────────────┐  │
    │  │ Layer 1    │ │ Layer 2    │ │ Layer 3 (NEW)      │  │
    │  │ OS Build   │ │ KBs (WUA)  │ │ Product Versions   │  │
    │  │ RtlGetVer  │ │ COM API    │ │ Registry probes    │  │
    │  └─────┬──────┘ └─────┬──────┘ └─────┬──────────────┘  │
    │        │              │              │                  │
    │        └──────┬───────┘──────────────┘                  │
    │               ▼                                         │
    │  ┌───────────────────────────────────────────────────┐  │
    │  │              inventory_latest.json                 │  │
    │  │  spectraMachineId, OS build, KBs, Office C2R      │  │
    │  │  build, .NET version, Edge version, etc.           │  │
    │  └───────────────────────────────────────────────────┘  │
    └─────────────────────────────────────────────────────────┘

### Layer 3 — JSON Output Addition

Layer 3 adds a new top-level section to the existing inventory JSON:

    {
      "spectraMachineId": "SPECTRA-A1B2C3D4-...",
      "collectionTimestamp": "2026-02-14T10:30:00",
      "agentVersion": "1.1.0",
      "machineNetBiosName": "WIN-SERVER01",

      "productVersions": {
        "officeClickToRun": {
          "version": "16.0.18324.20194",
          "updateChannel": "http://officecdn.microsoft.com/pr/...",
          "platform": "x64",
          "installed": true
        },
        "dotNetFramework": {
          "release": 533320,
          "version": "4.8.1",
          "installed": true
        },
        "dotNetRuntimes": [
          { "name": "Microsoft.NETCore.App", "version": "8.0.11" },
          { "name": "Microsoft.AspNetCore.App", "version": "8.0.11" }
        ],
        "edgeChromium": {
          "version": "131.0.2903.112",
          "installed": true
        },
        "sqlServerInstances": [
          { "instanceName": "MSSQLSERVER", "version": "16.0.4165.4", "edition": "Standard" }
        ]
      },

      "installedAppsByUser": [ ],
      "installedUpdates": [ ]
    }

---

## 9. Implementation Plan

### Phase 1 — Layer 3: Product Version Probes (Immediate)

**Scope:** Add a new collector module that reads version information from well-known registry keys.

**New files:**
- src/ProductVersions.h — Struct definitions and collector declaration
- src/ProductVersions.cpp — Registry-based version collection

**Modified files:**
- src/Utils/Utils.cpp — Add productVersions section to GenerateJSON()
- src/WindowsEnum.h — Include new header
- Windows-Info-Gathering.vcxproj — Add new source files

**Estimated effort:** 1–2 days
**Risk:** Low — registry reads only, same privilege level Spectra already uses
**Testing:** Verify on Windows 10, Windows 11, Server 2019, Server 2022 with and without Office C2R installed

### Phase 2 — Layer 4: Server-Driven File Version Checks (Future)

**Scope:** Spectra reads a JSON manifest from the Config directory and calls GetFileVersionInfoW + VerQueryValueW on each listed file.

**Prerequisites:**
- Server-side correlation engine operational
- Manifest generation pipeline (MSRC API to CVE to file mapping)
- Manifest delivery mechanism (initially GPO/SCCM file drop; later HTTPS pull via ServerUrl)

**New files:**
- src/FileVersionChecker.h — Manifest parser and version checker
- src/FileVersionChecker.cpp — Implementation

**Estimated effort:** 3–5 days (agent side); weeks–months (server side)
**Risk:** Medium — requires robust JSON parsing, path validation (%SystemRoot% expansion), and AV/EDR-friendly file access patterns

---

## 10. Security Considerations

All implementation must follow the project's security requirements as defined in .github/copilot-instructions.md:

| Concern | Mitigation |
|---|---|
| **Registry data is hostile input** | Validate all string lengths, types, and format before use. Reject unexpected REG_TYPE. Cap string reads at reasonable max (e.g., 1024 chars). |
| **Manifest file is hostile input** (Layer 4) | Validate JSON schema strictly. Reject paths containing .., null bytes, or UNC paths. Only expand approved environment variables (%SystemRoot%, %ProgramFiles%). |
| **File I/O from manifest paths** (Layer 4) | Open files with FILE_SHARE_READ, never follow reparse points, validate path is under expected root directories. |
| **Privilege requirements** | Layer 3 requires only KEY_READ on HKLM — no additional privileges beyond what Spectra already holds. Layer 4 requires SE_BACKUP_NAME for reading protected files — already declared in ServiceConfig::REQUIRED_PRIVILEGES. |
| **Information disclosure** | Do not log file contents or registry values that could contain PII. Log only version strings and paths. |

---

## Appendix A — Data Source Reference

### MSRC Security Update API (Server-Side)

    GET https://api.msrc.microsoft.com/cvrf/v3.0/updates
    GET https://api.msrc.microsoft.com/cvrf/v3.0/cvrf/2026-Feb

- Free, no API key required
- Returns CVE to KB to product to CVSS mapping
- Updated on each Patch Tuesday
- Does NOT include file-level version information

### NVD Data Feed (Server-Side)

    https://nvd.nist.gov/vuln/data-feeds

- CPE-based matching (product + version range)
- Free, JSON format
- Complements MSRC with third-party CVE data

### Office C2R Registry (Agent-Side, Layer 3)

    HKLM\SOFTWARE\Microsoft\Office\ClickToRun\Configuration
      VersionToReport    REG_SZ    "16.0.18324.20194"
      UpdateChannel      REG_SZ    "http://officecdn.microsoft.com/pr/..."
      Platform           REG_SZ    "x64"

### .NET Runtime Registry (Agent-Side, Layer 3)

    HKLM\SOFTWARE\dotnet\Setup\InstalledVersions\x64\sharedhost
      Version            REG_SZ    "8.0.11"

    HKLM\SOFTWARE\dotnet\Setup\InstalledVersions\x64\sharedfx\Microsoft.NETCore.App
      8.0.11             (subkey existence = installed)

---

## Appendix B — Spectra vs. Industry Comparison

| Capability | Spectra (Today) | Spectra (After Layer 3+4) | Qualys Agent | Tenable Agent | CrowdStrike Spotlight |
|---|---|---|---|---|---|
| OS build detection | Yes | Yes | Yes | Yes | Yes |
| KB enumeration | Yes | Yes | Yes | Yes | Yes |
| Office C2R version | No | Yes | Yes | Yes | Yes |
| .NET runtime version | No | Yes | Yes | Yes | Yes |
| Edge version | No | Yes | Yes | Yes | Yes |
| Targeted file checks | No | Yes (Layer 4) | Yes | Yes | Yes |
| Agent-side CVE logic | N/A | No (by design) | Yes | Yes | No |
| Credential-free | Yes | Yes | Yes | Yes | Yes |
| Offline / air-gapped | Yes | Yes | Partial | Partial | No |
| Tamper protection | Yes | Yes | Yes | Yes | Yes |
| Write-restricted SID | Yes | Yes | No | No | No |

---

**End of Document**