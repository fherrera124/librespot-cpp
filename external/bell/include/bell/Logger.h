#pragma once

// Standard includes
#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Library includes
#include <fmt/chrono.h>
#include <fmt/color.h>
#include <fmt/core.h>

namespace std {
inline auto format_as(std::error_code err) {
  return err.message();
}
}  // namespace std

namespace bell {

// List of available levels for the BELL_LOG macro
enum class LogLevel { DEBUG, INFO, WARN, ERROR };

class LoggerBackend {
 public:
  LoggerBackend() = default;
  virtual ~LoggerBackend() = default;

  /**
   * @brief Implement this function to log a message to the underlying logger
   *
   * @param level log level
   * @param filename Filename of the caller, cleaned up to only include the basename
   * @param line Line number of the caller
   * @param tag Optional tag to include in the log message
   * @param message Formatted log message
   */
  virtual void log(LogLevel level, const std::string& filename, int line,
                   const std::string& tag, const std::string& message) = 0;

  /**
   * @brief Set the minimum log level to be logged. Default is LogLevel::DEBUG
   *
   * @param level log level
   */
  inline void setLogLevel(LogLevel level) {
    std::scoped_lock lock(loggerMutex);
    logLevel = level;
  }

 protected:
  LogLevel logLevel = LogLevel::DEBUG;
  std::mutex loggerMutex;
};

class BaseLogger {
 private:
  // List of registered logger backends, eg stdout, file, etc
  std::vector<std::unique_ptr<LoggerBackend>> registeredBackends;

  // Mutex to protect the list of registered loggers
  std::mutex loggerMutex;

 public:
  BaseLogger() = default;
  ~BaseLogger() = default;

  // Return a reference to the singleton logger
  static BaseLogger& instance() {
    static BaseLogger logger;
    return logger;
  }

  /**
   * @brief Register a logger backend to be used for logging
   *
   * @param logger Pointer to the logger backend
   */
  inline void registerBackend(std::unique_ptr<LoggerBackend> logger) {
    std::scoped_lock lock(loggerMutex);
    registeredBackends.push_back(std::move(logger));
  }

  /**
   * @brief Set the minimum log level to be logged in all backends. Default is LogLevel::DEBUG
   *
   * @param level log level
   */
  inline void setLogLevel(LogLevel level) {
    for (auto& backend : registeredBackends) {
      backend->setLogLevel(level);
    }
  }

  template <typename... Args>
  inline void debug(const std::string& filename, int line,
                    const std::string& tag, const std::string& format,
                    Args&&... args) {
    return log(LogLevel::DEBUG, filename, line, tag, format,
               std::forward<Args>(args)...);
  }

  template <typename... Args>
  inline void error(const std::string& filename, int line,
                    const std::string& tag, const std::string& format,
                    Args&&... args) {
    return log(LogLevel::ERROR, filename, line, tag, format,
               std::forward<Args>(args)...);
  }

  template <typename... Args>
  inline void info(const std::string& filename, int line,
                   const std::string& tag, const std::string& format,
                   Args&&... args) {
    return log(LogLevel::INFO, filename, line, tag, format,
               std::forward<Args>(args)...);
  }

  template <typename... Args>
  inline void warn(const std::string& filename, int line,
                   const std::string& tag, const std::string& format,
                   Args&&... args) {
    return log(LogLevel::WARN, filename, line, tag, format,
               std::forward<Args>(args)...);
  }

  template <typename... Args>
  inline void log(LogLevel level, const std::string& filename, int line,
                  const std::string& tag, const std::string& format,
                  Args&&... args) {
    const std::string msg =
        fmt::format(fmt::runtime(format), std::forward<Args>(args)...);
#ifdef _WIN32
    const std::string basenameStr = filename.substr(filename.rfind('\\') + 1);
#else
    const std::string basenameStr = filename.substr(filename.rfind('/') + 1);
#endif

    std::scoped_lock lock(loggerMutex);
    // Log to all registered backends
    for (auto& backend : registeredBackends) {
      backend->log(level, basenameStr, line, tag, msg);
    }
  }
};

/**
 * @brief Logger backend that logs to stdout, with color coding for different log levels. Used as the default logger.
 */
class StdoutLoggerBackend : public bell::LoggerBackend {
 public:
  StdoutLoggerBackend(bool includeTags, bool logFullTimestamp);

  void log(LogLevel level, const std::string& filename, int line,
           const std::string& tag, const std::string& message) override;

 private:
  bool includeTags;
  bool logFullTimestamp;

  static constexpr const char* colorReset = "\033[0m";

  static constexpr std::array<uint8_t, 15> allColors = {
      31, 32, 33, 34, 35, 36, 37, 90, 91, 92, 93, 94, 95, 96, 97};
};

/**
 * @brief Registers the Stdout logger
 *
 * @param includeTags whether to include the tags as part of the log message
 * @param logFullTimestamp whether to format the timestamp as local time since start, or full system time
 */
void registerDefaultLogger(bool includeTags = false,
                           bool logFullTimestamp = false,
                           LogLevel level = LogLevel::DEBUG);

/**
 * @brief Registers a logger implementation. Multiple loggers can be used at the same time.
 */
void registerLoggerBackend(std::unique_ptr<bell::LoggerBackend> logger);

/**
 * @brief Set the minimum log level to be logged. Default is LogLevel::DEBUG
 *
 * @remark This can also be set per logger backend
 * @param level log level
 */
void setLogLevel(LogLevel level);
}  // namespace bell

#define BELL_LOG(type, ...)                                               \
  do {                                                                    \
    bell::BaseLogger::instance().type(__FILE__, __LINE__, ##__VA_ARGS__); \
  } while (0)
