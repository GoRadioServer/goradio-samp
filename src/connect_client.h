#ifndef GORADIO_CONNECT_CLIENT_H
#define GORADIO_CONNECT_CLIENT_H

#include <string>

#include "http.h"
#include "json.h"

namespace goradio {

// The service path prefix every RPC hangs off, straight from the .proto's
// package and service name.
extern const char *const kServicePath;

// A Connect-protocol error: either one the audio server returned (a JSON
// {"code","message"} body with a matching HTTP status) or a local
// transport failure, which is reported as "unavailable" so it retries
// like any other transient condition.
struct RpcError {
	std::string code;
	std::string message;
	int http_status;

	RpcError() : http_status(0) {}

	// Retrying these can never change the outcome: the token is wrong,
	// the token doesn't cover this slug, or the request itself is
	// malformed. A controller that retries them anyway just spins
	// forever on what is really a config mistake.
	bool permanent() const {
		return code == "unauthenticated" || code == "permission_denied" ||
		       code == "invalid_argument";
	}

	std::string ToString() const { return code + ": " + message; }
};

// A Connect client over one keep-alive HTTP/1.1 connection. Not
// thread-safe -- each worker thread owns one.
class ConnectClient {
public:
	ConnectClient(const Url &url, const std::string &token, int timeout_ms);

	// Calls a unary RPC. request_json is the request message; on success
	// *out is the parsed response message (which for an all-defaults
	// response is legitimately an empty object).
	bool Call(const std::string &rpc, const std::string &request_json, JsonValue *out, RpcError *err);

	void Close() { conn_.Close(); }
	void Interrupt() { conn_.Interrupt(); }

private:
	HttpConnection conn_;
	std::string token_;
};

// A SubscribeEvents stream. The response body is a sequence of
// Connect envelopes -- [1 byte flags][4 byte big-endian length][payload]
// -- so this owns the framing as well as the connection.
class EventStream {
public:
	EventStream(const Url &url, const std::string &token, int timeout_ms);

	// Sends SubscribeEvents(slug) and reads the response headers. The
	// stream is live once this returns true.
	bool Open(const std::string &slug, RpcError *err);

	// Pulls the next event. Returns 1 with *event set, 0 if the timeout
	// elapsed with no event (normal -- a quiet station sends nothing for
	// minutes at a time), or -1 when the stream has ended, with *err
	// describing why.
	int Read(JsonValue *event, int timeout_ms, RpcError *err);

	void Close() { conn_.Close(); }
	// Safe to call from another thread: unblocks a Read() parked on the
	// socket so shutdown doesn't wait out the read timeout.
	void Interrupt() { conn_.Interrupt(); }

private:
	HttpConnection conn_;
	std::string token_;
	std::string pending_; // body bytes not yet consumed as whole envelopes
	size_t pending_pos_;
	bool ended_;
};

} // namespace goradio

#endif // GORADIO_CONNECT_CLIENT_H
