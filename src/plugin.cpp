// SA-MP plugin entry points.
//
// Everything the server calls lands here: Load/Unload, per-script native
// registration, and the ProcessTick pump that delivers callbacks from the
// worker threads onto the main thread.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "amx.h"
#include "amxutil.h"
#include "log.h"
#include "manager.h"
#include "natives.h"
#include "plugincommon.h"

// Baked in at build time from the git tag (see the Makefile and
// CMakeLists.txt). "dev" for a local build with nothing passed in --
// the same convention the audio server uses for its own version.
#ifndef GORADIO_VERSION
	#define GORADIO_VERSION "dev"
#endif

namespace {

using namespace goradio;

std::string Trim(const std::string &in) {
	size_t start = 0;
	size_t end = in.size();
	while (start < end && (in[start] == ' ' || in[start] == '\t' || in[start] == '\r')) {
		++start;
	}
	while (end > start && (in[end - 1] == ' ' || in[end - 1] == '\t' || in[end - 1] == '\r')) {
		--end;
	}
	return in.substr(start, end - start);
}

// Reads the goradio_* settings out of server.cfg. SA-MP gives plugins no
// API for this, so the file is parsed directly -- it's a flat
// "key value" list, with the value running to the end of the line.
bool ReadServerCfg(ManagerConfig *config, bool *debug) {
	std::ifstream file("server.cfg");
	if (!file.is_open()) {
		return false;
	}
	bool found_any = false;
	std::string line;
	while (std::getline(file, line)) {
		line = Trim(line);
		if (line.empty() || line[0] == '#' || line[0] == ';') {
			continue;
		}
		size_t space = line.find_first_of(" \t");
		if (space == std::string::npos) {
			continue;
		}
		std::string key = line.substr(0, space);
		std::string value = Trim(line.substr(space + 1));
		if (key.compare(0, 8, "goradio_") != 0) {
			continue;
		}
		found_any = true;
		if (key == "goradio_url") {
			config->url = value;
		} else if (key == "goradio_token") {
			config->token = value;
		} else if (key == "goradio_poll_interval") {
			config->poll_interval_ms = std::atoi(value.c_str()) * 1000;
		} else if (key == "goradio_workers") {
			config->workers = std::atoi(value.c_str());
		} else if (key == "goradio_timeout") {
			config->timeout_ms = std::atoi(value.c_str());
		} else if (key == "goradio_debug") {
			*debug = std::atoi(value.c_str()) != 0;
		}
	}
	return found_any;
}

} // namespace

PLUGIN_EXPORT unsigned int PLUGIN_CALL Supports() {
	return SUPPORTS_VERSION | SUPPORTS_AMX_NATIVES | SUPPORTS_PROCESS_TICK;
}

PLUGIN_EXPORT bool PLUGIN_CALL Load(void **ppData) {
	LogInit(reinterpret_cast<LogPrintfFn>(ppData[PLUGIN_DATA_LOGPRINTF]), false);

	if (!AmxInit(reinterpret_cast<void **>(ppData[PLUGIN_DATA_AMX_EXPORTS]))) {
		LogError("could not resolve the server's AMX exports -- refusing to load");
		return false;
	}

	HttpGlobalInit();
	LogInfo("goradio " GORADIO_VERSION " loaded" +
	        std::string(TlsSupported() ? " (https supported)" : " (http only)"));

	ManagerConfig config;
	bool debug = false;
	if (ReadServerCfg(&config, &debug)) {
		LogSetDebug(debug);
		if (config.url.empty()) {
			config.url = "http://127.0.0.1:9090";
		}
		if (!config.token.empty()) {
			std::string err;
			if (!Manager::Get().Configure(config, &err)) {
				LogError("server.cfg: " + err);
			}
		} else {
			LogWarn("goradio_token is not set in server.cfg -- call GoRadio_SetServer from your "
			        "script before creating stations");
		}
	} else {
		LogInfo("no goradio_* settings in server.cfg -- call GoRadio_SetServer from your script");
	}
	return true;
}

PLUGIN_EXPORT void PLUGIN_CALL Unload() {
	// Blocks until the worker and stream threads are joined. Each one is
	// woken from its socket rather than waited out, so this is quick even
	// with a station parked on an idle event stream.
	Manager::Get().Shutdown();
	AmxForgetAllScripts();
	HttpGlobalShutdown();
	LogInfo("goradio unloaded");
}

PLUGIN_EXPORT int PLUGIN_CALL AmxLoad(AMX *amx) {
	AmxTrackScript(amx);
	return RegisterNatives(amx);
}

PLUGIN_EXPORT int PLUGIN_CALL AmxUnload(AMX *amx) {
	AmxForgetScript(amx);
	return AMX_ERR_NONE;
}

PLUGIN_EXPORT void PLUGIN_CALL ProcessTick() { DispatchPendingEvents(); }
