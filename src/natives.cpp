// The PAWN-facing surface: every GoRadio_* native, plus the translation
// of queued PawnEvents into OnGoRadio* callbacks. Everything in this file
// runs on the server's main thread.
#include "natives.h"

#include "amxutil.h"
#include "log.h"
#include "manager.h"

namespace goradio {
namespace {

// params[0] is the argument list size in bytes, not the count.
int ArgCount(const cell *params) { return static_cast<int>(params[0] / sizeof(cell)); }

// PAWN has no 64-bit integer, so every int64 the audio server reports has
// to land in a 32-bit cell. Saturate rather than truncate: a listener
// count that wrapped to a negative number would be read as real data,
// where a clamped one is merely implausible.
cell ClampCell(long long value) {
	const long long kMax = 2147483647LL;
	if (value > kMax) {
		return static_cast<cell>(kMax);
	}
	if (value < -2147483647LL - 1) {
		return static_cast<cell>(-2147483647LL - 1);
	}
	return static_cast<cell>(value);
}

cell ClampCell(size_t value) { return ClampCell(static_cast<long long>(value)); }

bool CheckArgs(const cell *params, int expected, const char *name) {
	if (ArgCount(params) < expected) {
		LogError(Format("%s called with %d arguments, expected %d", name, ArgCount(params),
		                expected));
		return false;
	}
	return true;
}

// Writes value into the destination string argument at dest_index (whose
// size is the argument at len_index) and returns ok as the native's
// result -- the shape every GoRadio_Get*(..., dest[], len) native shares.
cell WriteStringResult(AMX *amx, const cell *params, int dest_index, int len_index,
                       const std::string &value, bool ok) {
	if (!ok) {
		AmxWriteString(amx, params[dest_index], std::string(), params[len_index]);
		return 0;
	}
	AmxWriteString(amx, params[dest_index], value, params[len_index]);
	return 1;
}

const TrackInfo *ItemAt(const std::vector<TrackInfo> &items, cell index) {
	if (index < 0 || static_cast<size_t>(index) >= items.size()) {
		return 0;
	}
	return &items[static_cast<size_t>(index)];
}

// ---------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------

// native GoRadio_SetServer(const url[], const token[]);
cell AMXAPI n_SetServer(AMX *amx, cell *params) {
	if (!CheckArgs(params, 2, "GoRadio_SetServer")) {
		return 0;
	}
	ManagerConfig config;
	config.url = AmxReadString(amx, params[1]);
	config.token = AmxReadString(amx, params[2]);

	std::string err;
	if (!Manager::Get().Configure(config, &err)) {
		LogError("GoRadio_SetServer: " + err);
		return 0;
	}
	return 1;
}

// native GoRadio_IsReady();
cell AMXAPI n_IsReady(AMX *, cell *) { return Manager::Get().configured() ? 1 : 0; }

// native GoRadio_GetServerVersion(dest[], len = sizeof dest);
cell AMXAPI n_GetServerVersion(AMX *amx, cell *params) {
	if (!CheckArgs(params, 2, "GoRadio_GetServerVersion")) {
		return 0;
	}
	std::string version = Manager::Get().server_version();
	return WriteStringResult(amx, params, 1, 2, version, !version.empty());
}

// ---------------------------------------------------------------------
// Stations
// ---------------------------------------------------------------------

// native GoRadio_CreateStation(const slug[], const name[], const description[],
//                              lowQueueThreshold, const logoUrl[]);
cell AMXAPI n_CreateStation(AMX *amx, cell *params) {
	if (!CheckArgs(params, 5, "GoRadio_CreateStation")) {
		return GORADIO_INVALID_STATION;
	}
	std::string slug = AmxReadString(amx, params[1]);
	std::string name = AmxReadString(amx, params[2]);
	std::string description = AmxReadString(amx, params[3]);
	int threshold = static_cast<int>(params[4]);
	std::string logo_url = AmxReadString(amx, params[5]);

	std::string err;
	int id = Manager::Get().CreateStation(slug, name, description, threshold, logo_url, &err);
	if (id == GORADIO_INVALID_STATION) {
		LogError("GoRadio_CreateStation: " + err);
	}
	return id;
}

// native GoRadio_DestroyStation(stationid);
cell AMXAPI n_DestroyStation(AMX *, cell *params) {
	if (!CheckArgs(params, 1, "GoRadio_DestroyStation")) {
		return 0;
	}
	return Manager::Get().DestroyStation(static_cast<int>(params[1])) ? 1 : 0;
}

// native GoRadio_IsValidStation(stationid);
cell AMXAPI n_IsValidStation(AMX *, cell *params) {
	if (!CheckArgs(params, 1, "GoRadio_IsValidStation")) {
		return 0;
	}
	return Manager::Get().IsValidStation(static_cast<int>(params[1])) ? 1 : 0;
}

// native GoRadio_GetStationBySlug(const slug[]);
cell AMXAPI n_GetStationBySlug(AMX *amx, cell *params) {
	if (!CheckArgs(params, 1, "GoRadio_GetStationBySlug")) {
		return GORADIO_INVALID_STATION;
	}
	return Manager::Get().FindStationBySlug(AmxReadString(amx, params[1]));
}

// native GoRadio_GetStationCount();
cell AMXAPI n_GetStationCount(AMX *, cell *) {
	return ClampCell(Manager::Get().StationIds().size());
}

// native GoRadio_GetStationAtIndex(index);
cell AMXAPI n_GetStationAtIndex(AMX *, cell *params) {
	if (!CheckArgs(params, 1, "GoRadio_GetStationAtIndex")) {
		return GORADIO_INVALID_STATION;
	}
	std::vector<int> ids = Manager::Get().StationIds();
	cell index = params[1];
	if (index < 0 || static_cast<size_t>(index) >= ids.size()) {
		return GORADIO_INVALID_STATION;
	}
	return ids[static_cast<size_t>(index)];
}

// native GoRadio_SetStationMetadata(stationid, const key[], const value[]);
cell AMXAPI n_SetStationMetadata(AMX *amx, cell *params) {
	if (!CheckArgs(params, 3, "GoRadio_SetStationMetadata")) {
		return 0;
	}
	return Manager::Get().SetMetadata(static_cast<int>(params[1]), AmxReadString(amx, params[2]),
	                                  AmxReadString(amx, params[3]))
	           ? 1
	           : 0;
}

// native GoRadio_ClearStationMetadata(stationid);
cell AMXAPI n_ClearStationMetadata(AMX *, cell *params) {
	if (!CheckArgs(params, 1, "GoRadio_ClearStationMetadata")) {
		return 0;
	}
	return Manager::Get().ClearMetadata(static_cast<int>(params[1])) ? 1 : 0;
}

// native GoRadio_UpdateStation(stationid, const name[], const description[],
//                              lowQueueThreshold, const logoUrl[]);
cell AMXAPI n_UpdateStation(AMX *amx, cell *params) {
	if (!CheckArgs(params, 5, "GoRadio_UpdateStation")) {
		return 0;
	}
	return Manager::Get().UpdateStation(static_cast<int>(params[1]), AmxReadString(amx, params[2]),
	                                    AmxReadString(amx, params[3]), static_cast<int>(params[4]),
	                                    AmxReadString(amx, params[5]))
	           ? 1
	           : 0;
}

// native GoRadio_GetStationSlug(stationid, dest[], len = sizeof dest);
cell AMXAPI n_GetStationSlug(AMX *amx, cell *params) {
	if (!CheckArgs(params, 3, "GoRadio_GetStationSlug")) {
		return 0;
	}
	std::string slug;
	bool ok = Manager::Get().GetSlug(static_cast<int>(params[1]), &slug);
	return WriteStringResult(amx, params, 2, 3, slug, ok);
}

// native GoRadio_GetStreamURL(stationid, dest[], len = sizeof dest);
cell AMXAPI n_GetStreamURL(AMX *amx, cell *params) {
	if (!CheckArgs(params, 3, "GoRadio_GetStreamURL")) {
		return 0;
	}
	std::string url;
	bool ok = Manager::Get().GetStreamUrl(static_cast<int>(params[1]), &url);
	return WriteStringResult(amx, params, 2, 3, url, ok && !url.empty());
}

// native GoRadio_IsStationRegistered(stationid);
cell AMXAPI n_IsStationRegistered(AMX *, cell *params) {
	if (!CheckArgs(params, 1, "GoRadio_IsStationRegistered")) {
		return 0;
	}
	return Manager::Get().IsRegistered(static_cast<int>(params[1])) ? 1 : 0;
}

// ---------------------------------------------------------------------
// Playback control
// ---------------------------------------------------------------------

// native GoRadio_QueueTrack(stationid, const location[], const title[],
//                           const artist[], const coverArt[], mode);
cell AMXAPI n_QueueTrack(AMX *amx, cell *params) {
	if (!CheckArgs(params, 6, "GoRadio_QueueTrack")) {
		return 0;
	}
	return Manager::Get().QueueTrack(static_cast<int>(params[1]), AmxReadString(amx, params[2]),
	                                 AmxReadString(amx, params[3]), AmxReadString(amx, params[4]),
	                                 AmxReadString(amx, params[5]), static_cast<int>(params[6]));
}

// native GoRadio_Dequeue(stationid, const queueId[]);
cell AMXAPI n_Dequeue(AMX *amx, cell *params) {
	if (!CheckArgs(params, 2, "GoRadio_Dequeue")) {
		return 0;
	}
	return Manager::Get().Dequeue(static_cast<int>(params[1]), AmxReadString(amx, params[2]));
}

// native GoRadio_ClearQueue(stationid, bool:stopCurrent);
cell AMXAPI n_ClearQueue(AMX *, cell *params) {
	if (!CheckArgs(params, 2, "GoRadio_ClearQueue")) {
		return 0;
	}
	return Manager::Get().ClearQueue(static_cast<int>(params[1]), params[2] != 0);
}

// native GoRadio_Skip(stationid);
cell AMXAPI n_Skip(AMX *, cell *params) {
	if (!CheckArgs(params, 1, "GoRadio_Skip")) {
		return 0;
	}
	return Manager::Get().Skip(static_cast<int>(params[1]));
}

// native GoRadio_SkipTo(stationid, const queueId[]);
cell AMXAPI n_SkipTo(AMX *amx, cell *params) {
	if (!CheckArgs(params, 2, "GoRadio_SkipTo")) {
		return 0;
	}
	return Manager::Get().SkipTo(static_cast<int>(params[1]), AmxReadString(amx, params[2]));
}

// native GoRadio_Pause(stationid);
cell AMXAPI n_Pause(AMX *, cell *params) {
	if (!CheckArgs(params, 1, "GoRadio_Pause")) {
		return 0;
	}
	return Manager::Get().Pause(static_cast<int>(params[1]));
}

// native GoRadio_Resume(stationid);
cell AMXAPI n_Resume(AMX *, cell *params) {
	if (!CheckArgs(params, 1, "GoRadio_Resume")) {
		return 0;
	}
	return Manager::Get().Resume(static_cast<int>(params[1]));
}

// native GoRadio_Seek(stationid, positionSeconds);
cell AMXAPI n_Seek(AMX *, cell *params) {
	if (!CheckArgs(params, 2, "GoRadio_Seek")) {
		return 0;
	}
	return Manager::Get().Seek(static_cast<int>(params[1]), static_cast<long long>(params[2]));
}

// native GoRadio_SeekBy(stationid, deltaSeconds);
cell AMXAPI n_SeekBy(AMX *, cell *params) {
	if (!CheckArgs(params, 2, "GoRadio_SeekBy")) {
		return 0;
	}
	return Manager::Get().SeekBy(static_cast<int>(params[1]), static_cast<long long>(params[2]));
}

// ---------------------------------------------------------------------
// Cached status
// ---------------------------------------------------------------------

// native GoRadio_RefreshStatus(stationid);
cell AMXAPI n_RefreshStatus(AMX *, cell *params) {
	if (!CheckArgs(params, 1, "GoRadio_RefreshStatus")) {
		return 0;
	}
	return Manager::Get().RefreshStatus(static_cast<int>(params[1]));
}

// native GoRadio_HasStatus(stationid);
cell AMXAPI n_HasStatus(AMX *, cell *params) {
	if (!CheckArgs(params, 1, "GoRadio_HasStatus")) {
		return 0;
	}
	StationStatus status;
	if (!Manager::Get().GetStatus(static_cast<int>(params[1]), &status)) {
		return 0;
	}
	return status.valid ? 1 : 0;
}

// native GoRadio_GetListenerCount(stationid);
cell AMXAPI n_GetListenerCount(AMX *, cell *params) {
	if (!CheckArgs(params, 1, "GoRadio_GetListenerCount")) {
		return 0;
	}
	StationStatus status;
	if (!Manager::Get().GetStatus(static_cast<int>(params[1]), &status)) {
		return 0;
	}
	return ClampCell(status.listener_count);
}

// native GoRadio_GetUptime(stationid);
cell AMXAPI n_GetUptime(AMX *, cell *params) {
	if (!CheckArgs(params, 1, "GoRadio_GetUptime")) {
		return 0;
	}
	StationStatus status;
	if (!Manager::Get().GetStatus(static_cast<int>(params[1]), &status)) {
		return 0;
	}
	return ClampCell(status.uptime_seconds);
}

// native GoRadio_IsSilence(stationid);
cell AMXAPI n_IsSilence(AMX *, cell *params) {
	if (!CheckArgs(params, 1, "GoRadio_IsSilence")) {
		return 0;
	}
	StationStatus status;
	if (!Manager::Get().GetStatus(static_cast<int>(params[1]), &status)) {
		return 0;
	}
	return status.is_silence ? 1 : 0;
}

// native GoRadio_IsPaused(stationid);
cell AMXAPI n_IsPaused(AMX *, cell *params) {
	if (!CheckArgs(params, 1, "GoRadio_IsPaused")) {
		return 0;
	}
	StationStatus status;
	if (!Manager::Get().GetStatus(static_cast<int>(params[1]), &status)) {
		return 0;
	}
	return status.is_paused ? 1 : 0;
}

// native GoRadio_IsPlaying(stationid);
cell AMXAPI n_IsPlaying(AMX *, cell *params) {
	if (!CheckArgs(params, 1, "GoRadio_IsPlaying")) {
		return 0;
	}
	StationStatus status;
	if (!Manager::Get().GetStatus(static_cast<int>(params[1]), &status)) {
		return 0;
	}
	return status.has_current ? 1 : 0;
}

// native GoRadio_GetQueueLength(stationid);
cell AMXAPI n_GetQueueLength(AMX *, cell *params) {
	if (!CheckArgs(params, 1, "GoRadio_GetQueueLength")) {
		return 0;
	}
	StationStatus status;
	if (!Manager::Get().GetStatus(static_cast<int>(params[1]), &status)) {
		return 0;
	}
	return ClampCell(status.queue_length);
}

// The current-track string getters all share this shape.
cell CurrentTrackString(AMX *amx, const cell *params, int field) {
	StationStatus status;
	if (!Manager::Get().GetStatus(static_cast<int>(params[1]), &status) || !status.has_current) {
		return WriteStringResult(amx, params, 2, 3, std::string(), false);
	}
	const TrackInfo &t = status.current;
	const std::string *value = &t.queue_id;
	switch (field) {
		case 1: value = &t.title; break;
		case 2: value = &t.artist; break;
		case 3: value = &t.location; break;
		case 4: value = &t.cover_art; break;
		default: break;
	}
	return WriteStringResult(amx, params, 2, 3, *value, true);
}

cell AMXAPI n_GetCurrentTrackID(AMX *amx, cell *params) {
	if (!CheckArgs(params, 3, "GoRadio_GetCurrentTrackID")) {
		return 0;
	}
	return CurrentTrackString(amx, params, 0);
}

cell AMXAPI n_GetCurrentTrackTitle(AMX *amx, cell *params) {
	if (!CheckArgs(params, 3, "GoRadio_GetCurrentTrackTitle")) {
		return 0;
	}
	return CurrentTrackString(amx, params, 1);
}

cell AMXAPI n_GetCurrentTrackArtist(AMX *amx, cell *params) {
	if (!CheckArgs(params, 3, "GoRadio_GetCurrentTrackArtist")) {
		return 0;
	}
	return CurrentTrackString(amx, params, 2);
}

cell AMXAPI n_GetCurrentTrackLocation(AMX *amx, cell *params) {
	if (!CheckArgs(params, 3, "GoRadio_GetCurrentTrackLocation")) {
		return 0;
	}
	return CurrentTrackString(amx, params, 3);
}

cell AMXAPI n_GetCurrentTrackCoverArt(AMX *amx, cell *params) {
	if (!CheckArgs(params, 3, "GoRadio_GetCurrentTrackCoverArt")) {
		return 0;
	}
	return CurrentTrackString(amx, params, 4);
}

// native GoRadio_GetCurrentTrackDuration(stationid);
cell AMXAPI n_GetCurrentTrackDuration(AMX *, cell *params) {
	if (!CheckArgs(params, 1, "GoRadio_GetCurrentTrackDuration")) {
		return 0;
	}
	StationStatus status;
	if (!Manager::Get().GetStatus(static_cast<int>(params[1]), &status) || !status.has_current) {
		return 0;
	}
	return ClampCell(status.current.duration_seconds);
}

// native GoRadio_GetCurrentTrackElapsed(stationid);
cell AMXAPI n_GetCurrentTrackElapsed(AMX *, cell *params) {
	if (!CheckArgs(params, 1, "GoRadio_GetCurrentTrackElapsed")) {
		return 0;
	}
	StationStatus status;
	if (!Manager::Get().GetStatus(static_cast<int>(params[1]), &status) || !status.has_current) {
		return 0;
	}
	return ClampCell(status.elapsed_seconds);
}

// ---------------------------------------------------------------------
// Queue and history snapshots
// ---------------------------------------------------------------------

// native GoRadio_GetQueueItemCount(stationid);
cell AMXAPI n_GetQueueItemCount(AMX *, cell *params) {
	if (!CheckArgs(params, 1, "GoRadio_GetQueueItemCount")) {
		return 0;
	}
	StationStatus status;
	if (!Manager::Get().GetStatus(static_cast<int>(params[1]), &status)) {
		return 0;
	}
	return ClampCell(status.queue.size());
}

cell QueueItemString(AMX *amx, const cell *params, int field) {
	StationStatus status;
	if (!Manager::Get().GetStatus(static_cast<int>(params[1]), &status)) {
		return WriteStringResult(amx, params, 3, 4, std::string(), false);
	}
	const TrackInfo *item = ItemAt(status.queue, params[2]);
	if (item == 0) {
		return WriteStringResult(amx, params, 3, 4, std::string(), false);
	}
	const std::string *value = &item->queue_id;
	switch (field) {
		case 1: value = &item->title; break;
		case 2: value = &item->artist; break;
		case 3: value = &item->location; break;
		default: break;
	}
	return WriteStringResult(amx, params, 3, 4, *value, true);
}

cell AMXAPI n_GetQueueItemID(AMX *amx, cell *params) {
	if (!CheckArgs(params, 4, "GoRadio_GetQueueItemID")) {
		return 0;
	}
	return QueueItemString(amx, params, 0);
}

cell AMXAPI n_GetQueueItemTitle(AMX *amx, cell *params) {
	if (!CheckArgs(params, 4, "GoRadio_GetQueueItemTitle")) {
		return 0;
	}
	return QueueItemString(amx, params, 1);
}

cell AMXAPI n_GetQueueItemArtist(AMX *amx, cell *params) {
	if (!CheckArgs(params, 4, "GoRadio_GetQueueItemArtist")) {
		return 0;
	}
	return QueueItemString(amx, params, 2);
}

cell AMXAPI n_GetQueueItemLocation(AMX *amx, cell *params) {
	if (!CheckArgs(params, 4, "GoRadio_GetQueueItemLocation")) {
		return 0;
	}
	return QueueItemString(amx, params, 3);
}

// native GoRadio_GetQueueItemDuration(stationid, index);
cell AMXAPI n_GetQueueItemDuration(AMX *, cell *params) {
	if (!CheckArgs(params, 2, "GoRadio_GetQueueItemDuration")) {
		return 0;
	}
	StationStatus status;
	if (!Manager::Get().GetStatus(static_cast<int>(params[1]), &status)) {
		return 0;
	}
	const TrackInfo *item = ItemAt(status.queue, params[2]);
	return item == 0 ? 0 : ClampCell(item->duration_seconds);
}

// native GoRadio_GetHistoryCount(stationid);
cell AMXAPI n_GetHistoryCount(AMX *, cell *params) {
	if (!CheckArgs(params, 1, "GoRadio_GetHistoryCount")) {
		return 0;
	}
	StationStatus status;
	if (!Manager::Get().GetStatus(static_cast<int>(params[1]), &status)) {
		return 0;
	}
	return ClampCell(status.history.size());
}

cell HistoryString(AMX *amx, const cell *params, int field) {
	StationStatus status;
	if (!Manager::Get().GetStatus(static_cast<int>(params[1]), &status)) {
		return WriteStringResult(amx, params, 3, 4, std::string(), false);
	}
	const TrackInfo *item = ItemAt(status.history, params[2]);
	if (item == 0) {
		return WriteStringResult(amx, params, 3, 4, std::string(), false);
	}
	const std::string *value = &item->title;
	switch (field) {
		case 1: value = &item->artist; break;
		case 2: value = &item->reason; break;
		case 3: value = &item->location; break;
		default: break;
	}
	return WriteStringResult(amx, params, 3, 4, *value, true);
}

cell AMXAPI n_GetHistoryTitle(AMX *amx, cell *params) {
	if (!CheckArgs(params, 4, "GoRadio_GetHistoryTitle")) {
		return 0;
	}
	return HistoryString(amx, params, 0);
}

cell AMXAPI n_GetHistoryArtist(AMX *amx, cell *params) {
	if (!CheckArgs(params, 4, "GoRadio_GetHistoryArtist")) {
		return 0;
	}
	return HistoryString(amx, params, 1);
}

cell AMXAPI n_GetHistoryReason(AMX *amx, cell *params) {
	if (!CheckArgs(params, 4, "GoRadio_GetHistoryReason")) {
		return 0;
	}
	return HistoryString(amx, params, 2);
}

// ---------------------------------------------------------------------
// Server-wide station listing
// ---------------------------------------------------------------------

// native GoRadio_RequestStationList();
cell AMXAPI n_RequestStationList(AMX *, cell *) { return Manager::Get().RequestStationList(); }

// native GoRadio_GetListedStationCount();
cell AMXAPI n_GetListedStationCount(AMX *, cell *) {
	return ClampCell(Manager::Get().ListedStations().size());
}

cell ListedStationString(AMX *amx, const cell *params, int field) {
	std::vector<StationSummary> listed = Manager::Get().ListedStations();
	cell index = params[1];
	if (index < 0 || static_cast<size_t>(index) >= listed.size()) {
		return WriteStringResult(amx, params, 2, 3, std::string(), false);
	}
	const StationSummary &s = listed[static_cast<size_t>(index)];
	const std::string *value = &s.slug;
	switch (field) {
		case 1: value = &s.name; break;
		case 2: value = &s.logo_url; break;
		default: break;
	}
	return WriteStringResult(amx, params, 2, 3, *value, true);
}

cell AMXAPI n_GetListedStationSlug(AMX *amx, cell *params) {
	if (!CheckArgs(params, 3, "GoRadio_GetListedStationSlug")) {
		return 0;
	}
	return ListedStationString(amx, params, 0);
}

cell AMXAPI n_GetListedStationName(AMX *amx, cell *params) {
	if (!CheckArgs(params, 3, "GoRadio_GetListedStationName")) {
		return 0;
	}
	return ListedStationString(amx, params, 1);
}

cell AMXAPI n_GetListedStationLogo(AMX *amx, cell *params) {
	if (!CheckArgs(params, 3, "GoRadio_GetListedStationLogo")) {
		return 0;
	}
	return ListedStationString(amx, params, 2);
}

// native GoRadio_GetListedListenerCount(index);
cell AMXAPI n_GetListedListenerCount(AMX *, cell *params) {
	if (!CheckArgs(params, 1, "GoRadio_GetListedListenerCount")) {
		return 0;
	}
	std::vector<StationSummary> listed = Manager::Get().ListedStations();
	cell index = params[1];
	if (index < 0 || static_cast<size_t>(index) >= listed.size()) {
		return 0;
	}
	return ClampCell(listed[static_cast<size_t>(index)].listener_count);
}

const AMX_NATIVE_INFO kNatives[] = {
    {"GoRadio_SetServer", n_SetServer},
    {"GoRadio_IsReady", n_IsReady},
    {"GoRadio_GetServerVersion", n_GetServerVersion},

    {"GoRadio_CreateStation", n_CreateStation},
    {"GoRadio_DestroyStation", n_DestroyStation},
    {"GoRadio_IsValidStation", n_IsValidStation},
    {"GoRadio_GetStationBySlug", n_GetStationBySlug},
    {"GoRadio_GetStationCount", n_GetStationCount},
    {"GoRadio_GetStationAtIndex", n_GetStationAtIndex},
    {"GoRadio_SetStationMetadata", n_SetStationMetadata},
    {"GoRadio_ClearStationMetadata", n_ClearStationMetadata},
    {"GoRadio_UpdateStation", n_UpdateStation},
    {"GoRadio_GetStationSlug", n_GetStationSlug},
    {"GoRadio_GetStreamURL", n_GetStreamURL},
    {"GoRadio_IsStationRegistered", n_IsStationRegistered},

    {"GoRadio_QueueTrack", n_QueueTrack},
    {"GoRadio_Dequeue", n_Dequeue},
    {"GoRadio_ClearQueue", n_ClearQueue},
    {"GoRadio_Skip", n_Skip},
    {"GoRadio_SkipTo", n_SkipTo},
    {"GoRadio_Pause", n_Pause},
    {"GoRadio_Resume", n_Resume},
    {"GoRadio_Seek", n_Seek},
    {"GoRadio_SeekBy", n_SeekBy},

    {"GoRadio_RefreshStatus", n_RefreshStatus},
    {"GoRadio_HasStatus", n_HasStatus},
    {"GoRadio_GetListenerCount", n_GetListenerCount},
    {"GoRadio_GetUptime", n_GetUptime},
    {"GoRadio_IsSilence", n_IsSilence},
    {"GoRadio_IsPaused", n_IsPaused},
    {"GoRadio_IsPlaying", n_IsPlaying},
    {"GoRadio_GetQueueLength", n_GetQueueLength},
    {"GoRadio_GetCurrentTrackID", n_GetCurrentTrackID},
    {"GoRadio_GetCurrentTrackTitle", n_GetCurrentTrackTitle},
    {"GoRadio_GetCurrentTrackArtist", n_GetCurrentTrackArtist},
    {"GoRadio_GetCurrentTrackLocation", n_GetCurrentTrackLocation},
    {"GoRadio_GetCurrentTrackCoverArt", n_GetCurrentTrackCoverArt},
    {"GoRadio_GetCurrentTrackDuration", n_GetCurrentTrackDuration},
    {"GoRadio_GetCurrentTrackElapsed", n_GetCurrentTrackElapsed},

    {"GoRadio_GetQueueItemCount", n_GetQueueItemCount},
    {"GoRadio_GetQueueItemID", n_GetQueueItemID},
    {"GoRadio_GetQueueItemTitle", n_GetQueueItemTitle},
    {"GoRadio_GetQueueItemArtist", n_GetQueueItemArtist},
    {"GoRadio_GetQueueItemLocation", n_GetQueueItemLocation},
    {"GoRadio_GetQueueItemDuration", n_GetQueueItemDuration},
    {"GoRadio_GetHistoryCount", n_GetHistoryCount},
    {"GoRadio_GetHistoryTitle", n_GetHistoryTitle},
    {"GoRadio_GetHistoryArtist", n_GetHistoryArtist},
    {"GoRadio_GetHistoryReason", n_GetHistoryReason},

    {"GoRadio_RequestStationList", n_RequestStationList},
    {"GoRadio_GetListedStationCount", n_GetListedStationCount},
    {"GoRadio_GetListedStationSlug", n_GetListedStationSlug},
    {"GoRadio_GetListedStationName", n_GetListedStationName},
    {"GoRadio_GetListedStationLogo", n_GetListedStationLogo},
    {"GoRadio_GetListedListenerCount", n_GetListedListenerCount},

    {0, 0}};

} // namespace

int RegisterNatives(AMX *amx) {
	int count = 0;
	while (kNatives[count].name != 0) {
		++count;
	}
	return g_amx.Register(amx, kNatives, count);
}

void DispatchPendingEvents() {
	std::vector<PawnEvent> events;
	Manager::Get().Tick(&events);

	for (size_t i = 0; i < events.size(); ++i) {
		const PawnEvent &ev = events[i];
		switch (ev.kind) {
			case kEvServerInfo:
				AmxCallback("OnGoRadioServerInfo").Str(ev.text).Invoke();
				break;

			case kEvStationRegistered:
				AmxCallback("OnGoRadioStationRegistered")
				    .Int(ev.station_id)
				    .Str(ev.text)
				    .Bool(ev.flag)
				    .Bool(ev.num1 != 0)
				    .Invoke();
				break;

			case kEvTrackStarted:
				AmxCallback("OnGoRadioTrackStarted")
				    .Int(ev.station_id)
				    .Str(ev.queue_id)
				    .Str(ev.location)
				    .Str(ev.title)
				    .Str(ev.artist)
				    .Str(ev.cover_art)
				    .Int64(ev.num1)
				    .Invoke();
				break;

			case kEvTrackEnded:
				AmxCallback("OnGoRadioTrackEnded")
				    .Int(ev.station_id)
				    .Str(ev.queue_id)
				    .Str(ev.text)
				    .Invoke();
				break;

			case kEvTrackQueued:
				AmxCallback("OnGoRadioTrackQueued")
				    .Int(ev.station_id)
				    .Int(ev.request_id)
				    .Str(ev.queue_id)
				    .Int64(ev.num2)
				    .Invoke();
				break;

			case kEvQueueLow:
				AmxCallback("OnGoRadioQueueLow")
				    .Int(ev.station_id)
				    .Int64(ev.num1)
				    .Int64(ev.num2)
				    .Invoke();
				break;

			case kEvQueueUpdated:
				AmxCallback("OnGoRadioQueueUpdated").Int(ev.station_id).Int64(ev.num1).Invoke();
				break;

			case kEvListenerCount:
				AmxCallback("OnGoRadioListenerCountChanged")
				    .Int(ev.station_id)
				    .Int64(ev.num1)
				    .Invoke();
				break;

			case kEvSilenceStarted:
				AmxCallback("OnGoRadioSilenceStarted").Int(ev.station_id).Invoke();
				break;

			case kEvSilenceEnded:
				AmxCallback("OnGoRadioSilenceEnded").Int(ev.station_id).Invoke();
				break;

			case kEvStatusUpdate:
				AmxCallback("OnGoRadioStatusUpdate").Int(ev.station_id).Invoke();
				break;

			case kEvCommandResult:
				AmxCallback("OnGoRadioCommandResult")
				    .Int(ev.station_id)
				    .Int(ev.request_id)
				    .Str(ev.text)
				    .Bool(ev.flag)
				    .Int64(ev.num1)
				    .Invoke();
				break;

			case kEvStationList:
				AmxCallback("OnGoRadioStationList").Int64(ev.num1).Invoke();
				break;

			case kEvError:
				AmxCallback("OnGoRadioError")
				    .Int(ev.station_id)
				    .Str(ev.text)
				    .Str(ev.text2)
				    .Invoke();
				break;
		}
	}
}

} // namespace goradio
