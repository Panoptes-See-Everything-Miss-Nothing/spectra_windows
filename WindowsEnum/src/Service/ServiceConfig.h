#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Service configuration constants
namespace ServiceConfig
{
    // Service identity (CRITICAL: No spaces in internal name for unquoted path protection)
    constexpr const wchar_t* SERVICE_NAME = L"PanoptesSpectra";
    constexpr const wchar_t* SERVICE_DISPLAY_NAME = L"Panoptes Spectra Windows Agent";
    constexpr const wchar_t* SERVICE_DESCRIPTION = L"Panoptes Spectra sensor for vulnerability management. Collects system inventory data using native Windows APIs. Runs as LocalSystem with SE_BACKUP and SE_RESTORE privileges.";
    
    // Service account (LocalSystem required for SE_BACKUP_NAME and SE_RESTORE_NAME)
    constexpr const wchar_t* SERVICE_ACCOUNT = L"LocalSystem";
    
    // Service startup type
    constexpr DWORD SERVICE_START_TYPE = SERVICE_AUTO_START;
    
    // Service dependencies (none required for this service)
    constexpr const wchar_t* SERVICE_DEPENDENCIES = L"";
    
    // Data collection interval (in milliseconds)
    constexpr DWORD COLLECTION_INTERVAL_MS = 3600000; // 1 hour
    
    // Secure output directory
    constexpr const wchar_t* OUTPUT_DIRECTORY = L"C:\\ProgramData\\Panoptes\\Spectra\\Output";
    constexpr const wchar_t* LOG_DIRECTORY = L"C:\\ProgramData\\Panoptes\\Spectra\\Logs";
    constexpr const wchar_t* CONFIG_DIRECTORY = L"C:\\ProgramData\\Panoptes\\Spectra\\Config";
    constexpr const wchar_t* TEMP_DIRECTORY = L"C:\\ProgramData\\Panoptes\\Spectra\\Temp";
    
    // Required privileges (declared to SCM for transparency)
    constexpr const wchar_t* REQUIRED_PRIVILEGES = L"SeBackupPrivilege\0SeRestorePrivilege\0\0";
    
    // Version information
    constexpr const wchar_t* VERSION = L"1.0.0";
    constexpr const wchar_t* PRODUCT_NAME = L"Panoptes Spectra";
    constexpr const wchar_t* VENDOR = L"Panoptes Security";
}
