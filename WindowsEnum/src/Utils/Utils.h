#pragma once

#include <string>
#include <string_view>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <iomanip>
#include <mutex>
#include <WinSock2.h>    // Must be before windows.h to avoid conflicts
#include <windows.h>     // For HKEY and basic Windows types
#include <sstream>
#include <vector>
#include <array>
#include <map>

namespace fs = std::filesystem;

// Logs a wide (UTF-16) string as UTF-8 with a text prefix
void LogWideStringAsUtf8(const std::string& prefix, const std::wstring& value);

// UTF-8 JSON ESCAPE
std::string JsonEscape(const std::wstring& input);

void WriteJSONToFile(const std::string& jsonData, const std::wstring& filename = L"inventory.json");

// Helper function for logging errors
// Thread-safe logging to "spectra_log.txt" in the current directory
void LogError(const std::string& message);

// Helper: Check if IP is loopback (defined in WindowsInfoGathering.cpp)
bool IsLoopbackIP(const std::string& ip);

// Helper: Convert wstring to UTF-8 string safely
std::string WideToUtf8(const std::wstring& wstr);

// Helper: Check if username is a system account
bool IsSystemAccount(const std::wstring& username);

// Helper: Translate Windows error code to message
std::string GetWindowsErrorMessage(LONG errorCode);

// Directory Security Validation Functions

// Check if a path is a directory (not a file)
bool IsDirectory(const std::wstring& path);

// Check if a path is a reparse point (symlink, junction, etc.)
bool IsReparsePoint(const std::wstring& path);

// Verify that a directory is owned by the current user or SYSTEM
bool IsDirectoryOwnedByTrustedUser(const std::wstring& path);

// Validate that a directory is safe to use (not a symlink, owned by trusted user)
bool IsDirectorySafeToUse(const std::wstring& path);

// Get the current user's SID
PSID GetCurrentUserSid();

// Free a SID allocated by GetCurrentUserSid
void FreeUserSid(PSID pSid);