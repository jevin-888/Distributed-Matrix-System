#pragma once

#include <cstdarg>
#include <fstream>
#include <mutex>
#include <string>

namespace dms {

enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARNING = 2,
    ERROR = 3
};

class Logger {
public:
    static Logger& getInstance();

    void setLevel(LogLevel level);
    bool setOutputFile(const std::string& filename);

    void debug(const char* format, ...);
    void info(const char* format, ...);
    void warning(const char* format, ...);
    void error(const char* format, ...);
    void shutdown();

private:
    Logger();
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void log(LogLevel level, const char* format, va_list args);
    const char* getLevelString(LogLevel level) const;
    std::string getCurrentTimeString() const;

    LogLevel m_level;
    std::ofstream m_fileStream;
    bool m_useFile;
    std::mutex m_mutex;
};

} // namespace dms

#define LOG_DEBUG(...) dms::Logger::getInstance().debug(__VA_ARGS__)
#define LOG_INFO(...) dms::Logger::getInstance().info(__VA_ARGS__)
#define LOG_WARNING(...) dms::Logger::getInstance().warning(__VA_ARGS__)
#define LOG_ERROR(...) dms::Logger::getInstance().error(__VA_ARGS__)
