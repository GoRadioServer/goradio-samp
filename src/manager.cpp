#include "manager.h"

#include <chrono>

#include "log.h"

namespace goradio {
namespace {

const int kMinBackoffMs = 1000;
const int kMaxBackoffMs = 30000;

// How long a stream read parks before coming up for air to check whether
// the station is being torn down. Nothing is lost by waiting -- a quiet
// station legitimately sends nothing for minutes.
const int kStreamReadTimeoutMs = 1000;

int NextBackoff(int current) {
	int next = current * 2;
	return next > kMaxBackoffMs ? kMaxBackoffMs : next;
}

const char *SourceTypeFor(const std::string &location) {
	// The caller never declares which kind of source this is, so infer it
	// the way the audio server's own clients do: anything that looks like
	// a URL is an HTTP source, everything else is a path under the audio
	// server's configured audio root.
	if (location.compare(0, 7, "http://") == 0 || location.compare(0, 8, "https://") == 0) {
		return "TRACK_SOURCE_TYPE_HTTP_URL";
	}
	return "TRACK_SOURCE_TYPE_LOCAL_FILE";
}

} // namespace

Manager &Manager::Get() {
	static Manager instance;
	return instance;
}

Manager::Manager()
    : configured_(false), running_(false), next_station_id_(0), next_request_id_(0) {}

Manager::~Manager() { Shutdown(); }

bool Manager::configured() {
	std::lock_guard<std::mutex> lock(state_mutex_);
	return configured_;
}

std::string Manager::server_version() {
	std::lock_guard<std::mutex> lock(state_mutex_);
	return server_version_;
}

std::string Manager::server_url() {
	std::lock_guard<std::mutex> lock(state_mutex_);
	return config_.url;
}

bool Manager::Configure(const ManagerConfig &config, std::string *err) {
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		if (!stations_.empty()) {
			*err =
			    "cannot change the audio server while stations exist -- destroy them first, or "
			    "configure it in server.cfg before any station is created";
			return false;
		}
		if (config.token.empty()) {
			*err = "no auth token configured (set goradio_token in server.cfg, or pass one to "
			       "GoRadio_SetServer)";
			return false;
		}
		Url url;
		if (!ParseUrl(config.url, &url, err)) {
			return false;
		}
		if (url.secure() && !TlsSupported()) {
			*err = "this build has no TLS support, so it cannot use an https:// audio server URL";
			return false;
		}
		config_ = config;
		url_ = url;
		configured_ = true;
	}

	// Re-pointing an already-running plugin retires the old worker pool
	// rather than editing it in place: a worker's client is only ever
	// touched by the thread that owns it, and rewriting its URL or token
	// underneath a call in flight would be a race for no benefit. There
	// are no stations at this point (checked above), so there is nothing
	// to tear down but the workers themselves.
	StopWorkers();

	running_ = true;
	int worker_count = config.workers < 1 ? 1 : config.workers;
	for (int i = 0; i < worker_count; ++i) {
		ConnectClient *client = new ConnectClient(url_, config.token, config.timeout_ms);
		worker_clients_.push_back(client);
		workers_.push_back(std::thread(&Manager::WorkerLoop, this, client));
	}
	poller_ = std::thread(&Manager::PollerLoop, this);

	// Doubles as a connectivity check: the answer shows up in the server
	// log, and a bad token surfaces here rather than on the first
	// station.
	Job job;
	job.kind = kJobServerInfo;
	job.rpc = "GetServerInfo";
	job.body = "{}";
	Enqueue(job);

	LogInfo(Format("using audio server %s (%d worker%s, status poll every %ds)",
	               config.url.c_str(), config.workers, config.workers == 1 ? "" : "s",
	               config.poll_interval_ms / 1000));
	return true;
}

void Manager::Shutdown() {
	if (!running_) {
		return;
	}
	// running_ stays true until StopWorkers() below: the stream threads
	// are stopped by their own `stopping` flag, and they still need live
	// workers to unregister through on the way out.
	std::vector<std::shared_ptr<Station> > stations;
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		std::map<int, std::shared_ptr<Station> >::iterator it = stations_.begin();
		for (; it != stations_.end(); ++it) {
			stations.push_back(it->second);
		}
		stations_.clear();
	}

	// Stop the stream threads first: each is parked on a socket read, so
	// interrupting the socket is what actually makes the join quick.
	for (size_t i = 0; i < stations.size(); ++i) {
		stations[i]->stopping = true;
		stations[i]->WakeBackoff();
		if (stations[i]->stream != 0) {
			stations[i]->stream->Interrupt();
		}
	}
	for (size_t i = 0; i < stations.size(); ++i) {
		if (stations[i]->stream_thread.joinable()) {
			stations[i]->stream_thread.join();
		}
		delete stations[i]->stream;
		stations[i]->stream = 0;
	}

	StopWorkers();
	configured_ = false;
}

// Stops and joins the worker pool and the status poller. Callers are
// responsible for the stream threads first -- a stream loop that
// outlived its workers would sit registering against a pool that can no
// longer run its jobs.
void Manager::StopWorkers() {
	if (!running_) {
		return;
	}
	running_ = false;

	{
		std::lock_guard<std::mutex> lock(jobs_mutex_);
		jobs_.clear();
	}
	jobs_cv_.notify_all();
	poller_cv_.notify_all();

	// Interrupting the sockets is what makes a worker parked on a slow
	// response return promptly instead of waiting out its timeout.
	for (size_t i = 0; i < worker_clients_.size(); ++i) {
		worker_clients_[i]->Interrupt();
	}
	for (size_t i = 0; i < workers_.size(); ++i) {
		if (workers_[i].joinable()) {
			workers_[i].join();
		}
	}
	workers_.clear();
	for (size_t i = 0; i < worker_clients_.size(); ++i) {
		delete worker_clients_[i];
	}
	worker_clients_.clear();

	if (poller_.joinable()) {
		poller_.join();
	}
}

void Manager::Enqueue(const Job &job) {
	{
		std::lock_guard<std::mutex> lock(jobs_mutex_);
		jobs_.push_back(job);
	}
	jobs_cv_.notify_one();
}

void Manager::Push(const PawnEvent &ev) {
	std::lock_guard<std::mutex> lock(dispatch_mutex_);
	dispatch_.push_back(ev);
}

void Manager::PushError(int station_id, const RpcError &err) {
	PawnEvent ev;
	ev.kind = kEvError;
	ev.station_id = station_id;
	ev.text = err.code;
	ev.text2 = err.message;
	Push(ev);
}

void Manager::Tick(std::vector<PawnEvent> *out) {
	std::lock_guard<std::mutex> lock(dispatch_mutex_);
	while (!dispatch_.empty()) {
		out->push_back(dispatch_.front());
		dispatch_.pop_front();
	}
}

std::shared_ptr<Station> Manager::FindStation(int station_id) {
	std::lock_guard<std::mutex> lock(state_mutex_);
	std::map<int, std::shared_ptr<Station> >::iterator it = stations_.find(station_id);
	if (it == stations_.end()) {
		return std::shared_ptr<Station>();
	}
	return it->second;
}

bool Manager::IsValidStation(int station_id) { return FindStation(station_id).get() != 0; }

int Manager::FindStationBySlug(const std::string &slug) {
	std::lock_guard<std::mutex> lock(state_mutex_);
	std::map<int, std::shared_ptr<Station> >::iterator it = stations_.begin();
	for (; it != stations_.end(); ++it) {
		if (it->second->slug == slug) {
			return it->first;
		}
	}
	return GORADIO_INVALID_STATION;
}

std::vector<int> Manager::StationIds() {
	std::vector<int> ids;
	std::lock_guard<std::mutex> lock(state_mutex_);
	std::map<int, std::shared_ptr<Station> >::iterator it = stations_.begin();
	for (; it != stations_.end(); ++it) {
		ids.push_back(it->first);
	}
	return ids;
}

int Manager::CreateStation(const std::string &slug, const std::string &name,
                           const std::string &description, int low_queue_threshold,
                           const std::string &logo_url, std::string *err) {
	if (slug.empty()) {
		*err = "station slug is empty";
		return GORADIO_INVALID_STATION;
	}
	if (!configured()) {
		*err = "no audio server configured (set goradio_url/goradio_token in server.cfg, or call "
		       "GoRadio_SetServer first)";
		return GORADIO_INVALID_STATION;
	}
	if (FindStationBySlug(slug) != GORADIO_INVALID_STATION) {
		*err = "a station with slug \"" + slug + "\" already exists on this server";
		return GORADIO_INVALID_STATION;
	}

	std::shared_ptr<Station> station(new Station());
	Url url;
	int timeout_ms;
	std::string token;
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		station->id = next_station_id_++;
		station->slug = slug;
		station->name = name.empty() ? slug : name;
		station->description = description;
		station->logo_url = logo_url;
		station->low_queue_threshold = low_queue_threshold;
		stations_[station->id] = station;
		url = url_;
		timeout_ms = config_.timeout_ms;
		token = config_.token;
	}

	station->stream = new EventStream(url, token, timeout_ms);
	station->stream_thread = std::thread(&Manager::StreamLoop, this, station);
	LogInfo(Format("station %d (%s) created, registering...", station->id, slug.c_str()));
	return station->id;
}

bool Manager::DestroyStation(int station_id) {
	std::shared_ptr<Station> station;
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		std::map<int, std::shared_ptr<Station> >::iterator it = stations_.find(station_id);
		if (it == stations_.end()) {
			return false;
		}
		station = it->second;
		stations_.erase(it);
	}

	station->stopping = true;
	station->WakeBackoff();
	if (station->stream != 0) {
		station->stream->Interrupt();
	}
	if (station->stream_thread.joinable()) {
		station->stream_thread.join();
	}
	delete station->stream;
	station->stream = 0;

	// Unregistering disconnects listeners and drops the queue server-side,
	// so it goes out after the local teardown rather than racing it.
	JsonObject req;
	req.Str("slug", station->slug);
	Job job;
	job.kind = kJobUnregister;
	job.rpc = "UnregisterStation";
	job.body = req.Build();
	job.station_id = station_id;
	Enqueue(job);

	LogInfo(Format("station %d (%s) destroyed", station_id, station->slug.c_str()));
	return true;
}

bool Manager::SetMetadata(int station_id, const std::string &key, const std::string &value) {
	std::shared_ptr<Station> station = FindStation(station_id);
	if (!station) {
		return false;
	}
	std::lock_guard<std::mutex> lock(state_mutex_);
	if (value.empty()) {
		station->metadata.erase(key);
	} else {
		station->metadata[key] = value;
	}
	return true;
}

bool Manager::ClearMetadata(int station_id) {
	std::shared_ptr<Station> station = FindStation(station_id);
	if (!station) {
		return false;
	}
	std::lock_guard<std::mutex> lock(state_mutex_);
	station->metadata.clear();
	return true;
}

bool Manager::UpdateStation(int station_id, const std::string &name, const std::string &description,
                            int low_queue_threshold, const std::string &logo_url) {
	std::shared_ptr<Station> station = FindStation(station_id);
	if (!station) {
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		station->name = name.empty() ? station->slug : name;
		station->description = description;
		station->low_queue_threshold = low_queue_threshold;
		station->logo_url = logo_url;
	}
	Job job;
	job.kind = kJobRegister;
	job.station_id = station_id;
	job.station = station;
	Enqueue(job);
	return true;
}

bool Manager::GetSlug(int station_id, std::string *out) {
	std::shared_ptr<Station> station = FindStation(station_id);
	if (!station) {
		return false;
	}
	std::lock_guard<std::mutex> lock(state_mutex_);
	*out = station->slug;
	return true;
}

bool Manager::GetStreamUrl(int station_id, std::string *out) {
	std::shared_ptr<Station> station = FindStation(station_id);
	if (!station) {
		return false;
	}
	std::lock_guard<std::mutex> lock(state_mutex_);
	*out = station->stream_url;
	return true;
}

bool Manager::IsRegistered(int station_id) {
	std::shared_ptr<Station> station = FindStation(station_id);
	if (!station) {
		return false;
	}
	std::lock_guard<std::mutex> lock(state_mutex_);
	return station->registered;
}

bool Manager::GetStatus(int station_id, StationStatus *out) {
	std::shared_ptr<Station> station = FindStation(station_id);
	if (!station) {
		return false;
	}
	std::lock_guard<std::mutex> lock(state_mutex_);
	*out = station->status;
	if (out->valid && out->has_current && !out->is_paused) {
		// Carry elapsed forward from when the snapshot was taken, so a
		// progress readout advances smoothly between polls instead of
		// jumping once every poll interval.
		std::chrono::steady_clock::duration since =
		    std::chrono::steady_clock::now() - out->updated_at;
		long long secs = std::chrono::duration_cast<std::chrono::seconds>(since).count();
		if (secs > 0) {
			out->elapsed_seconds += secs;
			if (out->current.duration_seconds > 0 &&
			    out->elapsed_seconds > out->current.duration_seconds) {
				out->elapsed_seconds = out->current.duration_seconds;
			}
		}
	}
	return true;
}

std::vector<StationSummary> Manager::ListedStations() {
	std::lock_guard<std::mutex> lock(state_mutex_);
	return listed_stations_;
}

int Manager::EnqueueCommand(int station_id, const std::string &rpc, const std::string &command,
                            const std::string &body, const std::string &flag_key,
                            const std::string &result_key) {
	std::shared_ptr<Station> station = FindStation(station_id);
	if (!station) {
		return 0;
	}
	Job job;
	job.kind = kJobCommand;
	job.rpc = rpc;
	job.command = command;
	job.body = body;
	job.station_id = station_id;
	job.station = station;
	job.flag_key = flag_key;
	job.result_key = result_key;
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		job.request_id = ++next_request_id_;
	}
	Enqueue(job);
	return job.request_id;
}

int Manager::QueueTrack(int station_id, const std::string &location, const std::string &title,
                        const std::string &artist, const std::string &cover_art, int mode) {
	std::shared_ptr<Station> station = FindStation(station_id);
	if (!station) {
		return 0;
	}
	std::string slug;
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		slug = station->slug;
	}

	JsonObject source;
	source.Str("type", SourceTypeFor(location));
	source.Str("location", location);
	source.StrIfSet("displayTitle", title);
	source.StrIfSet("displayArtist", artist);
	source.StrIfSet("coverArtUrl", cover_art);

	JsonObject req;
	req.Str("slug", slug);
	req.Raw("source", source.Build());
	req.Str("mode", QueueModeToProto(mode));

	Job job;
	job.kind = kJobQueueTrack;
	job.rpc = "QueueTrack";
	job.command = "QueueTrack";
	job.body = req.Build();
	job.station_id = station_id;
	job.station = station;
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		job.request_id = ++next_request_id_;
	}
	Enqueue(job);
	return job.request_id;
}

int Manager::Dequeue(int station_id, const std::string &queue_id) {
	std::string slug;
	if (!GetSlug(station_id, &slug)) {
		return 0;
	}
	JsonObject req;
	req.Str("slug", slug);
	req.Str("queueId", queue_id);
	return EnqueueCommand(station_id, "RemoveFromQueue", "RemoveFromQueue", req.Build(), "removed",
	                      "");
}

int Manager::ClearQueue(int station_id, bool stop_current) {
	std::string slug;
	if (!GetSlug(station_id, &slug)) {
		return 0;
	}
	JsonObject req;
	req.Str("slug", slug);
	if (stop_current) {
		req.Bool("stopCurrent", true);
	}
	return EnqueueCommand(station_id, "ClearQueue", "ClearQueue", req.Build(), "", "removedCount");
}

int Manager::Skip(int station_id) {
	std::string slug;
	if (!GetSlug(station_id, &slug)) {
		return 0;
	}
	JsonObject req;
	req.Str("slug", slug);
	return EnqueueCommand(station_id, "Skip", "Skip", req.Build(), "skipped", "");
}

int Manager::SkipTo(int station_id, const std::string &queue_id) {
	std::string slug;
	if (!GetSlug(station_id, &slug)) {
		return 0;
	}
	JsonObject req;
	req.Str("slug", slug);
	req.Str("queueId", queue_id);
	return EnqueueCommand(station_id, "SkipTo", "SkipTo", req.Build(), "", "removedCount");
}

int Manager::Pause(int station_id) {
	std::string slug;
	if (!GetSlug(station_id, &slug)) {
		return 0;
	}
	JsonObject req;
	req.Str("slug", slug);
	return EnqueueCommand(station_id, "Pause", "Pause", req.Build(), "paused", "");
}

int Manager::Resume(int station_id) {
	std::string slug;
	if (!GetSlug(station_id, &slug)) {
		return 0;
	}
	JsonObject req;
	req.Str("slug", slug);
	return EnqueueCommand(station_id, "Resume", "Resume", req.Build(), "resumed", "");
}

int Manager::Seek(int station_id, long long position_seconds) {
	std::string slug;
	if (!GetSlug(station_id, &slug)) {
		return 0;
	}
	JsonObject req;
	req.Str("slug", slug);
	req.Int("positionSeconds", position_seconds);
	return EnqueueCommand(station_id, "Seek", "Seek", req.Build(), "seeked", "positionSeconds");
}

int Manager::SeekBy(int station_id, long long delta_seconds) {
	std::string slug;
	if (!GetSlug(station_id, &slug)) {
		return 0;
	}
	JsonObject req;
	req.Str("slug", slug);
	req.Int("deltaSeconds", delta_seconds);
	return EnqueueCommand(station_id, "SeekBy", "SeekBy", req.Build(), "seeked", "positionSeconds");
}

int Manager::RefreshStatus(int station_id) {
	std::shared_ptr<Station> station = FindStation(station_id);
	if (!station) {
		return 0;
	}
	std::string slug;
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		slug = station->slug;
	}
	JsonObject req;
	req.Str("slug", slug);

	Job job;
	job.kind = kJobStatus;
	job.rpc = "GetStatus";
	job.body = req.Build();
	job.station_id = station_id;
	job.station = station;
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		job.request_id = ++next_request_id_;
	}
	Enqueue(job);
	return job.request_id;
}

int Manager::RequestStationList() {
	if (!configured()) {
		return 0;
	}
	Job job;
	job.kind = kJobListStations;
	job.rpc = "ListStations";
	job.body = "{}";
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		job.request_id = ++next_request_id_;
	}
	Enqueue(job);
	return job.request_id;
}

void Manager::WorkerLoop(ConnectClient *client) {
	for (;;) {
		Job job;
		{
			std::unique_lock<std::mutex> lock(jobs_mutex_);
			jobs_cv_.wait(lock, [this] { return !jobs_.empty() || !running_; });
			if (jobs_.empty()) {
				if (!running_) {
					return;
				}
				continue;
			}
			job = jobs_.front();
			jobs_.pop_front();
		}

		if (job.kind == kJobRegister) {
			// A re-registration triggered by GoRadio_UpdateStation. It
			// retries on its own, so it doesn't go through Call() here.
			if (job.station && !job.station->stopping) {
				RegisterWithRetry(job.station, client, false);
			}
			continue;
		}

		JsonValue resp;
		RpcError err;
		if (!client->Call(job.rpc, job.body, &resp, &err)) {
			if (job.kind == kJobUnregister) {
				// The station is already gone locally; the audio server
				// drops it on its own if this never lands.
				LogWarn(Format("UnregisterStation failed: %s", err.ToString().c_str()));
				continue;
			}
			LogWarn(Format("%s failed: %s", job.rpc.c_str(), err.ToString().c_str()));
			if (job.kind == kJobCommand || job.kind == kJobQueueTrack) {
				PawnEvent ev;
				ev.kind = kEvCommandResult;
				ev.station_id = job.station_id;
				ev.request_id = job.request_id;
				ev.text = job.command;
				ev.flag = false;
				Push(ev);
			}
			PushError(job.station_id, err);
			continue;
		}
		HandleJobResponse(job, resp);
	}
}

void Manager::HandleJobResponse(const Job &job, const JsonValue &resp) {
	switch (job.kind) {
		case kJobQueueTrack: {
			PawnEvent ev;
			ev.kind = kEvTrackQueued;
			ev.station_id = job.station_id;
			ev.request_id = job.request_id;
			ev.queue_id = resp.Str("queueId");
			ev.text = resp.Str("status");
			ev.num2 = resp.Int("queuePosition", 0);
			ev.flag = true;
			Push(ev);
			break;
		}

		case kJobCommand: {
			PawnEvent ev;
			ev.kind = kEvCommandResult;
			ev.station_id = job.station_id;
			ev.request_id = job.request_id;
			ev.text = job.command;
			// An RPC that reports "nothing to do" (nothing was playing,
			// the id wasn't in the queue) answers false rather than
			// erroring, so success here means "the call landed and did
			// what it says".
			ev.flag = job.flag_key.empty() ? true : resp.Bool(job.flag_key, false);
			ev.num1 = job.result_key.empty() ? (ev.flag ? 1 : 0) : resp.Int(job.result_key, 0);
			Push(ev);
			break;
		}

		case kJobStatus: {
			if (job.station) {
				ApplyStatus(job.station, resp);
			}
			PawnEvent ev;
			ev.kind = kEvStatusUpdate;
			ev.station_id = job.station_id;
			ev.request_id = job.request_id;
			Push(ev);
			break;
		}

		case kJobListStations: {
			std::vector<StationSummary> listed;
			const JsonValue &arr = resp.Get("stations");
			for (size_t i = 0; i < arr.Size(); ++i) {
				const JsonValue &item = arr.At(i);
				StationSummary s;
				s.slug = item.Str("slug");
				s.name = item.Str("name");
				s.logo_url = item.Str("logoUrl");
				s.listener_count = item.Int("listenerCount", 0);
				listed.push_back(s);
			}
			{
				std::lock_guard<std::mutex> lock(state_mutex_);
				listed_stations_ = listed;
			}
			PawnEvent ev;
			ev.kind = kEvStationList;
			ev.request_id = job.request_id;
			ev.num1 = static_cast<long long>(listed.size());
			Push(ev);
			break;
		}

		case kJobServerInfo: {
			std::string version = resp.Str("version");
			{
				std::lock_guard<std::mutex> lock(state_mutex_);
				server_version_ = version;
			}
			LogInfo(Format("connected to audio server %s", version.c_str()));
			PawnEvent ev;
			ev.kind = kEvServerInfo;
			ev.text = version;
			Push(ev);
			break;
		}

		case kJobUnregister:
		case kJobRegister:
		default:
			break;
	}
}

void Manager::ApplyStatus(const std::shared_ptr<Station> &station, const JsonValue &resp) {
	std::lock_guard<std::mutex> lock(state_mutex_);
	StationStatus &st = station->status;
	st.valid = true;
	st.name = resp.Str("name");
	st.is_registered = resp.Bool("isRegistered", false);
	st.is_silence = resp.Bool("isSilence", false);
	st.is_paused = resp.Bool("isPaused", false);
	st.listener_count = resp.Int("listenerCount", 0);
	st.uptime_seconds = resp.Int("uptimeSeconds", 0);
	st.elapsed_seconds = resp.Int("currentTrackElapsedSeconds", 0);
	st.logo_url = resp.Str("logoUrl");

	const JsonValue &current = resp.Get("currentTrack");
	st.has_current = !current.IsNull();
	st.current = TrackInfo();
	if (st.has_current) {
		st.current.FromJson(current);
	}

	st.queue.clear();
	const JsonValue &queue = resp.Get("queue");
	for (size_t i = 0; i < queue.Size(); ++i) {
		TrackInfo item;
		item.FromJson(queue.At(i));
		st.queue.push_back(item);
	}
	st.queue_length = static_cast<long long>(st.queue.size());

	st.history.clear();
	const JsonValue &history = resp.Get("history");
	for (size_t i = 0; i < history.Size(); ++i) {
		TrackInfo item;
		item.FromJson(history.At(i));
		item.reason = history.At(i).Str("reason");
		st.history.push_back(item);
	}

	st.updated_at = std::chrono::steady_clock::now();
}

bool Manager::RegisterWithRetry(const std::shared_ptr<Station> &station, ConnectClient *client,
                                bool reconnect) {
	int backoff = kMinBackoffMs;
	for (;;) {
		if (station->stopping || !running_) {
			return false;
		}
		std::string body;
		int station_id;
		std::string slug;
		{
			std::lock_guard<std::mutex> lock(state_mutex_);
			body = station->RegisterRequestJson();
			station_id = station->id;
			slug = station->slug;
		}

		JsonValue resp;
		RpcError err;
		if (client->Call("RegisterStation", body, &resp, &err)) {
			bool re_registered = resp.Bool("reRegistered", false);
			std::string stream_url = resp.Str("streamUrl");
			{
				std::lock_guard<std::mutex> lock(state_mutex_);
				station->registered = true;
				station->stream_url = stream_url;
			}
			LogInfo(Format("station %d (%s) registered%s, stream at %s", station_id, slug.c_str(),
			               re_registered ? " (existing station updated in place)" : "",
			               stream_url.c_str()));

			PawnEvent ev;
			ev.kind = kEvStationRegistered;
			ev.station_id = station_id;
			ev.text = stream_url;
			ev.flag = re_registered;
			ev.num1 = reconnect ? 1 : 0;
			Push(ev);
			return true;
		}

		{
			std::lock_guard<std::mutex> lock(state_mutex_);
			station->registered = false;
		}
		if (err.permanent()) {
			// Retrying a bad token or a slug this token doesn't cover
			// produces an unkillable-looking retry loop over what is
			// really a config mistake, so stop and say so.
			LogError(Format("station %d (%s) cannot register: %s", station_id, slug.c_str(),
			                err.ToString().c_str()));
			PushError(station_id, err);
			return false;
		}
		LogWarn(Format("station %d (%s) register failed (%s), retrying in %dms", station_id,
		               slug.c_str(), err.ToString().c_str(), backoff));
		station->WaitBackoff(backoff);
		backoff = NextBackoff(backoff);
	}
}

void Manager::StreamLoop(std::shared_ptr<Station> station) {
	Url url;
	std::string token;
	int timeout_ms;
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		url = url_;
		token = config_.token;
		timeout_ms = config_.timeout_ms;
	}
	ConnectClient client(url, token, timeout_ms);

	// Register before subscribing: SubscribeEvents against a slug the
	// audio server doesn't know is a NotFound, not a stream that starts
	// working once the station shows up.
	if (!RegisterWithRetry(station, &client, false)) {
		return;
	}
	RefreshStatus(station->id);

	int backoff = kMinBackoffMs;
	while (!station->stopping && running_) {
		std::string slug;
		{
			std::lock_guard<std::mutex> lock(state_mutex_);
			slug = station->slug;
		}

		RpcError err;
		if (!station->stream->Open(slug, &err)) {
			if (station->stopping) {
				return;
			}
			if (err.permanent()) {
				LogError(Format("station %d (%s) cannot subscribe to events: %s", station->id,
				                slug.c_str(), err.ToString().c_str()));
				PushError(station->id, err);
				return;
			}
			LogWarn(Format("station %d (%s) event subscribe failed (%s), retrying in %dms",
			               station->id, slug.c_str(), err.ToString().c_str(), backoff));
			station->WaitBackoff(backoff);
			backoff = NextBackoff(backoff);
			continue;
		}

		LogInfo(Format("station %d (%s) subscribed to events", station->id, slug.c_str()));
		backoff = kMinBackoffMs;

		for (;;) {
			JsonValue event;
			RpcError read_err;
			int r = station->stream->Read(&event, kStreamReadTimeoutMs, &read_err);
			if (r == 0) {
				if (station->stopping || !running_) {
					break;
				}
				continue;
			}
			if (r < 0) {
				if (!station->stopping && running_) {
					LogWarn(Format("station %d (%s) event stream ended (%s), reconnecting",
					               station->id, slug.c_str(), read_err.message.c_str()));
				}
				break;
			}
			DispatchStationEvent(station, event);
		}

		station->stream->Close();
		if (station->stopping || !running_) {
			return;
		}

		// A dropped stream may mean the audio server restarted, in which
		// case its registry -- and this station's whole queue -- came
		// back empty. Re-register unconditionally, and let the
		// OnGoRadioStationRegistered callback re-prime the queue.
		{
			std::lock_guard<std::mutex> lock(state_mutex_);
			station->registered = false;
		}
		if (!RegisterWithRetry(station, &client, true)) {
			return;
		}
		RefreshStatus(station->id);
		station->WaitBackoff(backoff);
		backoff = NextBackoff(backoff);
	}
}

void Manager::DispatchStationEvent(const std::shared_ptr<Station> &station,
                                   const JsonValue &event) {
	std::string type = event.Str("type");
	PawnEvent ev;
	ev.station_id = station->id;

	if (type == "EVENT_TYPE_TRACK_STARTED") {
		const JsonValue &p = event.Get("trackStarted");
		TrackInfo track;
		track.FromJson(p);
		{
			std::lock_guard<std::mutex> lock(state_mutex_);
			// Fold the event straight into the cache so the PAWN getters
			// are right immediately, rather than up to one poll stale.
			station->status.valid = true;
			station->status.has_current = true;
			station->status.current = track;
			station->status.is_silence = false;
			station->status.is_paused = false;
			station->status.elapsed_seconds = 0;
			station->status.updated_at = std::chrono::steady_clock::now();
		}
		ev.kind = kEvTrackStarted;
		ev.queue_id = track.queue_id;
		ev.location = track.location;
		ev.title = track.title;
		ev.artist = track.artist;
		ev.cover_art = track.cover_art;
		ev.num1 = track.duration_seconds;
		Push(ev);
		RefreshStatus(station->id);
		return;
	}

	if (type == "EVENT_TYPE_TRACK_ENDED") {
		const JsonValue &p = event.Get("trackEnded");
		{
			std::lock_guard<std::mutex> lock(state_mutex_);
			station->status.has_current = false;
			station->status.current = TrackInfo();
			station->status.elapsed_seconds = 0;
		}
		ev.kind = kEvTrackEnded;
		ev.queue_id = p.Str("queueId");
		ev.text = p.Str("reason");
		Push(ev);
		RefreshStatus(station->id);
		return;
	}

	if (type == "EVENT_TYPE_QUEUE_UPDATED") {
		long long length = event.Get("queueUpdated").Int("queueLength", 0);
		{
			std::lock_guard<std::mutex> lock(state_mutex_);
			station->status.queue_length = length;
		}
		ev.kind = kEvQueueUpdated;
		ev.num1 = length;
		Push(ev);
		return;
	}

	if (type == "EVENT_TYPE_LISTENER_COUNT_CHANGED") {
		long long count = event.Get("listenerCountChanged").Int("listenerCount", 0);
		{
			std::lock_guard<std::mutex> lock(state_mutex_);
			station->status.listener_count = count;
		}
		ev.kind = kEvListenerCount;
		ev.num1 = count;
		Push(ev);
		return;
	}

	if (type == "EVENT_TYPE_QUEUE_LOW") {
		const JsonValue &p = event.Get("queueLow");
		ev.kind = kEvQueueLow;
		ev.num1 = p.Int("queueLength", 0);
		ev.num2 = p.Int("threshold", 0);
		Push(ev);
		return;
	}

	if (type == "EVENT_TYPE_SILENCE_STARTED" || type == "EVENT_TYPE_SILENCE_ENDED") {
		bool silence = (type == "EVENT_TYPE_SILENCE_STARTED");
		{
			std::lock_guard<std::mutex> lock(state_mutex_);
			station->status.is_silence = silence;
		}
		ev.kind = silence ? kEvSilenceStarted : kEvSilenceEnded;
		Push(ev);
		return;
	}

	if (type == "EVENT_TYPE_ERROR") {
		const JsonValue &p = event.Get("error");
		ev.kind = kEvError;
		ev.text = p.Str("code");
		ev.text2 = p.Str("message");
		Push(ev);
		return;
	}

	LogDebug(Format("station %d: unhandled event type %s", station->id, type.c_str()));
}

void Manager::PollerLoop() {
	for (;;) {
		int interval;
		{
			std::lock_guard<std::mutex> lock(state_mutex_);
			interval = config_.poll_interval_ms;
		}
		if (interval <= 0) {
			interval = 5000; // still wake up, just don't poll
		}
		{
			std::unique_lock<std::mutex> lock(poller_mutex_);
			poller_cv_.wait_for(lock, std::chrono::milliseconds(interval),
			                    [this] { return !running_; });
		}
		if (!running_) {
			return;
		}
		{
			std::lock_guard<std::mutex> lock(state_mutex_);
			if (config_.poll_interval_ms <= 0) {
				continue;
			}
		}
		std::vector<int> ids = StationIds();
		for (size_t i = 0; i < ids.size(); ++i) {
			RefreshStatus(ids[i]);
		}
	}
}

} // namespace goradio
