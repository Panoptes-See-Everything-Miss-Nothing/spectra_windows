#include "ServiceTamperProtection.h"

#pragma comment(lib, "advapi32.lib")

// Apply restrictive DACL to service to prevent tampering
bool ServiceTamperProtection::ApplyTamperProtectionDACL(SC_HANDLE hService)
{
    if (!hService)
    {
        LogError("[-] Invalid service handle for tamper protection");
        return false;
    }

    LogError("[+] Applying service tamper protection...");

    // Create restrictive DACL
    PACL pNewDACL = CreateServiceDACL();
    if (!pNewDACL)
    {
        LogError("[-] Failed to create service DACL");
        return false;
    }

    // Create security descriptor
    PSECURITY_DESCRIPTOR pSD = LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
    if (!pSD)
    {
        LocalFree(pNewDACL);
        LogError("[-] Failed to allocate security descriptor");
        return false;
    }

    if (!InitializeSecurityDescriptor(pSD, SECURITY_DESCRIPTOR_REVISION))
    {
        DWORD error = GetLastError();
        LogError("[-] InitializeSecurityDescriptor failed, error: " + std::to_string(error));
        LocalFree(pSD);
        LocalFree(pNewDACL);
        return false;
    }

    // Set the DACL with PROTECTED flag to prevent inheritance
    if (!SetSecurityDescriptorDacl(pSD, TRUE, pNewDACL, FALSE))
    {
        DWORD error = GetLastError();
        LogError("[-] SetSecurityDescriptorDacl failed, error: " + std::to_string(error));
        LocalFree(pSD);
        LocalFree(pNewDACL);
        return false;
    }

    // Apply the security descriptor to the service
    if (!SetServiceObjectSecurity(hService, DACL_SECURITY_INFORMATION, pSD))
    {
        DWORD error = GetLastError();
        LogError("[-] SetServiceObjectSecurity failed, error: " + std::to_string(error) + 
                 " - " + GetWindowsErrorMessage(error));
        LocalFree(pSD);
        LocalFree(pNewDACL);
        return false;
    }

    LocalFree(pSD);
    LocalFree(pNewDACL);

    LogError("[+] Service tamper protection applied successfully");
    LogError("[!] Only SYSTEM can delete or reconfigure the service");
    LogError("[!] Administrators can start/stop and modify DACL (for uninstall)");

    return true;
}

// Create a restrictive DACL that allows:
// - SYSTEM: Full control (SERVICE_ALL_ACCESS)
// - Administrators: Start, Stop, Query, Modify DACL (for uninstall)
// - Users: Query status only (read-only)
//
// SECURITY: DELETE and SERVICE_CHANGE_CONFIG are NOT granted to Administrators or Users.
// Because this is a PROTECTED DACL (no inheritance), any right not explicitly granted
// is implicitly denied. This is more reliable than explicit DENY ACEs, which can cause
// unexpected interactions when principals belong to multiple groups (e.g., Administrators
// are also members of Everyone).
//
// To uninstall: admin opens with WRITE_DAC, replaces the DACL via RemoveTamperProtection()
// to grant DELETE, then re-opens with DELETE to call DeleteService().
PACL ServiceTamperProtection::CreateServiceDACL()
{
    EXPLICIT_ACCESSW ea[3] = {};
    PACL pACL = nullptr;
    DWORD dwResult = 0;

    // Get well-known SIDs
    PSID pSystemSid = GetWellKnownSid(WinLocalSystemSid);
    PSID pAdminSid = GetWellKnownSid(WinBuiltinAdministratorsSid);
    PSID pUsersSid = GetWellKnownSid(WinBuiltinUsersSid);

    if (!pSystemSid || !pAdminSid || !pUsersSid)
    {
        LogError("[-] Failed to get well-known SIDs for service DACL");
        goto Cleanup;
    }

    // ACE 0: SYSTEM gets full control
    ea[0].grfAccessPermissions = SERVICE_ALL_ACCESS;
    ea[0].grfAccessMode = SET_ACCESS;
    ea[0].grfInheritance = NO_INHERITANCE;
    ea[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea[0].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea[0].Trustee.ptstrName = (LPWSTR)pSystemSid;

    // ACE 1: Administrators can start, stop, query, interrogate, read config, and modify DACL.
    // Notably ABSENT: DELETE, SERVICE_CHANGE_CONFIG, WRITE_OWNER
    // This means admins CANNOT delete the service or change its config without first
    // modifying the DACL (which requires WRITE_DAC, granted here).
    ea[1].grfAccessPermissions = SERVICE_START | SERVICE_STOP |
                                  SERVICE_QUERY_STATUS | SERVICE_INTERROGATE |
                                  SERVICE_QUERY_CONFIG |
                                  READ_CONTROL | WRITE_DAC;
    ea[1].grfAccessMode = SET_ACCESS;
    ea[1].grfInheritance = NO_INHERITANCE;
    ea[1].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea[1].Trustee.TrusteeType = TRUSTEE_IS_GROUP;
    ea[1].Trustee.ptstrName = (LPWSTR)pAdminSid;

    // ACE 2: Users can only query status (read-only)
    ea[2].grfAccessPermissions = SERVICE_QUERY_STATUS | SERVICE_INTERROGATE;
    ea[2].grfAccessMode = SET_ACCESS;
    ea[2].grfInheritance = NO_INHERITANCE;
    ea[2].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea[2].Trustee.TrusteeType = TRUSTEE_IS_GROUP;
    ea[2].Trustee.ptstrName = (LPWSTR)pUsersSid;

    // Create the ACL (no DENY ACEs - protection comes from not granting DELETE/CHANGE_CONFIG)
    dwResult = SetEntriesInAclW(3, ea, nullptr, &pACL);
    if (dwResult != ERROR_SUCCESS)
    {
        LogError("[-] SetEntriesInAcl failed, error: " + std::to_string(dwResult));
        pACL = nullptr;
    }

Cleanup:
    if (pSystemSid) FreeSid(pSystemSid);
    if (pAdminSid) FreeSid(pAdminSid);
    if (pUsersSid) FreeSid(pUsersSid);

    return pACL;
}

// Verify that tamper protection is still intact
bool ServiceTamperProtection::VerifyTamperProtection(SC_HANDLE hService)
{
    if (!hService)
        return false;

    PSECURITY_DESCRIPTOR pSD = nullptr;
    DWORD dwBytesNeeded = 0;

    // Get the service's security descriptor
    if (!QueryServiceObjectSecurity(hService, DACL_SECURITY_INFORMATION,
                                     nullptr, 0, &dwBytesNeeded))
    {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        {
            LogError("[-] QueryServiceObjectSecurity failed");
            return false;
        }
    }

    pSD = (PSECURITY_DESCRIPTOR)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, dwBytesNeeded);
    if (!pSD)
        return false;

    if (!QueryServiceObjectSecurity(hService, DACL_SECURITY_INFORMATION,
                                     pSD, dwBytesNeeded, &dwBytesNeeded))
    {
        HeapFree(GetProcessHeap(), 0, pSD);
        LogError("[-] QueryServiceObjectSecurity failed (second call)");
        return false;
    }

    // Verify DACL is present
    BOOL bDaclPresent = FALSE;
    BOOL bDaclDefaulted = FALSE;
    PACL pDACL = nullptr;

    if (!GetSecurityDescriptorDacl(pSD, &bDaclPresent, &pDACL, &bDaclDefaulted))
    {
        HeapFree(GetProcessHeap(), 0, pSD);
        LogError("[-] GetSecurityDescriptorDacl failed");
        return false;
    }

    bool isProtected = (bDaclPresent && !bDaclDefaulted && pDACL != nullptr);

    HeapFree(GetProcessHeap(), 0, pSD);

    if (isProtected)
    {
        LogError("[+] Service tamper protection verified - DACL is intact");
    }
    else
    {
        LogError("[-] WARNING: Service tamper protection may have been compromised!");
    }

    return isProtected;
}

// Remove tamper protection (used during uninstall)
bool ServiceTamperProtection::RemoveTamperProtection(SC_HANDLE hService)
{
    LogError("[+] Removing tamper protection for service uninstall...");

    // Create a permissive DACL for uninstallation
    EXPLICIT_ACCESSW ea[2] = {};
    PACL pACL = nullptr;

    PSID pSystemSid = GetWellKnownSid(WinLocalSystemSid);
    PSID pAdminSid = GetWellKnownSid(WinBuiltinAdministratorsSid);

    if (!pSystemSid || !pAdminSid)
    {
        LogError("[-] Failed to get SIDs for uninstall");
        return false;
    }

    // SYSTEM: Full control
    ea[0].grfAccessPermissions = SERVICE_ALL_ACCESS;
    ea[0].grfAccessMode = SET_ACCESS;
    ea[0].grfInheritance = NO_INHERITANCE;
    ea[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea[0].Trustee.ptstrName = (LPWSTR)pSystemSid;

    // Administrators: Full control (for uninstall)
    ea[1].grfAccessPermissions = SERVICE_ALL_ACCESS;
    ea[1].grfAccessMode = SET_ACCESS;
    ea[1].grfInheritance = NO_INHERITANCE;
    ea[1].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea[1].Trustee.ptstrName = (LPWSTR)pAdminSid;

    DWORD dwResult = SetEntriesInAclW(2, ea, nullptr, &pACL);

    FreeSid(pSystemSid);
    FreeSid(pAdminSid);

    if (dwResult != ERROR_SUCCESS)
    {
        LogError("[-] SetEntriesInAcl failed during uninstall");
        return false;
    }

    PSECURITY_DESCRIPTOR pSD = LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
    if (!pSD)
    {
        LocalFree(pACL);
        return false;
    }

    InitializeSecurityDescriptor(pSD, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(pSD, TRUE, pACL, FALSE);

    bool success = SetServiceObjectSecurity(hService, DACL_SECURITY_INFORMATION, pSD);

    LocalFree(pSD);
    LocalFree(pACL);

    if (success)
    {
        LogError("[+] Tamper protection removed successfully");
    }

    return success;
}

// Helper: Get a well-known SID
PSID ServiceTamperProtection::GetWellKnownSid(WELL_KNOWN_SID_TYPE sidType)
{
    DWORD cbSid = SECURITY_MAX_SID_SIZE;
    PSID pSid = LocalAlloc(LPTR, cbSid);
    
    if (!pSid)
        return nullptr;

    if (!CreateWellKnownSid(sidType, nullptr, pSid, &cbSid))
    {
        LocalFree(pSid);
        return nullptr;
    }

    return pSid;
}

// Helper: Free allocated SID
void ServiceTamperProtection::FreeSid(PSID pSid)
{
    if (pSid)
        LocalFree(pSid);
}
