#include "RegistryUtils.h"

std::wstring GetRegistryString(HKEY hKey, const std::wstring& valueName)
{
    if (!hKey) {
        return L"";
    }

    DWORD type = 0;
    DWORD size = 0;
    DWORD actualSize = 0;
    size_t charCount = 0;

    // First call: get size and type
    if (RegQueryValueExW(hKey, valueName.c_str(), nullptr, &type, nullptr, &size) != ERROR_SUCCESS)
        return L"";

    if (type != REG_SZ && type != REG_EXPAND_SZ)
        return L"";

    if (size % sizeof(wchar_t) != 0) {
        LogError("[-] Registry value has invalid size (not wchar_t aligned): " + std::to_string(size));
        return L"";
    }

    charCount = size / sizeof(wchar_t); 
    std::wstring data(charCount, L'\0');

    // Second call: get data
    actualSize = size;
    if (RegQueryValueExW(hKey, valueName.c_str(), nullptr, nullptr, (LPBYTE)data.data(), &actualSize) != ERROR_SUCCESS)
    {
        return L"";
    }

    if (actualSize != size) {
        LogError("[-] Registry value size changed between calls");
        return L"";
    }

    if (!data.empty() && data.back() == L'\0')
        data.pop_back();

    return data;
}
