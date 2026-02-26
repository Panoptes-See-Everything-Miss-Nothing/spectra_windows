#pragma once

#include "./Utils/Utils.h"
#include <winreg.h>

// Read REG_SZ or REG_EXPAND_SZ registry value safely
std::wstring GetRegistryString(HKEY hKey, const std::wstring& valueName);
