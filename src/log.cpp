#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <vector>

namespace goradio {
namespace {

LogPrintfFn g_logprintf = 0;
bool g_debug = false;
std::mutex g_mutex;

void Emit(const char *level, const std::string &msg) {
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_logprintf != 0) {
		// Pass the text as an argument rather than as the format string --
		// a slug or a track title containing a '%' would otherwise be
		// interpreted as a conversion specifier.
		g_logprintf("[goradio]%s %s", level, msg.c_str());
	} else {
		std::fprintf(stderr, "[goradio]%s %s\n", level, msg.c_str());
	}
}

} // namespace

void LogInit(LogPrintfFn fn, bool debug) {
	std::lock_guard<std::mutex> lock(g_mutex);
	g_logprintf = fn;
	g_debug = debug;
}

void LogSetDebug(bool debug) {
	std::lock_guard<std::mutex> lock(g_mutex);
	g_debug = debug;
}

bool LogDebugEnabled() {
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_debug;
}

void LogInfo(const std::string &msg) { Emit("", msg); }
void LogWarn(const std::string &msg) { Emit(" WARN:", msg); }
void LogError(const std::string &msg) { Emit(" ERROR:", msg); }

void LogDebug(const std::string &msg) {
	if (!LogDebugEnabled()) {
		return;
	}
	Emit(" debug:", msg);
}

std::string Format(const char *fmt, ...) {
	std::vector<char> buf(256);
	for (;;) {
		va_list args;
		va_start(args, fmt);
		int n = std::vsnprintf(&buf[0], buf.size(), fmt, args);
		va_end(args);
		if (n < 0) {
			return std::string();
		}
		if (static_cast<size_t>(n) < buf.size()) {
			return std::string(&buf[0], static_cast<size_t>(n));
		}
		buf.resize(static_cast<size_t>(n) + 1);
	}
}

} // namespace goradio
