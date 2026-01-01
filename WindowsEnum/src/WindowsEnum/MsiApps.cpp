#define WIN32_LEAN_AND_MEAN
#include "MsiApps.h"
#include <Msi.h>
#include <sstream>
#include <array>

// Link MSI library
#pragma comment(lib, "Msi.lib")

namespace {
    // RAII wrapper for MSI handles
    class MsiHandleGuard
    {
    public:
        explicit MsiHandleGuard(MSIHANDLE handle) : m_handle(handle) {}
        
        ~MsiHandleGuard()
        {
            if (m_handle)
            {
                MsiCloseHandle(m_handle);
            }
        }

        // Delete copy operations
        MsiHandleGuard(const MsiHandleGuard&) = delete;
        MsiHandleGuard& operator=(const MsiHandleGuard&) = delete;

        // Allow move operations
        MsiHandleGuard(MsiHandleGuard&& other) noexcept : m_handle(other.m_handle)
        {
            other.m_handle = 0;
        }

        MsiHandleGuard& operator=(MsiHandleGuard&& other) noexcept
        {
            if (this != &other)
            {
                if (m_handle)
                {
                    MsiCloseHandle(m_handle);
                }
                m_handle = other.m_handle;
                other.m_handle = 0;
            }
            return *this;
        }

        MSIHANDLE Get() const { return m_handle; }
        operator bool() const { return m_handle != 0; }

    private:
        MSIHANDLE m_handle;
    };

    // Helper: Get MSI property value with proper buffer management
    std::wstring GetMsiProperty(const std::wstring& productCode, const std::wstring& property)
    {
        // First call to get required buffer size
        DWORD bufferSize = 0;
        UINT result = MsiGetProductInfoW(productCode.c_str(), property.c_str(), nullptr, &bufferSize);
        
        if (result != ERROR_SUCCESS && result != ERROR_MORE_DATA)
        {
            return L"";
        }

        if (bufferSize == 0)
        {
            return L"";
        }

        // Allocate buffer (bufferSize doesn't include null terminator)
        std::wstring buffer(bufferSize, L'\0');
        
        // Second call to get actual data
        result = MsiGetProductInfoW(productCode.c_str(), property.c_str(), &buffer[0], &bufferSize);
        
        if (result != ERROR_SUCCESS)
        {
            return L"";
        }

        // Resize to actual size (remove null terminator padding)
        buffer.resize(bufferSize);
        
        return buffer;
    }

    // Helper: Convert INSTALLSTATE to string
    std::wstring InstallStateToString(INSTALLSTATE state)
    {
        switch (state)
        {
        case INSTALLSTATE_DEFAULT:      return L"Default";
        case INSTALLSTATE_ADVERTISED:   return L"Advertised";
        case INSTALLSTATE_ABSENT:       return L"Absent";
        case INSTALLSTATE_LOCAL:        return L"Local";
        case INSTALLSTATE_SOURCE:       return L"Source";
        case INSTALLSTATE_UNKNOWN:      return L"Unknown";
        default:                        return L"Invalid";
        }
    }

    // Helper: Enumerate MSI products for specific context
    std::vector<MsiApp> EnumerateMsiProductsInternal(const wchar_t* userSid, MSIINSTALLCONTEXT context)
    {
        std::vector<MsiApp> apps;
        DWORD index = 0;
        constexpr DWORD GUID_BUFFER_SIZE = 39; // GUID string is 38 chars + null terminator
        std::array<WCHAR, GUID_BUFFER_SIZE> productCode;

        while (true)
        {
            UINT result = MsiEnumProductsExW(
                nullptr,        // All products
                userSid,        // User SID or NULL for all users
                context,        // Install context
                index,          // Index
                productCode.data(),
                nullptr,        // Installed context (output, not needed)
                nullptr,        // User SID (output, not needed)
                nullptr         // SID length (output, not needed)
            );

            if (result == ERROR_NO_MORE_ITEMS)
            {
                break; // Done enumerating
            }

            if (result != ERROR_SUCCESS)
            {
                LogError("[-] MsiEnumProductsEx failed at index " + std::to_string(index) + 
                         ", error: " + std::to_string(result));
                break;
            }

            try
            {
                MsiApp app{};
                app.productCode = productCode.data();

                // Get product state
                app.installState = MsiQueryProductStateW(app.productCode.c_str());

                // Only include installed products
                if (app.installState == INSTALLSTATE_DEFAULT || 
                    app.installState == INSTALLSTATE_LOCAL ||
                    app.installState == INSTALLSTATE_SOURCE)
                {
                    // Get product properties
                    app.productName = GetMsiProperty(app.productCode, INSTALLPROPERTY_PRODUCTNAME);
                    app.productVersion = GetMsiProperty(app.productCode, INSTALLPROPERTY_VERSIONSTRING);
                    app.publisher = GetMsiProperty(app.productCode, INSTALLPROPERTY_PUBLISHER);
                    app.installDate = GetMsiProperty(app.productCode, INSTALLPROPERTY_INSTALLDATE);
                    app.installLocation = GetMsiProperty(app.productCode, INSTALLPROPERTY_INSTALLLOCATION);
                    app.installSource = GetMsiProperty(app.productCode, INSTALLPROPERTY_INSTALLSOURCE);
                    app.packageCode = GetMsiProperty(app.productCode, INSTALLPROPERTY_PACKAGECODE);
                    app.language = GetMsiProperty(app.productCode, INSTALLPROPERTY_LANGUAGE);

                    // Set assignment type based on context
                    if (context == MSIINSTALLCONTEXT_MACHINE)
                    {
                        app.assignmentType = L"Per-Machine";
                    }
                    else if (context == MSIINSTALLCONTEXT_USERUNMANAGED || context == MSIINSTALLCONTEXT_USERMANAGED)
                    {
                        app.assignmentType = L"Per-User";
                    }
                    else
                    {
                        app.assignmentType = L"Unknown";
                    }

                    // Only add if we have at least a name
                    if (!app.productName.empty())
                    {
                        apps.push_back(std::move(app));
                    }
                }
            }
            catch (const std::exception& ex)
            {
                LogError("[-] Exception processing MSI product: " + std::string(ex.what()));
            }

            index++;
        }

        return apps;
    }
}

std::vector<MsiApp> EnumerateMsiProducts()
{
    std::vector<MsiApp> allApps;
    
    LogError("[+] Enumerating MSI-installed products...");

    try
    {
        // Enumerate per-machine products
        auto machineApps = EnumerateMsiProductsInternal(nullptr, MSIINSTALLCONTEXT_MACHINE);
        LogError("[+] Found " + std::to_string(machineApps.size()) + " per-machine MSI products");
        allApps.insert(allApps.end(), 
                      std::make_move_iterator(machineApps.begin()), 
                      std::make_move_iterator(machineApps.end()));

        // Enumerate per-user products for current user
        auto userApps = EnumerateMsiProductsInternal(nullptr, MSIINSTALLCONTEXT_USERUNMANAGED);
        LogError("[+] Found " + std::to_string(userApps.size()) + " per-user MSI products (unmanaged)");
        allApps.insert(allApps.end(), 
                      std::make_move_iterator(userApps.begin()), 
                      std::make_move_iterator(userApps.end()));

        // Enumerate managed per-user products
        auto managedApps = EnumerateMsiProductsInternal(nullptr, MSIINSTALLCONTEXT_USERMANAGED);
        LogError("[+] Found " + std::to_string(managedApps.size()) + " per-user MSI products (managed)");
        allApps.insert(allApps.end(), 
                      std::make_move_iterator(managedApps.begin()), 
                      std::make_move_iterator(managedApps.end()));

        LogError("[+] Total MSI products enumerated: " + std::to_string(allApps.size()));
    }
    catch (const std::exception& ex)
    {
        LogError("[-] Exception during MSI enumeration: " + std::string(ex.what()));
    }

    return allApps;
}

std::vector<MsiApp> EnumerateMsiProductsForUser(const std::wstring& userSid)
{
    std::vector<MsiApp> allApps;
    
    std::string userDisplay = WideToUtf8(userSid);
    LogError("[+] Enumerating MSI-installed products for user: " + userDisplay);

    try
    {
        // Enumerate per-user products for specific user
        auto userApps = EnumerateMsiProductsInternal(userSid.c_str(), MSIINSTALLCONTEXT_USERUNMANAGED);
        LogError("[+] Found " + std::to_string(userApps.size()) + " per-user MSI products (unmanaged) for: " + userDisplay);
        allApps.insert(allApps.end(), 
                      std::make_move_iterator(userApps.begin()), 
                      std::make_move_iterator(userApps.end()));

        // Enumerate managed per-user products for specific user
        auto managedApps = EnumerateMsiProductsInternal(userSid.c_str(), MSIINSTALLCONTEXT_USERMANAGED);
        LogError("[+] Found " + std::to_string(managedApps.size()) + " per-user MSI products (managed) for: " + userDisplay);
        allApps.insert(allApps.end(), 
                      std::make_move_iterator(managedApps.begin()), 
                      std::make_move_iterator(managedApps.end()));

        LogError("[+] Total MSI products for user " + userDisplay + ": " + std::to_string(allApps.size()));
    }
    catch (const std::exception& ex)
    {
        LogError("[-] Exception during MSI enumeration for user " + userDisplay + ": " + std::string(ex.what()));
    }

    return allApps;
}
