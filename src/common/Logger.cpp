#include "common/Logger.h"

#include <cstdarg>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <sys/time.h>

namespace dms {

Logger& Logger::getInstance() {
    static Logger instance;
    return instance;
}

Logger::Logger() : m_level(LogLevel::INFO), m_useFile(false) {}

Logger::~Logger() {
    shutdown();
}

void Logger::setLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_level = level;
}

bool Logger::setOutputFile(const std::string& filename) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_fileStream.is_open()) {
        m_fileStream.close();
    }
    m_fileStream.clear();
    if (filename.empty()) {
        m_useFile = false;
        return true;
    }
    m_fileStream.open(filename, std::ios::app);
    m_useFile = m_fileStream.is_open();
    return m_useFile;
}

void Logger::debug(const char* format, ...) {
    va_list args;
    va_start(args, format);
    log(LogLevel::DEBUG, format, args);
    va_end(args);
}

void Logger::info(const char* format, ...) {
    va_list args;
    va_start(args, format);
    log(LogLevel::INFO, format, args);
    va_end(args);
}

void Logger::warning(const char* format, ...) {
    va_list args;
    va_start(args, format);
    log(LogLevel::WARNING, format, args);
    va_end(args);
}

void Logger::error(const char* format, ...) {
    va_list args;
    va_start(args, format);
    log(LogLevel::ERROR, format, args);
    va_end(args);
}

void Logger::shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_fileStream.is_open()) {
        m_fileStream.close();
    }
    m_useFile = false;
}

void Logger::log(LogLevel level, const char* format, va_list args) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (level < m_level || format == nullptr) {
        return;
    }

    char buffer[4096];
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    std::ostringstream output;
    output << '[' << getCurrentTimeString() << "] [" << getLevelString(level) << "] " << buffer;

    if (m_useFile && m_fileStream.is_open()) {
        m_fileStream << output.str() << std::endl;
    } else {
        std::cerr << output.str() << std::endl;
    }
}

const char* Logger::getLevelString(LogLevel level) const {
    switch (level) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO: return "INFO";
        case LogLevel::WARNING: return "WARN";
        case LogLevel::ERROR: return "ERROR";
    }
    return "UNKNOWN";
}

std::string Logger::getCurrentTimeString() const {
    struct timeval value {};
    gettimeofday(&value, nullptr);
    const time_t seconds = value.tv_sec;
    struct tm localTime {};
    localtime_r(&seconds, &localTime);
    char timeBuffer[64] {};
    strftime(timeBuffer, sizeof(timeBuffer), "%Y-%m-%d %H:%M:%S", &localTime);
    std::ostringstream output;
    output << timeBuffer << '.' << std::setfill('0') << std::setw(3) << (value.tv_usec / 1000);
    return output.str();
}

} // namespace dms
