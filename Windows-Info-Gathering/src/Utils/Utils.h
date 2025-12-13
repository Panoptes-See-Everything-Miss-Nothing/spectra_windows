#pragma once

#include <string>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <iomanip>
#include <mutex>

// Logs a wide (UTF-16) string as UTF-8 with a text prefix
void LogWideStringAsUtf8(const std::string& prefix, const std::wstring& value);
#include <windows.h>
#include <sstream>

// UTF-8 JSON ESCAPE
std::string JsonEscape(const std::wstring& input);

void WriteJSONToFile(const std::string& jsonData, const std::wstring& filename = L"inventory.json");

// Helper function for logging errors
// Thread-safe logging to "spectra_log.txt" in the current directory
void LogError(const std::string& message);
