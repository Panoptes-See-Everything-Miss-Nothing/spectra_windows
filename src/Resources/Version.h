#pragma once

// ── Panoptes Spectra version information ──────────────────────────────
// Update these values for each release.
//
// Both the resource compiler (Panoptes.rc) and C++ source files can
// include this header so that the version is defined in one place.

#define SPECTRA_VERSION_MAJOR    1
#define SPECTRA_VERSION_MINOR    0
#define SPECTRA_VERSION_PATCH    0
#define SPECTRA_VERSION_BUILD    0

// Comma-separated for VERSIONINFO FILEVERSION / PRODUCTVERSION fields
#define SPECTRA_VERSION_CSV      SPECTRA_VERSION_MAJOR,SPECTRA_VERSION_MINOR,SPECTRA_VERSION_PATCH,SPECTRA_VERSION_BUILD

// String representations (narrow — required by the RC resource compiler)
#define SPECTRA_VERSION_STR      "1.0.0.0"
#define SPECTRA_VERSION_DISPLAY  "1.0.0"

// Product metadata (narrow — required by VERSIONINFO block in Panoptes.rc)
#define SPECTRA_PRODUCT_NAME     "Panoptes Spectra Sensor for Windows"
#define SPECTRA_COMPANY_NAME     "Panoptes Project"
#define SPECTRA_FILE_DESCRIPTION "Panoptes Spectra \x97 Endpoint Inventory Sensor"
#define SPECTRA_COPYRIGHT        "Copyright \xa9 2025 Panoptes Project. Licensed under GPLv3."
#define SPECTRA_INTERNAL_NAME    "Panoptes-Spectra"
#define SPECTRA_ORIGINAL_NAME    "Panoptes-Spectra.exe"

// Architecture label for runtime display (matches output binary naming)
#ifdef _WIN64
#define SPECTRA_ARCH_LABEL       "x64"
#else
#define SPECTRA_ARCH_LABEL       "x86"
#endif

// ── Wide-string versions for use in C++ code (Win32 wide-char APIs) ──
// The narrow macros above are required by the resource compiler (.rc).
// These wide versions are for use in C++ source files with LogError(),
// std::wstring, MessageBox, etc.
#define SPECTRA_VERSION_STR_W      L"1.0.0.0"
#define SPECTRA_VERSION_DISPLAY_W  L"1.0.0"
#define SPECTRA_PRODUCT_NAME_W     L"Panoptes Spectra Sensor for Windows"
#define SPECTRA_INTERNAL_NAME_W    L"Panoptes-Spectra"

#ifdef _WIN64
#define SPECTRA_ARCH_LABEL_W       L"x64"
#else
#define SPECTRA_ARCH_LABEL_W       L"x86"
#endif
