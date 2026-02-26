#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include "../Utils/Utils.h"

// Service tamper protection - restrict who can start/stop/modify the service
class ServiceTamperProtection
{
public:
    // Apply tamper protection to the service during installation
    static bool ApplyTamperProtectionDACL(SC_HANDLE hService);
    
    // Verify tamper protection is still intact (periodic check)
    static bool VerifyTamperProtection(SC_HANDLE hService);
    
    // Remove tamper protection during uninstall (allows clean removal)
    static bool RemoveTamperProtection(SC_HANDLE hService);

private:
    // Create a restrictive DACL for the service
    static PACL CreateServiceDACL();
    
    // Helper: Get SID for a well-known account
    static PSID GetWellKnownSid(WELL_KNOWN_SID_TYPE sidType);
    
    // Helper: Free allocated SID
    static void FreeSid(PSID pSid);
};
