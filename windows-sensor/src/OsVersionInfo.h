#pragma once

#include "Utils/Utils.h"

// OS version metadata collected from ntoskrnl.exe and system APIs.
// No paths are hardcoded — the system directory is resolved at runtime.
struct OsVersionInfo
{
    std::wstring osDisplayName;         // e.g., "Microsoft Windows 11 Pro 64-bit"
    std::wstring ntoskrnlPath;          // e.g., "C:\\WINDOWS\\system32\\ntoskrnl.exe"
    std::wstring ntoskrnlVersion;       // e.g., "10.0.22621.4317"
    std::wstring processorArchitecture; // e.g., "x64", "x86", "ARM64" (hardware CPU, not OS bitness)
};

// Collect OS version info by reading ntoskrnl.exe file version resource.
// Uses GetSystemDirectoryW to locate the system directory at runtime.
// Privilege requirement: Standard read access (no elevation needed for version info).
OsVersionInfo GetOsVersionInfo();