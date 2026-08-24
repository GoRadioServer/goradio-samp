#ifndef GORADIO_STATION_H
#define GORADIO_STATION_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "connect_client.h"

namespace goradio {

#define GORADIO_INVALID_STATION (-1)

// Queue modes, matching audioserver.v1.QueueMode. The PAWN-side constants
// in goradio.inc use these same numbers.
enum QueueMode {
	kQueueAppend = 0,
	kQueuePlayNext = 1,
	kQueuePlayNowInterrupt = 2
};

const char *QueueModeToProto(int mode);

struct TrackInfo {
	std::string queue_id;
	std::string location;
	std::string title;
	std::string artist;
	std::string cover_art;
	long long duration_seconds;
	// "completed" or "interrupted" -- only set on history entries.
	std::string reason;

	TrackInfo() : duration_seconds(0) {}
	void FromJson(const JsonValue &item);
};

// The last GetStatus snapshot, kept so the PAWN getters can answer
// immediately instead of blocking a game frame on an HTTP round trip.
// Refreshed by the status poller, and nudged by events in between.
struct StationStatus {
	bool valid;
	std::string name;
	bool is_registered;
	bool is_silence;
	bool is_paused;
	long long listener_count;
	long long uptime_seconds;
	long long elapsed_seconds;
	bool has_current;
	TrackInfo current;
	// The pending items as of the last snapshot. queue_length is tracked
	// separately because QUEUE_UPDATED events carry a fresh length
	// without the items to go with it -- so the count stays current
	// between polls even though this vector doesn't.
	std::vector<TrackInfo> queue;
	long long queue_length;
	// Most recently finished items, oldest first, as the audio server
	// returned them -- capped server-side at a small fixed count.
	std::vector<TrackInfo> history;
	std::string logo_url;

	// When this snapshot was taken, used to extrapolate elapsed_seconds
	// between polls so a progress readout doesn't visibly stutter.
	std::chrono::steady_clock::time_point updated_at;

	StationStatus()
	    : valid(false),
	      is_registered(false),
	      is_silence(true),
	      is_paused(false),
	      listener_count(0),
	      uptime_seconds(0),
	      elapsed_seconds(0),
	      has_current(false),
	      queue_length(0) {}
};

// One station this server owns. Created by GoRadio_CreateStation and kept
// alive by a shared_ptr, because a job or an event referencing it can
// still be in flight on a worker thread after PAWN has destroyed it.
struct Station {
	int id;
	std::string slug;

	// Registration parameters, re-sent verbatim on every reconnect --
	// RegisterStation is idempotent by slug and replaces all of them, so
	// anything missed here silently reverts to its zero value the next
	// time the connection drops.
	std::string name;
	std::string description;
	std::string logo_url;
	std::map<std::string, std::string> metadata;
	int low_queue_threshold;

	std::string stream_url;
	bool registered;

	// Set by DestroyStation. The stream loop checks it after a dropped
	// connection so an intentional teardown isn't undone by the usual
	// "reconnect and re-register" recovery. Atomic because the loop reads
	// it without taking the manager's lock.
	std::atomic<bool> stopping;

	StationStatus status;

	std::thread stream_thread;
	EventStream *stream;

	Station()
	    : id(GORADIO_INVALID_STATION),
	      low_queue_threshold(0),
	      registered(false),
	      stopping(false),
	      stream(0) {}

	// Serialises metadata as a protobuf-JSON map value ("{}" when empty).
	std::string MetadataJson() const;
	// The RegisterStation request body for this station's current fields.
	std::string RegisterRequestJson() const;

	// Sleeps for the reconnect backoff, returning early the moment
	// stopping is set -- so unloading the plugin doesn't have to wait out
	// a 30-second backoff before the stream thread notices.
	void WaitBackoff(int ms);
	void WakeBackoff();

	std::mutex wait_mutex;
	std::condition_variable wait_cv;

private:
	Station(const Station &);
	Station &operator=(const Station &);
};

// One entry from ListStations -- every station this server's token
// authorizes, which is not the same set as the stations this plugin
// created.
struct StationSummary {
	std::string slug;
	std::string name;
	std::string logo_url;
	long long listener_count;

	StationSummary() : listener_count(0) {}
};

} // namespace goradio

#endif // GORADIO_STATION_H
