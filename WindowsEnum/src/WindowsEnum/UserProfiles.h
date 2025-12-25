#pragma once

#include <string>
#include <vector>
#include <windows.h>

// User profile information
struct UserProfile
{
    std::wstring username;
    std::wstring sid;
    std::wstring profilePath;
    bool isLoaded;  // Is registry hive currently loaded?
};

// Enumerate all user profiles on the system
std::vector<UserProfile> EnumerateUserProfiles();
