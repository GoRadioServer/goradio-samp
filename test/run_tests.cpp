// Host-side tests for everything below the AMX layer: URL parsing, the
// protobuf-JSON quirks, and a full Manager session driven against
// test/fake_audioserver.py.
//
// Build and run with test/run_tests.sh -- it starts the fake server and
// passes its URL in as argv[1].
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "json.h"
#include "log.h"
#include "manager.h"

using namespace goradio;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const std::string &what) {
	++g_checks;
	if (ok) {
		std::printf("  ok   %s\n", what.c_str());
	} else {
		++g_failures;
		std::printf("  FAIL %s\n", what.c_str());
	}
}

void CheckEq(const std::string &got, const std::string &want, const std::string &what) {
	Check(got == want, what + " (got \"" + got + "\", want \"" + want + "\")");
}

void CheckEqInt(long long got, long long want, const std::string &what) {
	char buf[128];
	std::snprintf(buf, sizeof(buf), "%s (got %lld, want %lld)", what.c_str(), got, want);
	Check(got == want, buf);
}

void TestUrlParsing() {
	std::printf("URL parsing\n");
	Url url;
	std::string err;

	Check(ParseUrl("http://127.0.0.1:9090", &url, &err), "plain http URL parses");
	CheckEq(url.host, "127.0.0.1", "host");
	CheckEqInt(url.port, 9090, "port");
	Check(!url.secure(), "not secure");

	Check(ParseUrl("localhost:9090", &url, &err), "bare host:port parses");
	CheckEq(url.host, "localhost", "bare host");
	CheckEqInt(url.port, 9090, "bare port");

	Check(ParseUrl("https://radio.example.com", &url, &err), "https URL parses");
	CheckEqInt(url.port, 443, "https default port");
	Check(url.secure(), "https is secure");

	Check(ParseUrl("http://radio.example.com/base/path/", &url, &err), "URL with base path");
	CheckEq(url.base_path, "/base/path", "base path, trailing slash trimmed");
	CheckEqInt(url.port, 80, "http default port");

	Check(!ParseUrl("ftp://example.com", &url, &err), "unknown scheme is rejected");
	Check(!ParseUrl("", &url, &err), "empty URL is rejected");
}

void TestJson() {
	std::printf("JSON / protobuf-JSON mapping\n");
	// The three things a hand-written protobuf-JSON client gets wrong.
	const char *doc =
	    "{\"uptimeSeconds\":\"412\",\"queuePosition\":3,\"isRegistered\":true,"
	    "\"title\":\"Intro \\u2014 \\\"quoted\\\"\",\"nested\":{\"a\":{\"b\":[1,2,3]}},"
	    "\"emoji\":\"\\ud83c\\udfb5\"}";
	JsonValue v;
	std::string err;
	Check(JsonValue::Parse(doc, &v, &err), "document parses");
	CheckEqInt(v.Int("uptimeSeconds"), 412, "int64 arrives as a quoted string");
	CheckEqInt(v.Int("queuePosition"), 3, "int32 arrives as a number");
	Check(v.Bool("isRegistered"), "bool field");
	CheckEq(v.Str("title"), "Intro \xe2\x80\x94 \"quoted\"", "escapes and \\u decoding");
	CheckEq(v.Str("emoji"), "\xf0\x9f\x8e\xb5", "surrogate pair becomes one code point");
	CheckEqInt(v.Get("nested").Get("a").Get("b").Size(), 3, "nested array size");
	CheckEqInt(v.Get("nested").Get("a").Get("b").At(1).AsInt(), 2, "nested array element");

	// An omitted field must read as the zero value, not as an error, and
	// a missing sub-message must be safe to walk through.
	CheckEqInt(v.Int("listenerCount", 0), 0, "missing int64 reads as 0");
	CheckEq(v.Get("missing").Get("alsoMissing").Str("stillMissing"), "",
	        "walking a missing sub-message is safe");

	Check(!JsonValue::Parse("{\"a\":", &v, &err), "truncated document is rejected");
	Check(!JsonValue::Parse("", &v, &err), "empty document is rejected");

	JsonObject obj;
	obj.Str("slug", "my\"fm").Int("threshold", 3).Bool("stopCurrent", true).StrIfSet("skipped", "");
	CheckEq(obj.Build(), "{\"slug\":\"my\\\"fm\",\"threshold\":3,\"stopCurrent\":true}",
	        "request body building and escaping");
}

// Pumps the manager until an event of the wanted kind shows up.
bool WaitFor(PawnEventKind kind, int timeout_ms, PawnEvent *out) {
	std::chrono::steady_clock::time_point deadline =
	    std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	static std::vector<PawnEvent> backlog;

	for (;;) {
		for (size_t i = 0; i < backlog.size(); ++i) {
			if (backlog[i].kind == kind) {
				*out = backlog[i];
				backlog.erase(backlog.begin() + static_cast<long>(i));
				return true;
			}
		}
		if (std::chrono::steady_clock::now() > deadline) {
			return false;
		}
		Manager::Get().Tick(&backlog);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}

void TestManagerSession(const std::string &url) {
	std::printf("Manager session against %s\n", url.c_str());
	Manager &mgr = Manager::Get();

	ManagerConfig config;
	config.url = url;
	config.token = "test-token";
	config.poll_interval_ms = 1000;
	config.timeout_ms = 5000;
	config.workers = 2;

	std::string err;
	Check(mgr.Configure(config, &err), "Configure succeeds: " + err);

	PawnEvent ev;
	Check(WaitFor(kEvServerInfo, 5000, &ev), "GetServerInfo answered");
	CheckEq(ev.text, "v9.9.9-fake", "server version");

	// Re-pointing the plugin while nothing is registered retires the
	// worker pool and starts a fresh one, rather than editing a client a
	// worker thread is using.
	Check(mgr.Configure(config, &err), "reconfigure with no stations: " + err);
	Check(WaitFor(kEvServerInfo, 5000, &ev), "the new worker pool answers");

	int station = mgr.CreateStation("testfm", "Test FM", "a test station", 3, "", &err);
	Check(station != GORADIO_INVALID_STATION, "CreateStation returns an id: " + err);

	Check(WaitFor(kEvStationRegistered, 5000, &ev), "station registered");
	CheckEq(ev.text, "http://fake/stream/testfm", "stream URL");
	Check(!ev.flag, "first registration is not a re-registration");
	Check(mgr.IsRegistered(station), "station reports registered");

	// Re-pointing with a station live would strand that registration on
	// the old server, so it is refused.
	Check(!mgr.Configure(config, &err), "reconfigure is refused while a station exists");

	// A duplicate slug is rejected locally rather than quietly hijacking
	// the running station.
	int dup = mgr.CreateStation("testfm", "", "", 0, "", &err);
	Check(dup == GORADIO_INVALID_STATION, "duplicate slug is rejected");
	CheckEqInt(mgr.FindStationBySlug("testfm"), station, "lookup by slug");

	// Events pushed down the subscription.
	Check(WaitFor(kEvTrackStarted, 5000, &ev), "TRACK_STARTED delivered");
	CheckEq(ev.title, "Intro \xe2\x80\x94 live", "track title survived UTF-8");
	CheckEqInt(ev.num1, 212, "track duration");
	CheckEq(ev.queue_id, "queue-1", "track queue id");

	Check(WaitFor(kEvListenerCount, 5000, &ev), "LISTENER_COUNT_CHANGED delivered");
	CheckEqInt(ev.num1, 13, "listener count from event");

	Check(WaitFor(kEvQueueLow, 5000, &ev), "QUEUE_LOW delivered");
	CheckEqInt(ev.num2, 3, "queue low threshold");

	Check(WaitFor(kEvTrackEnded, 5000, &ev), "TRACK_ENDED delivered");
	CheckEq(ev.text, "completed", "track ended reason");

	// These two arrive glued into a single TCP write.
	Check(WaitFor(kEvSilenceStarted, 5000, &ev), "SILENCE_STARTED delivered");
	Check(WaitFor(kEvQueueUpdated, 5000, &ev), "QUEUE_UPDATED delivered (same TCP write)");
	CheckEqInt(ev.num1, 4, "queue length from event");

	// The cached status, as the PAWN getters see it.
	Check(WaitFor(kEvStatusUpdate, 5000, &ev), "status snapshot arrived");
	StationStatus status;
	Check(mgr.GetStatus(station, &status), "GetStatus reads the cache");
	Check(status.valid, "status is valid");
	CheckEqInt(status.listener_count, 7, "listener count from snapshot");
	CheckEqInt(status.uptime_seconds, 412, "uptime (int64-as-string)");
	Check(status.has_current, "a track is playing");
	CheckEq(status.current.title, "Intro \xe2\x80\x94 \"quoted\" & escaped", "current track title");
	CheckEqInt(status.current.duration_seconds, 212, "current track duration");
	CheckEqInt(static_cast<long long>(status.queue.size()), 2, "queue snapshot size");
	CheckEq(status.queue[0].title, "Second", "first pending item");
	CheckEqInt(status.queue[1].duration_seconds, 0, "unknown duration reads as 0");
	CheckEqInt(static_cast<long long>(status.history.size()), 1, "history size");
	CheckEq(status.history[0].reason, "completed", "history reason");
	Check(status.elapsed_seconds >= 30, "elapsed is carried forward from the snapshot");

	// Commands.
	int req = mgr.QueueTrack(station, "songs/next.mp3", "Next Up", "Someone", "", kQueueAppend);
	Check(req > 0, "QueueTrack returns a request id");
	Check(WaitFor(kEvTrackQueued, 5000, &ev), "QueueTrack answered");
	CheckEqInt(ev.request_id, req, "request id matches");
	CheckEq(ev.queue_id, "queue-1", "queue id assigned");

	req = mgr.Skip(station);
	Check(WaitFor(kEvCommandResult, 5000, &ev), "Skip answered");
	CheckEq(ev.text, "Skip", "command name");
	Check(ev.flag, "Skip reports success");

	// "Nothing to do" comes back as a false flag on a 200, not an error.
	mgr.Pause(station);
	Check(WaitFor(kEvCommandResult, 5000, &ev), "Pause answered");
	CheckEq(ev.text, "Pause", "command name");
	Check(!ev.flag, "Pause reports false rather than erroring");

	mgr.RequestStationList();
	Check(WaitFor(kEvStationList, 5000, &ev), "ListStations answered");
	CheckEqInt(ev.num1, 2, "two stations listed");
	std::vector<StationSummary> listed = mgr.ListedStations();
	Check(listed.size() == 2, "listed stations cached");
	if (listed.size() == 2) {
		CheckEq(listed[0].slug, "testfm", "listed slug");
		CheckEqInt(listed[0].listener_count, 42, "listed listener count");
		CheckEqInt(listed[1].listener_count, 0, "omitted listener count reads as 0");
	}

	// The stream ends cleanly a second in; the loop should re-register and
	// resubscribe on its own, and the second registration is flagged as
	// one the server already knew about.
	Check(WaitFor(kEvStationRegistered, 15000, &ev), "re-registered after the stream ended");
	Check(ev.flag, "re-registration is flagged as such");
	Check(ev.num1 != 0, "re-registration is flagged as reconnect-driven");

	// Reloading: what it can and cannot change while a station is live.
	ManagerConfig reload = config;
	std::string reload_err;
	CheckEqInt(mgr.Reload(reload, &reload_err), kReloadApplied,
	           "reloading an unchanged config succeeds as a no-op");

	reload.poll_interval_ms = 2000;
	CheckEqInt(mgr.Reload(reload, &reload_err), kReloadApplied,
	           "the poll interval can change under a live station");

	// Somewhere nothing is listening -- the point is that it is a
	// *different* server, not that it works.
	reload.url = "http://127.0.0.1:1";
	CheckEqInt(mgr.Reload(reload, &reload_err), kReloadPartial,
	           "the audio server cannot be re-pointed under a live station");
	Check(!reload_err.empty(), "a partial reload explains what it skipped");
	Check(mgr.IsRegistered(station), "a partial reload leaves the station alone");

	Check(mgr.DestroyStation(station), "DestroyStation");
	Check(!mgr.IsValidStation(station), "station id is no longer valid");

	// With no stations left there is nothing to strand, so the connection
	// settings apply and the worker pool is rebuilt on them.
	reload.url = config.url;
	reload.workers = 1;
	CheckEqInt(mgr.Reload(reload, &reload_err), kReloadApplied,
	           "with no stations, connection settings apply");
	Check(WaitFor(kEvServerInfo, 5000, &ev), "the rebuilt worker pool answers");

	mgr.Shutdown();
	std::printf("  ok   Shutdown returned\n");
	++g_checks;
}

} // namespace

int main(int argc, char **argv) {
	LogInit(0, false);
	TestUrlParsing();
	TestJson();
	if (argc > 1) {
		TestManagerSession(argv[1]);
	} else {
		std::printf("(no server URL given, skipping the Manager session)\n");
	}
	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
