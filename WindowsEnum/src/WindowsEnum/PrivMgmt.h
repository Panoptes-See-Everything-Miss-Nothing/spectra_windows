#pragma once

// Minimize Windows API surface area
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <string>
#include <windows.h>
#include "../Utils/Utils.h"

// Link required libraries
#pragma comment(lib, "Advapi32.lib")  // For registry and privilege APIs

// Enable a specific privilege for the current process
bool EnablePrivilege(const std::wstring& privilegeName);

// Disable a specific privilege for the current process
bool DisablePrivilege(const std::wstring& privilegeName);

