#include "connect_client.h"

#include <cstring>

#include "log.h"

namespace goradio {

const char *const kServicePath = "/audioserver.v1.AudioServerService/";

namespace {

// Turns a failed HTTP response into an RpcError. A Connect error is a
// non-200 whose body is {"code","message"} JSON -- but two failures happen
// before the RPC is ever reached and come back as plain text instead: a
// 404 for a mistyped RPC name, and a 405 for a non-POST. Recognising those
// separately is the difference between "your path is wrong" and a
// genuinely NotFound station.
RpcError ErrorFromResponse(const HttpResponse &resp) {
	RpcError err;
	err.http_status = resp.status;

	JsonValue body;
	std::string parse_err;
	if (JsonValue::Parse(resp.body, &body, &parse_err) && body.IsObject() && body.Has("code")) {
		err.code = body.Str("code");
		err.message = body.Str("message");
		return err;
	}

	if (resp.status == 404) {
		err.code = "internal";
		err.message = "no such RPC on the audio server (check the plugin's service path)";
	} else if (resp.status == 405) {
		err.code = "internal";
		err.message = "audio server rejected the request method";
	} else {
		err.code = "internal";
		std::string body_text = resp.body;
		if (body_text.size() > 200) {
			body_text = body_text.substr(0, 200) + "...";
		}
		err.message = Format("HTTP %d from audio server: %s", resp.status, body_text.c_str());
	}
	return err;
}

RpcError TransportError(const std::string &message) {
	RpcError err;
	err.code = "unavailable";
	err.message = message;
	return err;
}

// Connect envelope: one flags byte, then a big-endian uint32 length.
std::string Envelope(const std::string &payload) {
	std::string out;
	out.reserve(payload.size() + 5);
	out.push_back(0);
	unsigned long n = static_cast<unsigned long>(payload.size());
	out.push_back(static_cast<char>((n >> 24) & 0xFF));
	out.push_back(static_cast<char>((n >> 16) & 0xFF));
	out.push_back(static_cast<char>((n >> 8) & 0xFF));
	out.push_back(static_cast<char>(n & 0xFF));
	out += payload;
	return out;
}

} // namespace

ConnectClient::ConnectClient(const Url &url, const std::string &token, int timeout_ms)
    : conn_(url, timeout_ms), token_(token) {}

bool ConnectClient::Call(const std::string &rpc, const std::string &request_json, JsonValue *out,
                         RpcError *err) {
	HeaderList headers;
	headers.push_back(std::make_pair(std::string("Content-Type"), std::string("application/json")));
	headers.push_back(std::make_pair(std::string("Connect-Protocol-Version"), std::string("1")));
	headers.push_back(std::make_pair(std::string("Authorization"), "Bearer " + token_));

	HttpResponse resp;
	std::string transport_err;
	LogDebug(Format("-> %s %s", rpc.c_str(), request_json.c_str()));
	if (!conn_.Post(kServicePath + rpc, headers, request_json, &resp, &transport_err)) {
		*err = TransportError(transport_err);
		return false;
	}
	LogDebug(Format("<- %s HTTP %d %s", rpc.c_str(), resp.status, resp.body.c_str()));

	if (resp.status != 200) {
		*err = ErrorFromResponse(resp);
		return false;
	}

	std::string parse_err;
	if (!JsonValue::Parse(resp.body, out, &parse_err)) {
		*err = TransportError("could not parse " + rpc + " response: " + parse_err);
		return false;
	}
	return true;
}

EventStream::EventStream(const Url &url, const std::string &token, int timeout_ms)
    : conn_(url, timeout_ms), token_(token), pending_pos_(0), ended_(true) {}

bool EventStream::Open(const std::string &slug, RpcError *err) {
	pending_.clear();
	pending_pos_ = 0;
	ended_ = false;

	JsonObject req;
	req.Str("slug", slug);
	std::string body = Envelope(req.Build());

	HeaderList headers;
	headers.push_back(
	    std::make_pair(std::string("Content-Type"), std::string("application/connect+json")));
	headers.push_back(std::make_pair(std::string("Connect-Protocol-Version"), std::string("1")));
	headers.push_back(std::make_pair(std::string("Authorization"), "Bearer " + token_));

	HttpResponse resp;
	std::string transport_err;
	if (!conn_.BeginPost(kServicePath + std::string("SubscribeEvents"), headers, body, &resp,
	                     &transport_err)) {
		ended_ = true;
		*err = TransportError(transport_err);
		return false;
	}
	if (resp.status != 200) {
		// A rejected stream answers like a unary call: the error body has
		// already been read as part of the response.
		std::string rest;
		std::string ignored;
		while (conn_.ReadBody(&rest, 1000, &ignored) == 1) {
		}
		HttpResponse full = resp;
		full.body += rest;
		ended_ = true;
		*err = ErrorFromResponse(full);
		conn_.Close();
		return false;
	}
	return true;
}

int EventStream::Read(JsonValue *event, int timeout_ms, RpcError *err) {
	for (;;) {
		// Do we already have a whole envelope buffered?
		size_t available = pending_.size() - pending_pos_;
		if (available >= 5) {
			const unsigned char *p =
			    reinterpret_cast<const unsigned char *>(pending_.data()) + pending_pos_;
			unsigned char flags = p[0];
			unsigned long length = (static_cast<unsigned long>(p[1]) << 24) |
			                       (static_cast<unsigned long>(p[2]) << 16) |
			                       (static_cast<unsigned long>(p[3]) << 8) |
			                       static_cast<unsigned long>(p[4]);
			if (available >= 5 + length) {
				std::string payload(pending_, pending_pos_ + 5, length);
				pending_pos_ += 5 + length;
				if (pending_pos_ > 65536) {
					pending_.erase(0, pending_pos_);
					pending_pos_ = 0;
				}

				// Flag bit 1 marks the end-of-stream envelope: "{}" for a
				// clean close, or {"error":{...}} when the server is
				// ending the stream because something went wrong.
				if ((flags & 0x02) != 0) {
					ended_ = true;
					JsonValue eos;
					std::string parse_err;
					if (JsonValue::Parse(payload, &eos, &parse_err) && eos.Has("error")) {
						const JsonValue &e = eos.Get("error");
						err->code = e.Str("code", "internal");
						err->message = e.Str("message", "stream ended with an error");
						err->http_status = 200;
					} else {
						*err = TransportError("event stream closed by the audio server");
					}
					return -1;
				}

				std::string parse_err;
				if (!JsonValue::Parse(payload, event, &parse_err)) {
					// One unreadable frame means the framing itself is
					// suspect, so drop the stream and let the caller
					// reconnect rather than trying to resynchronise.
					ended_ = true;
					*err = TransportError("could not parse event: " + parse_err);
					return -1;
				}
				return 1;
			}
		}

		if (ended_) {
			*err = TransportError("event stream ended");
			return -1;
		}

		std::string transport_err;
		int r = conn_.ReadBody(&pending_, timeout_ms, &transport_err);
		if (r == 0) {
			return 0;
		}
		if (r < 0) {
			ended_ = true;
			*err = TransportError(transport_err.empty() ? "event stream ended" : transport_err);
			return -1;
		}
	}
}

} // namespace goradio
