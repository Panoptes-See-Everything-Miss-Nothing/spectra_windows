#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

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
    
    // Registry configuration key
    constexpr const wchar_t* REGISTRY_KEY = L"SOFTWARE\\Panoptes\\Spectra";
    
    // Registry configuration values (can be set by MSI installer or GPO)
    constexpr const wchar_t* REG_COLLECTION_INTERVAL = L"CollectionIntervalSeconds";
    constexpr const wchar_t* REG_SERVER_URL = L"ServerUrl";
    constexpr const wchar_t* REG_ENABLE_DETAILED_LOGGING = L"EnableDetailedLogging";
    constexpr const wchar_t* REG_OUTPUT_DIRECTORY = L"OutputDirectory";
    constexpr const wchar_t* REG_ENABLE_PROCESS_TRACKING = L"EnableProcessTracking";
    
    // Default values (used if registry not configured)
    constexpr DWORD DEFAULT_COLLECTION_INTERVAL_SECONDS = 86400; // 24 hours (daily collection)
    constexpr const wchar_t* DEFAULT_OUTPUT_DIRECTORY = L"C:\\ProgramData\\Panoptes\\Spectra\\Output";
    constexpr const wchar_t* DEFAULT_LOG_DIRECTORY = L"C:\\ProgramData\\Panoptes\\Spectra\\Logs";
    constexpr const wchar_t* DEFAULT_CONFIG_DIRECTORY = L"C:\\ProgramData\\Panoptes\\Spectra\\Config";
    
    // Legacy constants for backward compatibility
    constexpr DWORD COLLECTION_INTERVAL_MS = DEFAULT_COLLECTION_INTERVAL_SECONDS * 1000;
    constexpr const wchar_t* OUTPUT_DIRECTORY = DEFAULT_OUTPUT_DIRECTORY;
    constexpr const wchar_t* LOG_DIRECTORY = DEFAULT_LOG_DIRECTORY;
    constexpr const wchar_t* CONFIG_DIRECTORY = DEFAULT_CONFIG_DIRECTORY;
    constexpr const wchar_t* TEMP_DIRECTORY = L"C:\\ProgramData\\Panoptes\\Spectra\\Temp";
    
    // Required privileges (declared to SCM for transparency).
    // SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO strips all undeclared privileges
    // from the service token. SeSystemProfilePrivilege is required for ETW
    // session creation (StartTraceW) — without it, SYSTEM gets ACCESS_DENIED.
    constexpr const wchar_t* REQUIRED_PRIVILEGES = L"SeBackupPrivilege\0SeRestorePrivilege\0SeSystemProfilePrivilege\0\0";
    
    // Version information
    constexpr const wchar_t* VERSION = L"1.0.0";
    constexpr const wchar_t* PRODUCT_NAME = L"Panoptes Spectra";
    constexpr const wchar_t* VENDOR = L"Panoptes Security";
    
    // Configuration helper functions
    DWORD GetCollectionIntervalSeconds();
    std::wstring GetOutputDirectory();
    std::wstring GetServerUrl();
    bool IsDetailedLoggingEnabled();
    bool IsProcessTrackingEnabled();
}
