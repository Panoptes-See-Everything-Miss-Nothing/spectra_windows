#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "Utils/Utils.h"
#include "Service/ServiceMain.h"
#include "Service/ServiceInstaller.h"
#include "Service/ServiceConfig.h"
#include <iostream>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "advapi32.lib")

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
    
    LogError("[+] Console mode execution completed");
    return 0;
}

// Test mode: Run data collection once and display summary
int RunTestMode()
{
    LogError("[+] Running in TEST mode");
    LogError("[+] Performing single data collection for validation...");
    
    std::string jsonData = GenerateJSON();
    
    std::cout << "\n==========================================================\n";
    std::cout << "DATA COLLECTION TEST COMPLETED\n";
    std::cout << "==========================================================\n";
    std::cout << "JSON Output Size: " << jsonData.size() << " bytes\n";
    std::cout << "Output Location: Current Directory\n";
    std::cout << "\nTo install as service, run: Spectra.exe /install\n";
    std::cout << "==========================================================\n";
    
    WriteJSONToFile(jsonData);
    return 0;
}

// Display usage information
void ShowUsage()
{
    std::wcout << L"\n==========================================================\n";
    std::wcout << ServiceConfig::SERVICE_DISPLAY_NAME << L" v" << ServiceConfig::VERSION << L"\n";
    std::wcout << L"==========================================================\n\n";
    std::wcout << L"USAGE:\n";
    std::wcout << L"  Spectra.exe /install    - Install Windows service\n";
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
    std::wcout << L"  Start:     sc start " << ServiceConfig::SERVICE_NAME << L"\n";
    std::wcout << L"  Stop:      sc stop " << ServiceConfig::SERVICE_NAME << L"\n";
    std::wcout << L"  Uninstall: Spectra.exe /uninstall\n\n";
    std::wcout << L"==========================================================\n";
}

int wmain(int argc, wchar_t* argv[])
{
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
    }
    
    // Windows 10 is version 10.0
    if (osvi.dwMajorVersion < 10)
    {
        std::wstringstream msg;
        msg << L"This application requires Windows 10 or later.\n\n"
            << L"Current Windows version: " << osvi.dwMajorVersion << L"." << osvi.dwMinorVersion << L"\n\n"
            << L"Please run this on Windows 10 or Windows 11.";
        
        MessageBoxW(nullptr, msg.str().c_str(), L"Unsupported Windows Version", MB_OK | MB_ICONERROR);
        
        LogError("[-] FATAL: This application requires Windows 10 or later. Current version: " + 
                 std::to_string(osvi.dwMajorVersion) + "." + std::to_string(osvi.dwMinorVersion));
        return 1;
    }
    
    LogError("[+] Running on Windows " + std::to_string(osvi.dwMajorVersion) + "." + 
             std::to_string(osvi.dwMinorVersion) + "." + std::to_string(osvi.dwBuildNumber));

    // Parse command line arguments
    if (argc > 1)
    {
        std::wstring arg = argv[1];
        
        // Convert to lowercase for comparison
        std::transform(arg.begin(), arg.end(), arg.begin(), ::towlower);
        
        if (arg == L"/install")
        {
            return ServiceInstaller::InstallService() ? 0 : 1;
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
            std::wcerr << L"Unknown argument: " << argv[1] << L"\n";
            ShowUsage();
            return 1;
        }
    }

    // No arguments: Assume we're being launched by SCM as a service
    LogError("[+] No command-line arguments - assuming Service Control Manager launch");
    return ServiceMain::RunService() ? 0 : 1;
}

