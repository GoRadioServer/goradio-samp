#ifndef GORADIO_MANAGER_H
#define GORADIO_MANAGER_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "http.h"
#include "station.h"

namespace goradio {

// What a queued PawnEvent turns into on the PAWN side. The dispatcher in
// natives.cpp maps each one to its forward.
enum PawnEventKind {
	kEvStationRegistered,
	kEvTrackStarted,
	kEvTrackEnded,
	kEvQueueLow,
	kEvQueueUpdated,
	kEvListenerCount,
	kEvSilenceStarted,
	kEvSilenceEnded,
	kEvStatusUpdate,
	kEvTrackQueued,
	kEvCommandResult,
	kEvStationList,
	kEvServerInfo,
	kEvError
};

// A callback waiting to be delivered to PAWN. Built on a worker thread and
// drained by Tick() on the main thread, because a SA-MP public may only be
// called from the server's own thread. Every field is a plain value --
// nothing here points back at state a worker might still be mutating.
struct PawnEvent {
	PawnEventKind kind;
	int station_id;
	int request_id;
	bool flag; // re_registered / command success

	std::string queue_id;
	std::string location;
	std::string title;
	std::string artist;
	std::string cover_art;
	std::string text;  // reason / stream_url / error code / command name / version
	std::string text2; // error message

	long long num1; // duration / queue_length / listener_count / result value
	long long num2; // threshold / queue_position

	PawnEvent()
	    : kind(kEvError), station_id(GORADIO_INVALID_STATION), request_id(0), flag(false), num1(0),
	      num2(0) {}
};

struct ManagerConfig {
	std::string url;
	std::string token;
	int poll_interval_ms;
	int timeout_ms;
	int workers;

	ManagerConfig()
	    : url("http://127.0.0.1:9090"), poll_interval_ms(5000), timeout_ms(10000), workers(2) {}

	// The settings that can't be changed without rebuilding the worker
	// pool, because each worker's connection is built from them once.
	bool ConnectionDiffers(const ManagerConfig &other) const {
		return url != other.url || token != other.token || timeout_ms != other.timeout_ms ||
		       workers != other.workers;
	}
};

// What GoRadio_ReloadConfig managed to do. The numbers are the native's
// return value; goradio.inc has the matching constants.
enum ReloadResult {
	kReloadFailed = 0,
	kReloadApplied = 1,
	// The live settings were applied, but the audio server connection was
	// left alone because stations are registered against it.
	kReloadPartial = 2
};

// Owns every station, the worker threads that talk to the audio server,
// and the queue of callbacks waiting for the main thread. One instance,
// reached through Get().
class Manager {
public:
	static Manager &Get();

	// Points the plugin at an audio server and starts the worker threads.
	// Rejected once stations exist: the workers hold live connections and
	// registrations bound to the old server, and silently re-pointing
	// them would leave stations registered somewhere nothing is watching.
	bool Configure(const ManagerConfig &config, std::string *err);

	// Applies a freshly read configuration to a plugin that is already
	// running. Settings that can be changed underneath live connections
	// (the status poll interval) always take effect; the audio server
	// URL, token, worker count and timeout can only change while no
	// station is registered, since the workers' connections and those
	// registrations are built from them. Returns kReloadPartial when it
	// applied the former and had to skip the latter, with *err saying
	// which.
	ReloadResult Reload(const ManagerConfig &config, std::string *err);
	bool configured();
	std::string server_version();
	std::string server_url();

	int CreateStation(const std::string &slug, const std::string &name,
	                  const std::string &description, int low_queue_threshold,
	                  const std::string &logo_url, std::string *err);
	bool DestroyStation(int station_id);
	bool IsValidStation(int station_id);
	int FindStationBySlug(const std::string &slug);
	std::vector<int> StationIds();

	bool SetMetadata(int station_id, const std::string &key, const std::string &value);
	bool ClearMetadata(int station_id);
	// Re-sends RegisterStation with the station's current fields. The
	// audio server updates metadata in place without disturbing playback.
	bool UpdateStation(int station_id, const std::string &name, const std::string &description,
	                   int low_queue_threshold, const std::string &logo_url);

	bool GetSlug(int station_id, std::string *out);
	bool GetStreamUrl(int station_id, std::string *out);
	bool GetStatus(int station_id, StationStatus *out);
	bool IsRegistered(int station_id);

	// Commands. Each returns a request id (> 0) to correlate with
	// OnGoRadioCommandResult, or 0 if the station id is unknown.
	int QueueTrack(int station_id, const std::string &location, const std::string &title,
	               const std::string &artist, const std::string &cover_art, int mode);
	int Dequeue(int station_id, const std::string &queue_id);
	int ClearQueue(int station_id, bool stop_current);
	int Skip(int station_id);
	int SkipTo(int station_id, const std::string &queue_id);
	int Pause(int station_id);
	int Resume(int station_id);
	int Seek(int station_id, long long position_seconds);
	int SeekBy(int station_id, long long delta_seconds);
	int RefreshStatus(int station_id);

	int RequestStationList();
	std::vector<StationSummary> ListedStations();

	// Drains the pending callbacks. Called once per server frame from
	// ProcessTick, on the main thread.
	void Tick(std::vector<PawnEvent> *out);
	void Shutdown();

private:
	Manager();
	~Manager();
	Manager(const Manager &);
	Manager &operator=(const Manager &);

	enum JobKind {
		kJobRegister,
		kJobUnregister,
		kJobQueueTrack,
		kJobCommand,
		kJobStatus,
		kJobListStations,
		kJobServerInfo
	};

	struct Job {
		JobKind kind;
		std::string rpc;
		std::string body;
		int station_id;
		int request_id;
		std::string command;    // shown in OnGoRadioCommandResult
		std::string result_key; // response field carrying the numeric result
		std::string flag_key;   // response field carrying the success flag
		std::shared_ptr<Station> station;

		Job() : kind(kJobCommand), station_id(GORADIO_INVALID_STATION), request_id(0) {}
	};

	void Enqueue(const Job &job);
	int EnqueueCommand(int station_id, const std::string &rpc, const std::string &command,
	                   const std::string &body, const std::string &flag_key,
	                   const std::string &result_key);
	void Push(const PawnEvent &ev);
	void PushError(int station_id, const RpcError &err);

	void StopWorkers();
	void WorkerLoop(ConnectClient *client);
	void PollerLoop();
	void StreamLoop(std::shared_ptr<Station> station);

	void HandleJobResponse(const Job &job, const JsonValue &resp);
	void ApplyStatus(const std::shared_ptr<Station> &station, const JsonValue &resp);
	void DispatchStationEvent(const std::shared_ptr<Station> &station, const JsonValue &event);
	// Registers (or re-registers) with backoff until it succeeds, the
	// station stops, or the failure is one retrying can't fix.
	bool RegisterWithRetry(const std::shared_ptr<Station> &station, ConnectClient *client,
	                       bool reconnect);

	std::shared_ptr<Station> FindStation(int station_id);

	ManagerConfig config_;
	Url url_;
	bool configured_;
	std::atomic<bool> running_;
	std::string server_version_;

	std::mutex state_mutex_; // stations_, config_, caches
	std::map<int, std::shared_ptr<Station> > stations_;
	std::vector<StationSummary> listed_stations_;
	int next_station_id_;
	int next_request_id_;

	std::mutex jobs_mutex_;
	std::condition_variable jobs_cv_;
	std::deque<Job> jobs_;
	std::vector<std::thread> workers_;
	std::vector<ConnectClient *> worker_clients_;

	std::thread poller_;
	std::mutex poller_mutex_;
	std::condition_variable poller_cv_;

	std::mutex dispatch_mutex_;
	std::deque<PawnEvent> dispatch_;
};

} // namespace goradio

#endif // GORADIO_MANAGER_H
