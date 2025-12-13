#include "Utils.h"

namespace fs = std::filesystem;

// Static mutex for thread-safe logging
static std::mutex g_logMutex;

void LogError(const std::string& message)
{
    struct tm timeinfo;
    // Lock for thread safety
    std::lock_guard<std::mutex> lock(g_logMutex);
    fs::path logPath = fs::current_path() / "spectra_log.txt";
    
    try {
        std::ofstream logFile(logPath, std::ios::app);
        
        if (!logFile.is_open()) {
            // Fail gracefully - write to stderr if we can't open log
            std::cerr << "ERROR: Could not open log file: " << logPath << std::endl;
            return;
        }

        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        
        if (localtime_s(&timeinfo, &time) != 0) {
            logFile << "[TIMESTAMP_ERROR] " << message << std::endl;
        } else {
            logFile << "[" << std::put_time(&timeinfo, "%Y-%m-%d %H:%M:%S") << "] " << message << std::endl;
        }
        
        logFile.close();
    }
    catch (const std::exception& e) {
        std::cerr << "ERROR: Exception in LogError(): " << e.what() << std::endl;
    }
    catch (...) {
        std::cerr << "ERROR: Unknown exception in LogError()" << std::endl;
    }
}

void LogWideStringAsUtf8(const std::string& prefix, const std::wstring& value)
{
    // Convert wstring to UTF-8 for logging
    if (value.empty()) {
        LogError(prefix + "<empty>");
        return;
    }

    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (sizeNeeded <= 0) {
        LogError(prefix + "<conversion failed>");
        return;
    }

    std::string utf8(static_cast<size_t>(sizeNeeded - 1), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, utf8.data(), sizeNeeded, nullptr, nullptr) <= 0) {
        LogError(prefix + "<conversion failed>");
        return;
    }

    LogError(prefix + utf8);
}

// UTF-8 JSON ESCAPE
std::string JsonEscape(const std::wstring& input)
{
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, input.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (sizeNeeded <= 0) return "\"\"";

    std::string utf8(sizeNeeded - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.c_str(), -1, utf8.data(), sizeNeeded, nullptr, nullptr);

    std::ostringstream out;
    out << "\"";

    for (char c : utf8)
    {
        switch (c)
        {
        case '\"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default: out << c; break;
        }
    }

    out << "\"";
    return out.str();
}

void WriteJSONToFile(const std::string& jsonData, const std::wstring& filename)
{
    std::filesystem::path outputPath = std::filesystem::current_path() / filename;

    std::ofstream outFile(outputPath, std::ios::out | std::ios::trunc);
    if (outFile.is_open()) {
        outFile << jsonData;
        outFile.close();
        std::wcout << L"JSON written to: " << outputPath << std::endl;
    }
    else {
        std::cerr << "Failed to open file for writing." << std::endl;
    }
}

