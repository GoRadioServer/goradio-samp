#ifndef GORADIO_LOG_H
#define GORADIO_LOG_H

#include <string>

namespace goradio {

// The server's logprintf, captured in Load(). Safe to call from any
// thread: it is a plain fprintf to the server log under the hood, and the
// worker threads use it for connection/retry reporting that would
// otherwise be invisible until something else surfaced it.
typedef void (*LogPrintfFn)(const char *format, ...);

void LogInit(LogPrintfFn fn, bool debug);
void LogSetDebug(bool debug);
bool LogDebugEnabled();

void LogInfo(const std::string &msg);
void LogWarn(const std::string &msg);
void LogError(const std::string &msg);
void LogDebug(const std::string &msg);

// printf-style helper for building the one-off messages above.
std::string Format(const char *fmt, ...);

} // namespace goradio

#endif // GORADIO_LOG_H
