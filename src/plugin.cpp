// SA-MP plugin entry points.
//
// Everything the server calls lands here: Load/Unload, per-script native
// registration, and the ProcessTick pump that delivers callbacks from the
// worker threads onto the main thread.
#include <string>

#include "amx.h"
#include "amxutil.h"
#include "log.h"
#include "manager.h"
#include "natives.h"
#include "plugincommon.h"
#include "serverconfig.h"

// Baked in at build time from the git tag (see the Makefile and
// CMakeLists.txt). "dev" for a local build with nothing passed in --
// the same convention the audio server uses for its own version.
#ifndef GORADIO_VERSION
	#define GORADIO_VERSION "dev"
#endif

// The entry points below are extern "C" at global scope, so they cannot
// be inside the namespace and would otherwise have to qualify every call.
using namespace goradio;

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
