#include "station.h"

#include <chrono>

namespace goradio {

const char *QueueModeToProto(int mode) {
	switch (mode) {
		case kQueuePlayNext:
			return "QUEUE_MODE_PLAY_NEXT";
		case kQueuePlayNowInterrupt:
			return "QUEUE_MODE_PLAY_NOW_INTERRUPT";
		case kQueueAppend:
		default:
			return "QUEUE_MODE_APPEND";
	}
}

void TrackInfo::FromJson(const JsonValue &item) {
	queue_id = item.Str("queueId");
	const JsonValue &source = item.Get("source");
	location = source.Str("location");
	title = source.Str("displayTitle");
	artist = source.Str("displayArtist");
	cover_art = source.Str("coverArtUrl");
	// 0 means unknown or indefinite -- a live relay, or an item whose
	// prefetch hasn't resolved a duration yet.
	duration_seconds = item.Int("durationSeconds", 0);
}

void Station::WaitBackoff(int ms) {
	std::unique_lock<std::mutex> lock(wait_mutex);
	wait_cv.wait_for(lock, std::chrono::milliseconds(ms), [this] { return stopping.load(); });
}

void Station::WakeBackoff() {
	std::lock_guard<std::mutex> lock(wait_mutex);
	wait_cv.notify_all();
}

std::string Station::MetadataJson() const {
	std::string out = "{";
	std::map<std::string, std::string>::const_iterator it = metadata.begin();
	for (; it != metadata.end(); ++it) {
		if (out.size() > 1) {
			out += ',';
		}
		out += '"';
		out += JsonEscape(it->first);
		out += "\":\"";
		out += JsonEscape(it->second);
		out += '"';
	}
	out += '}';
	return out;
}

std::string Station::RegisterRequestJson() const {
	JsonObject req;
	req.Str("slug", slug);
	req.Str("name", name.empty() ? slug : name);
	req.StrIfSet("description", description);
	req.StrIfSet("logoUrl", logo_url);
	if (low_queue_threshold > 0) {
		req.Int("lowQueueThreshold", low_queue_threshold);
	}
	if (!metadata.empty()) {
		req.Raw("metadata", MetadataJson());
	}
	return req.Build();
}

} // namespace goradio
