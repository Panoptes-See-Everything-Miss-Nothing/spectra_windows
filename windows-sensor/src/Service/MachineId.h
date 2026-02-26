#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

// Machine ID management for Panoptes Spectra
// Generates a cryptographically unique identifier for this machine
namespace MachineId
{
    // Generate or retrieve the unique Spectra Machine ID
    // This ID persists across reboots and reinstalls
    // Format: SPECTRA-{GUID} (e.g., SPECTRA-A1B2C3D4-E5F6-7890-ABCD-EF1234567890)
    std::wstring GetOrCreateMachineId();
    
    // Verify that the stored Machine ID is valid
    bool ValidateMachineId(const std::wstring& machineId);
    
    // Get the Machine ID as UTF-8 string for JSON output
    std::string GetMachineIdUtf8();
    
    // Registry key where Machine ID is stored
    constexpr const wchar_t* MACHINE_ID_REGISTRY_KEY = L"SOFTWARE\\Panoptes\\Spectra";
    constexpr const wchar_t* MACHINE_ID_REGISTRY_VALUE = L"SpectraMachineID";
    
    // Machine ID format constants
    constexpr const wchar_t* MACHINE_ID_PREFIX = L"SPECTRA-";
    constexpr size_t MACHINE_ID_EXPECTED_LENGTH = 44; // "SPECTRA-" + GUID (36 chars)
}
