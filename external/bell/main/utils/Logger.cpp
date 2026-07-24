#include "bell/Logger.h"

using namespace bell;

StdoutLoggerBackend::StdoutLoggerBackend(bool includeTags,
                                         bool logFullTimestamp)
    : includeTags(includeTags), logFullTimestamp(logFullTimestamp) {}

void StdoutLoggerBackend::log(LogLevel level, const std::string& filename,
                              int line, const std::string& tag,
                              const std::string& message) {
  std::scoped_lock lock(loggerMutex);
  if (level < logLevel) {
    return;  // Skip this log message
  }

  auto now = std::chrono::system_clock::now();
  std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
  int nowMs = (std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()) %
               1000)
                  .count();

  if (logFullTimestamp) {
    fmt::print("[{:%Y-%m-%d %H:%M:%S}.{:03}] ", *std::localtime(&nowTime),
               nowMs);
  } else {
    fmt::print("[{:%H:%M:%S}.{:03}] ", *std::localtime(&nowTime), nowMs);
  }

  // Print the log level indicator
  switch (level) {
    case LogLevel::DEBUG:
      fmt::print(fg(fmt::color::dark_gray), "D ");
      break;
    case LogLevel::INFO:
      fmt::print(fg(fmt::color::blue), "I ");
      break;
    case LogLevel::WARN:
      fmt::print(fg(fmt::color::yellow), "W ");
      break;
    case LogLevel::ERROR:
      fmt::print(fg(fmt::color::red), "E ");
      break;
  }

  // Print the tag if requested
  if (includeTags) {
    fmt::print("[{}] ", tag);
  }

  // Calculate a color based on the filename
  unsigned long hash = 5381;
  for (char const& c : filename) {
    hash = ((hash << 5) + hash) + c;  // hash * 33 + c
  }

  fmt::print("\033[0;{}m", allColors[hash % allColors.size()]);
  fmt::print("{}", filename);
  fmt::print(colorReset);

  // Print the line number
  fmt::print(":{}: ", line);

  // Print the message
  if (level == LogLevel::ERROR) {
    fmt::print(fg(fmt::color::red), "{}\n", message);
  } else if (level == LogLevel::WARN) {
    fmt::print(fg(fmt::color::yellow), "{}\n", message);
  } else {
    fmt::print("{}\n", message);
  }
}

void bell::registerLoggerBackend(std::unique_ptr<bell::LoggerBackend> logger) {
  BaseLogger::instance().registerBackend(std::move(logger));
}

void bell::registerDefaultLogger(bool includeTags, bool logFullTimestamp,
                                 LogLevel logLevel) {
  auto logger = std::make_unique<bell::StdoutLoggerBackend>(includeTags,
                                                            logFullTimestamp);
  logger->setLogLevel(logLevel);  // Set the default log level

  // Register the stdout logger
  registerLoggerBackend(std::move(logger));
}

void bell::setLogLevel(LogLevel level) {
  // Set the log level for all backends
  BaseLogger::instance().setLogLevel(level);
}
