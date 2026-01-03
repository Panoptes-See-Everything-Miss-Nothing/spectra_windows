#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "Utils/Utils.h"

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "advapi32.lib")  // For registry and privilege APIs (lowercase)

// Helper: Check if running 32-bit app on 64-bit Windows
bool IsWow64Process()
{
#ifdef _WIN64
    // 64-bit build always returns false (can't be WOW64)
    return false;
#else
    // 32-bit build: check if running under WOW64
    typedef BOOL (WINAPI *LPFN_ISWOW64PROCESS)(HANDLE, PBOOL);
    LPFN_ISWOW64PROCESS fnIsWow64Process = (LPFN_ISWOW64PROCESS)GetProcAddress(
        GetModuleHandleW(L"kernel32"), "IsWow64Process");
    
    if (fnIsWow64Process != nullptr)
    {
        BOOL isWow64 = FALSE;
        if (fnIsWow64Process(GetCurrentProcess(), &isWow64))
        {
            return isWow64 == TRUE;
        }
    }
    return false;
#endif
}

int main()
{
#ifndef _WIN64
    // 32-bit build: Block execution on 64-bit Windows
    if (IsWow64Process())
    {
        MessageBoxW(nullptr,
            L"This 32-bit application is not supported on 64-bit Windows.\n\n"
            L"Please use the 64-bit version of this application.",
            L"Architecture Mismatch",
            MB_OK | MB_ICONERROR);
        LogError("[-] FATAL: 32-bit application attempted to run on 64-bit Windows. Exiting.");
        return 1;  // Exit with error code
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

    LogError("[!] IMPORTANT: This application must run as SYSTEM account to enumerate all user profiles.");
    LogError("[!] Running as Administrator is NOT sufficient.");
    LogError("[!] Use PsExec or Task Scheduler to run as SYSTEM:");
    LogError("[!]   psexec -s -i Windows-Info-Gathering.exe");
    LogError("[!]   OR create a scheduled task with 'Run whether user is logged on or not' + 'Run with highest privileges'");
    
    std::string jsonData = GenerateJSON();
    WriteJSONToFile(jsonData); 
    return 0;
}

