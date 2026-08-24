#ifndef GORADIO_HTTP_H
#define GORADIO_HTTP_H

#include <string>
#include <utility>
#include <vector>

namespace goradio {

struct Url {
	std::string scheme; // "http" or "https"
	std::string host;
	int port;
	std::string base_path; // "" or a prefix like "/radio", never trailing '/'

	Url() : port(0) {}
	bool secure() const { return scheme == "https"; }
};

// Parses "http[s]://host[:port][/base/path]". A bare "host:port" with no
// scheme is accepted and treated as http, since that's the shape
// goradio's own grpc_addr config uses.
bool ParseUrl(const std::string &input, Url *out, std::string *err);

// Transport is the byte pipe under HTTP: a plain TCP socket, or an
// OpenSSL one when the plugin is built with GORADIO_TLS and the URL is
// https. Recv() returns >0 for bytes read, 0 for "nothing yet, try
// again" (the timeout elapsed), and -1 for a closed or broken
// connection with *err set.
class Transport {
public:
	virtual ~Transport() {}
	virtual bool Connect(const std::string &host, int port, int timeout_ms, std::string *err) = 0;
	virtual bool SendAll(const char *data, size_t len, std::string *err) = 0;
	virtual int Recv(char *buf, size_t len, int timeout_ms, std::string *err) = 0;
	virtual void Close() = 0;
	// Breaks a blocking Recv() from another thread. This is how the
	// long-lived event-stream reader is woken for shutdown without
	// waiting out its read timeout.
	virtual void Interrupt() = 0;
	virtual bool connected() const = 0;
};

// Creates a transport for the URL. Returns 0 with *err set if the URL is
// https and the plugin was built without TLS support.
Transport *NewTransport(const Url &url, std::string *err);

// True if this build can speak https.
bool TlsSupported();

// One-time process-wide setup (WSAStartup on Windows, OpenSSL init).
void HttpGlobalInit();
void HttpGlobalShutdown();

typedef std::vector<std::pair<std::string, std::string> > HeaderList;

struct HttpResponse {
	int status;
	HeaderList headers;
	std::string body;

	HttpResponse() : status(0) {}
	std::string Header(const std::string &name) const;
};

// A single keep-alive HTTP/1.1 connection. Not thread-safe: each worker
// and each event stream owns one.
class HttpConnection {
public:
	HttpConnection(const Url &url, int timeout_ms);
	~HttpConnection();

	// Sends a POST and reads the complete response. Reconnects and
	// retries once if the connection had gone stale -- a keep-alive
	// socket that the server (or an idle-timing-out proxy) closed
	// between requests is the normal case, not an error worth
	// surfacing.
	bool Post(const std::string &path, const HeaderList &headers, const std::string &body,
	          HttpResponse *resp, std::string *err);

	// Sends a POST and stops after the response headers, leaving the
	// body to be pulled incrementally with ReadBody() -- for
	// SubscribeEvents, whose body never ends on its own.
	bool BeginPost(const std::string &path, const HeaderList &headers, const std::string &body,
	               HttpResponse *resp, std::string *err);

	// Appends up to one transport read's worth of decoded body bytes.
	// Returns 1 if bytes were appended, 0 if the timeout elapsed with
	// nothing to read, and -1 at end of body or on error.
	int ReadBody(std::string *out, int timeout_ms, std::string *err);

	void Close();
	void Interrupt();
	bool connected() const;

	const Url &url() const { return url_; }

private:
	enum BodyMode { kBodyNone, kBodyLength, kBodyChunked, kBodyUntilClose };

	bool EnsureConnected(std::string *err);
	bool SendRequest(const std::string &path, const HeaderList &headers, const std::string &body,
	                 std::string *err);
	bool ReadStatusAndHeaders(HttpResponse *resp, std::string *err);
	// 1 = a complete line was consumed, 0 = timed out with the line
	// still incomplete (nothing is consumed), -1 = error.
	int ReadLine(std::string *line, int timeout_ms, std::string *err);
	int Fill(int timeout_ms, std::string *err);
	bool ReadFullBody(std::string *out, std::string *err);

	Url url_;
	int timeout_ms_;
	// Created in the constructor, never reassigned: Interrupt() is called
	// from a different thread than the one doing the I/O, and a pointer
	// that only ever changes before the object is shared needs no lock.
	// Null when the URL asked for TLS this build doesn't have, in which
	// case transport_err_ says so.
	Transport *transport_;
	std::string transport_err_;
	std::string buf_;   // bytes read from the socket, not yet consumed
	size_t buf_pos_;

	BodyMode body_mode_;
	long long body_remaining_; // kBodyLength: bytes left; kBodyChunked: bytes left in chunk
	bool chunk_need_size_;
	bool body_done_;
};

} // namespace goradio

#endif // GORADIO_HTTP_H
