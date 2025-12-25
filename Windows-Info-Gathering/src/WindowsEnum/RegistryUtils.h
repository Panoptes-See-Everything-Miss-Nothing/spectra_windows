#pragma once

#include <string>
#include <windows.h>

// Read REG_SZ or REG_EXPAND_SZ registry value safely
std::wstring GetRegistryString(HKEY hKey, const std::wstring& valueName);
