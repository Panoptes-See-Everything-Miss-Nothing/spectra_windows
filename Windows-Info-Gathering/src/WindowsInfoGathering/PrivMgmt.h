#pragma once

// Minimize Windows API surface area
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "../Utils/Utils.h"

// Link required libraries
#pragma comment(lib, "Advapi32.lib")  // For registry and privilege APIs
