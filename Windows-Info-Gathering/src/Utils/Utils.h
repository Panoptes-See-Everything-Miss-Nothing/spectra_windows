#pragma once

#include <string>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <iomanip>
#include <mutex>
#include <windows.h>
#include <sstream>

// UTF-8 JSON ESCAPE
std::string JsonEscape(const std::wstring& input);

void WriteJSONToFile(const std::string& jsonData, const std::wstring& filename = L"inventory.json");

// Helper function for logging errors
// Thread-safe logging to "spectra_log.txt" in the current directory
void LogError(const std::string& message);
