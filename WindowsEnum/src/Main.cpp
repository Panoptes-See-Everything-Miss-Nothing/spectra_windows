#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "Utils/Utils.h"
#include "Service/ServiceMain.h"
#include "Service/ServiceInstaller.h"
#include "Service/ServiceConfig.h"
#include <iostream>
#include <shlobj.h>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "advapi32.lib")

static std::wstring GetLogsDirectory()
{
    std::wstring logDir = ServiceConfig::DEFAULT_LOG_DIRECTORY;
    DWORD attribs = GetFileAttributesW(logDir.c_str());
    if (attribs == INVALID_FILE_ATTRIBUTES || !(attribs & FILE_ATTRIBUTE_DIRECTORY))
    {
        SHCreateDirectoryExW(nullptr, logDir.c_str(), nullptr);
    }

    return logDir;
}

static void AppendTrace(const wchar_t* message)
{
    if (message == nullptr || *message == L'\0')
    {
        return;
    }

    std::wstring logDir = GetLogsDirectory();
    std::wstring path = logDir + L"\\trace.txt";

    HANDLE hFile = CreateFileW(
        path.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (hFile == INVALID_HANDLE_VALUE)
    {
        return;
    }

    SYSTEMTIME st = {};
    GetLocalTime(&st);

    wchar_t buffer[768] = {};
    _snwprintf_s(
        buffer,
        _countof(buffer),
        _TRUNCATE,
        L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s\r\n",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        message);

    DWORD bytesToWrite = static_cast<DWORD>(wcslen(buffer) * sizeof(wchar_t));
    DWORD bytesWritten = 0;
    WriteFile(hFile, buffer, bytesToWrite, &bytesWritten, nullptr);
    CloseHandle(hFile);
}

// This runs before wmain (global initialization).
// If this marker does not appear when SCM starts the service, the process is stuck before global init.
struct PreWmainMarker
{
    PreWmainMarker()
    {
        std::wstring logDir = GetLogsDirectory();
        std::wstring path = logDir + L"\\pre_wmain.txt";

        HANDLE hFile = CreateFileW(
            path.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (hFile == INVALID_HANDLE_VALUE)
        {
            return;
        }

        SYSTEMTIME st = {};
        GetLocalTime(&st);

        wchar_t buffer[256] = {};
        _snwprintf_s(
            buffer,
            _countof(buffer),
            _TRUNCATE,
            L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] pre-wmain global init reached\r\n",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

        DWORD bytesToWrite = static_cast<DWORD>(wcslen(buffer) * sizeof(wchar_t));
        DWORD bytesWritten = 0;
        WriteFile(hFile, buffer, bytesToWrite, &bytesWritten, nullptr);
        CloseHandle(hFile);
    }
};

static PreWmainMarker g_preWmainMarker;

// Console mode: Run data collection once and exit
int RunConsoleMode()
{
    LogError("[+] Running in CONSOLE mode");
    LogError("[!] IMPORTANT: This application must run as SYSTEM account to enumerate all user profiles.");
    LogError("[!] Running as Administrator is NOT sufficient.");
    LogError("[!] Use PsExec or Task Scheduler to run as SYSTEM:");
    LogError("[!]   psexec -s -i Spectra.exe /console");
    
    std::string jsonData = GenerateJSON();
    WriteJSONToFile(jsonData);
    
    std::string processData = GenerateProcessJSON();
    WriteJSONToFile(processData, L"processes.json");

    std::string msptData = GenerateMsptInventoryJSON();
    WriteJSONToFile(msptData, L"mspt_inventory.json");
    
    LogError("[+] Console mode execution completed");
    return 0;
}

// Test mode: Run data collection once and display summary
int RunTestMode()
{
    LogError("[+] Running in TEST mode");
    LogError("[+] Performing single data collection for validation...");
    
    std::string jsonData = GenerateJSON();
    std::string processData = GenerateProcessJSON();
    std::string msptData = GenerateMsptInventoryJSON();
    
    std::cout << "\n==========================================================\n";
    std::cout << "DATA COLLECTION TEST COMPLETED\n";
    std::cout << "==========================================================\n";
    std::cout << "Inventory JSON Size: " << jsonData.size() << " bytes\n";
    std::cout << "Process JSON Size:   " << processData.size() << " bytes\n";
    std::cout << "MSPT JSON Size:      " << msptData.size() << " bytes\n";
    std::cout << "Output Location: Current Directory\n";
    std::cout << "\nTo install as service, run: Spectra.exe /install\n";
    std::cout << "==========================================================\n";
    
    WriteJSONToFile(jsonData);
    WriteJSONToFile(processData, L"processes.json");
    WriteJSONToFile(msptData, L"mspt_inventory.json");
    return 0;
}

// Display usage information
void ShowUsage()
{
    std::wcout << L"\n==========================================================\n";
    std::wcout << ServiceConfig::SERVICE_DISPLAY_NAME << L" v" << ServiceConfig::VERSION << L"\n";
    std::wcout << L"==========================================================\n\n";
    std::wcout << L"USAGE:\n";
    std::wcout << L"  Spectra.exe /install    - Install service (auto-upgrades if exists)\n";
    std::wcout << L"  Spectra.exe /upgrade    - Upgrade service in-place (preserves state)\n";
    std::wcout << L"  Spectra.exe /uninstall  - Uninstall Windows service\n";
    std::wcout << L"  Spectra.exe /console    - Run in console mode (one-time collection)\n";
    std::wcout << L"  Spectra.exe /test       - Test data collection\n";
    std::wcout << L"  (no arguments)          - Launched by Service Control Manager\n\n";
    std::wcout << L"SECURITY REQUIREMENTS:\n";
    std::wcout << L"  - Requires Administrator privileges for installation\n";
    std::wcout << L"  - Service runs as LocalSystem (NT AUTHORITY\\SYSTEM)\n";
    std::wcout << L"  - Requires SE_BACKUP and SE_RESTORE privileges\n\n";
    std::wcout << L"EXAMPLES:\n";
    std::wcout << L"  Install:   Spectra.exe /install\n";
    std::wcout << L"  Upgrade:   Spectra.exe /upgrade\n";
    std::wcout << L"  Start:     sc start " << ServiceConfig::SERVICE_NAME << L"\n";
    std::wcout << L"  Stop:      sc stop " << ServiceConfig::SERVICE_NAME << L"\n";
    std::wcout << L"  Uninstall: Spectra.exe /uninstall\n\n";
    std::wcout << L"==========================================================\n";
}

int wmain(int argc, wchar_t* argv[])
{
    AppendTrace(L"wmain reached");
    
    // Log argc for debugging
    {
        wchar_t argcMsg[64];
        _snwprintf_s(argcMsg, _countof(argcMsg), _TRUNCATE, L"argc=%d", argc);
        AppendTrace(argcMsg);
    }

#ifndef _WIN64
    // 32-bit build: Block execution on 64-bit Windows
    if (IsRunningUnderWow64())
    {
        MessageBoxW(nullptr,
            L"This 32-bit application is not supported on 64-bit Windows.\n\n"
            L"Please use the 64-bit version of this application.",
            L"Architecture Mismatch",
            MB_OK | MB_ICONERROR);
        LogError("[-] FATAL: 32-bit application attempted to run on 64-bit Windows. Exiting.");
        return 1;
    }
#endif

    // Check Windows version - require Windows 10 or later
    OSVERSIONINFOEXW osvi = {};
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    
    typedef LONG(WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
    auto RtlGetVersion = reinterpret_cast<RtlGetVersionPtr>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    
    if (RtlGetVersion)
    {
        RtlGetVersion(reinterpret_cast<PRTL_OSVERSIONINFOW>(&osvi));
        AppendTrace(L"after_RtlGetVersion");
    }
    
    // Windows 10 is version 10.0
    if (osvi.dwMajorVersion < 10)
    {
        AppendTrace(L"version_check_FAILED");
        std::wstringstream msg;
        msg << L"This application requires Windows 10 or later.\n\n"
            << L"Current Windows version: " << osvi.dwMajorVersion << L"." << osvi.dwMinorVersion << L"\n\n"
            << L"Please run this on Windows 10 or Windows 11.";
        
        MessageBoxW(nullptr, msg.str().c_str(), L"Unsupported Windows Version", MB_OK | MB_ICONERROR);
        
        LogError("[-] FATAL: This application requires Windows 10 or later. Current version: " + 
                 std::to_string(osvi.dwMajorVersion) + "." + std::to_string(osvi.dwMinorVersion));
        return 1;
    }
    
    AppendTrace(L"version_check_PASSED");
    
    LogError("[+] Running on Windows " + std::to_string(osvi.dwMajorVersion) + "." + 
             std::to_string(osvi.dwMinorVersion) + "." + std::to_string(osvi.dwBuildNumber));

    // Parse command line arguments
    if (argc > 1)
    {
        AppendTrace(L"argc_gt_1_branch");
        
        // Log what argv[1] actually is (with NULL check to prevent crash)
        if (argv == nullptr || argv[1] == nullptr)
        {
            AppendTrace(L"argv1_NULL_BUG");
            // argc > 1 but argv[1] is NULL - Windows SCM bug, treat as no arguments
            AppendTrace(L"noargs_service_path");
            LogError("[+] argv[1] is NULL - assuming Service Control Manager launch");
            return ServiceMain::RunService() ? 0 : 1;
        }
        
        {
            wchar_t argvLogMsg[512];
            _snwprintf_s(argvLogMsg, _countof(argvLogMsg), _TRUNCATE, L"argv_1=%s", argv[1]);
            AppendTrace(argvLogMsg);
        }
        
        std::wstring arg = argv[1];
        
        // Convert to lowercase for comparison
        std::transform(arg.begin(), arg.end(), arg.begin(), ::towlower);
        
        // CRITICAL FIX: SCM may pass service name as argv[1]
        // Check if it's the service name and treat as "no arguments"
        if (arg == L"panoptesspectra" || arg == L"\"panoptesspectra\"")
        {
            AppendTrace(L"service_name_passed_falling_through");
            // Fall through to service mode below
        }
        else if (arg == L"/install")
        {
            return ServiceInstaller::InstallService() ? 0 : 1;
        }
        else if (arg == L"/upgrade")
        {
            return ServiceInstaller::UpgradeService() ? 0 : 1;
        }
        else if (arg == L"/uninstall")
        {
            return ServiceInstaller::UninstallService() ? 0 : 1;
        }
        else if (arg == L"/console")
        {
            return RunConsoleMode();
        }
        else if (arg == L"/test")
        {
            return RunTestMode();
        }
        else if (arg == L"/?" || arg == L"-?" || arg == L"--help" || arg == L"/help")
        {
            ShowUsage();
            return 0;
        }
        else
        {
            // Unknown argument - log it and show usage
            wchar_t unknownMsg[512];
            _snwprintf_s(unknownMsg, _countof(unknownMsg), _TRUNCATE, L"unknown_arg=%s", arg.c_str());
            AppendTrace(unknownMsg);
            
            std::wcerr << L"Unknown argument: " << argv[1] << L"\n";
            ShowUsage();
            return 1;
        }
    }

    // No arguments OR service name argument: Run as service
    AppendTrace(L"noargs_service_path");
    LogError("[+] No command-line arguments - assuming Service Control Manager launch");
    return ServiceMain::RunService() ? 0 : 1;
}

