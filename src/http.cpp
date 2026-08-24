#include "http.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "log.h"

#if defined(_WIN32)
	#include <winsock2.h>
	#include <ws2tcpip.h>
	typedef SOCKET socket_t;
	#define GORADIO_INVALID_SOCKET INVALID_SOCKET
	#define GORADIO_CLOSESOCKET closesocket
	#define GORADIO_SHUT_BOTH SD_BOTH
#else
	#include <arpa/inet.h>
	#include <errno.h>
	#include <fcntl.h>
	#include <netdb.h>
	#include <netinet/in.h>
	#include <netinet/tcp.h>
	#include <signal.h>
	#include <sys/select.h>
	#include <sys/socket.h>
	#include <sys/types.h>
	#include <unistd.h>
	typedef int socket_t;
	#define GORADIO_INVALID_SOCKET (-1)
	#define GORADIO_CLOSESOCKET ::close
	#define GORADIO_SHUT_BOTH SHUT_RDWR
#endif

// Writing to a socket the peer has already closed raises SIGPIPE, whose
// default disposition kills the process -- and this plugin's process is
// the game server. It is a live path, not a corner case: the keep-alive
// retry in Post() exists precisely because a pooled connection can be
// closed between requests, and that retry's first write is the one that
// would take the server down. MSG_NOSIGNAL turns it into a plain EPIPE
// return without touching the host process's signal handling.
#if defined(MSG_NOSIGNAL)
	#define GORADIO_SEND_FLAGS MSG_NOSIGNAL
#else
	#define GORADIO_SEND_FLAGS 0
#endif

#if defined(GORADIO_TLS)
	#include <openssl/err.h>
	#include <openssl/ssl.h>
	#include <openssl/x509v3.h>
#endif

namespace goradio {
namespace {

std::string SocketErrorText() {
#if defined(_WIN32)
	int e = WSAGetLastError();
	char buf[64];
	std::snprintf(buf, sizeof(buf), "winsock error %d", e);
	return buf;
#else
	return std::strerror(errno);
#endif
}

bool WouldBlock() {
#if defined(_WIN32)
	int e = WSAGetLastError();
	return e == WSAEWOULDBLOCK || e == WSAETIMEDOUT;
#else
	return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

// setsockopt's value argument is `const void *` on POSIX but
// `const char *` on Winsock, and its length is socklen_t against int.
// Every int-valued option goes through this one wrapper so no call site
// has to remember the cast -- missing it on three of them is what broke
// the first Windows build.
int SetSockOptInt(socket_t fd, int level, int option, int value) {
	return ::setsockopt(fd, level, option, reinterpret_cast<const char *>(&value),
	                    static_cast<int>(sizeof(value)));
}

// gai_strerror is a macro on Windows that resolves to the wide-char form
// when UNICODE is defined; pin the ANSI one so this doesn't depend on how
// the consuming build happens to be configured.
std::string GaiErrorText(int rc) {
#if defined(_WIN32)
	return gai_strerrorA(rc);
#else
	return gai_strerror(rc);
#endif
}

void SetRecvTimeout(socket_t fd, int timeout_ms) {
#if defined(_WIN32)
	DWORD tv = static_cast<DWORD>(timeout_ms);
	::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&tv), sizeof(tv));
#else
	struct timeval tv;
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

void SetSendTimeout(socket_t fd, int timeout_ms) {
#if defined(_WIN32)
	DWORD tv = static_cast<DWORD>(timeout_ms);
	::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&tv), sizeof(tv));
#else
	struct timeval tv;
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

void SetBlocking(socket_t fd, bool blocking) {
#if defined(_WIN32)
	u_long mode = blocking ? 0 : 1;
	::ioctlsocket(fd, FIONBIO, &mode);
#else
	int flags = ::fcntl(fd, F_GETFL, 0);
	if (flags < 0) {
		return;
	}
	::fcntl(fd, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
#endif
}

// Enables TCP keepalives so a connection killed silently -- a NAT or load
// balancer idle timeout, a network partition, anything that never sends a
// FIN -- eventually surfaces as a read error instead of a reader parked
// forever. The event stream can legitimately sit idle for minutes between
// events, so a read timeout can't do this job on its own.
void EnableKeepalive(socket_t fd) {
	SetSockOptInt(fd, SOL_SOCKET, SO_KEEPALIVE, 1);

	// Guarded one at a time rather than as a group: which of these three a
	// platform spells varies, and Windows gained TCP_KEEPCNT in a later
	// SDK than TCP_KEEPIDLE, so a single guard around all three would
	// break on an older one.
#if defined(TCP_KEEPIDLE)
	SetSockOptInt(fd, IPPROTO_TCP, TCP_KEEPIDLE, 20);
#endif
#if defined(TCP_KEEPINTVL)
	SetSockOptInt(fd, IPPROTO_TCP, TCP_KEEPINTVL, 10);
#endif
#if defined(TCP_KEEPCNT)
	SetSockOptInt(fd, IPPROTO_TCP, TCP_KEEPCNT, 3);
#endif

	SetSockOptInt(fd, IPPROTO_TCP, TCP_NODELAY, 1);
}

// Opens a TCP connection with a bounded connect time. A blocking connect()
// to an unreachable host can hang for over a minute on some systems, which
// on a shared worker thread would stall every other station's calls too.
socket_t DialTcp(const std::string &host, int port, int timeout_ms, std::string *err) {
	char port_str[16];
	std::snprintf(port_str, sizeof(port_str), "%d", port);

	struct addrinfo hints;
	std::memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	struct addrinfo *res = 0;
	int rc = ::getaddrinfo(host.c_str(), port_str, &hints, &res);
	if (rc != 0 || res == 0) {
		*err = "resolve " + host + " failed: " + GaiErrorText(rc);
		return GORADIO_INVALID_SOCKET;
	}

	socket_t fd = GORADIO_INVALID_SOCKET;
	std::string last_err = "no addresses returned";
	for (struct addrinfo *ai = res; ai != 0; ai = ai->ai_next) {
		fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd == GORADIO_INVALID_SOCKET) {
			last_err = SocketErrorText();
			continue;
		}

		SetBlocking(fd, false);
		int cr = ::connect(fd, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
		bool ok = (cr == 0);
		if (!ok) {
			fd_set wfds;
			FD_ZERO(&wfds);
			FD_SET(fd, &wfds);
			struct timeval tv;
			tv.tv_sec = timeout_ms / 1000;
			tv.tv_usec = (timeout_ms % 1000) * 1000;
#if defined(_WIN32)
			const int nfds = 0; // ignored by Winsock
#else
			const int nfds = static_cast<int>(fd) + 1;
#endif
			int sel = ::select(nfds, 0, &wfds, 0, &tv);
			if (sel > 0) {
				int soerr = 0;
				socklen_t len = sizeof(soerr);
				if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&soerr), &len) == 0 &&
				    soerr == 0) {
					ok = true;
				} else {
					last_err = "connection refused";
				}
			} else if (sel == 0) {
				last_err = "connect timed out";
			} else {
				last_err = SocketErrorText();
			}
		}

		if (ok) {
			SetBlocking(fd, true);
			EnableKeepalive(fd);
#if defined(SO_NOSIGPIPE)
			// macOS/BSD have no MSG_NOSIGNAL; this is the per-socket
			// equivalent, and it also covers writes OpenSSL makes itself.
			SetSockOptInt(fd, SOL_SOCKET, SO_NOSIGPIPE, 1);
#endif
			SetSendTimeout(fd, timeout_ms);
			break;
		}
		GORADIO_CLOSESOCKET(fd);
		fd = GORADIO_INVALID_SOCKET;
	}
	::freeaddrinfo(res);

	if (fd == GORADIO_INVALID_SOCKET) {
		*err = "connect " + host + ":" + port_str + " failed: " + last_err;
	}
	return fd;
}

class PlainTransport : public Transport {
public:
	PlainTransport() : fd_(GORADIO_INVALID_SOCKET), recv_timeout_(-1) {}
	virtual ~PlainTransport() { PlainTransport::Close(); }

	virtual bool Connect(const std::string &host, int port, int timeout_ms, std::string *err) {
		Close();
		socket_t fd = DialTcp(host, port, timeout_ms, err);
		if (fd == GORADIO_INVALID_SOCKET) {
			return false;
		}
		std::lock_guard<std::mutex> lock(mutex_);
		fd_ = fd;
		recv_timeout_ = -1;
		return true;
	}

	virtual bool SendAll(const char *data, size_t len, std::string *err) {
		size_t sent = 0;
		while (sent < len) {
			socket_t fd = fd_;
			if (fd == GORADIO_INVALID_SOCKET) {
				*err = "not connected";
				return false;
			}
			int n = static_cast<int>(
			    ::send(fd, data + sent, static_cast<int>(len - sent), GORADIO_SEND_FLAGS));
			if (n > 0) {
				sent += static_cast<size_t>(n);
				continue;
			}
			if (n < 0 && WouldBlock()) {
				continue;
			}
			*err = "send failed: " + SocketErrorText();
			return false;
		}
		return true;
	}

	virtual int Recv(char *buf, size_t len, int timeout_ms, std::string *err) {
		socket_t fd = fd_;
		if (fd == GORADIO_INVALID_SOCKET) {
			*err = "not connected";
			return -1;
		}
		if (timeout_ms != recv_timeout_) {
			SetRecvTimeout(fd, timeout_ms);
			recv_timeout_ = timeout_ms;
		}
		int n = static_cast<int>(::recv(fd, buf, static_cast<int>(len), 0));
		if (n > 0) {
			return n;
		}
		if (n == 0) {
			*err = "connection closed by peer";
			return -1;
		}
		if (WouldBlock()) {
			return 0;
		}
		*err = "recv failed: " + SocketErrorText();
		return -1;
	}

	virtual void Close() {
		std::lock_guard<std::mutex> lock(mutex_);
		if (fd_ != GORADIO_INVALID_SOCKET) {
			GORADIO_CLOSESOCKET(fd_);
			fd_ = GORADIO_INVALID_SOCKET;
		}
	}

	virtual void Interrupt() {
		std::lock_guard<std::mutex> lock(mutex_);
		if (fd_ != GORADIO_INVALID_SOCKET) {
			::shutdown(fd_, GORADIO_SHUT_BOTH);
		}
	}

	virtual bool connected() const { return fd_ != GORADIO_INVALID_SOCKET; }

protected:
	socket_t fd_;
	int recv_timeout_;
	std::mutex mutex_;
};

#if defined(GORADIO_TLS)

class TlsTransport : public PlainTransport {
public:
	TlsTransport() : ctx_(0), ssl_(0) {}
	virtual ~TlsTransport() { TlsTransport::Close(); }

	virtual bool Connect(const std::string &host, int port, int timeout_ms, std::string *err) {
		if (!PlainTransport::Connect(host, port, timeout_ms, err)) {
			return false;
		}
		ctx_ = SSL_CTX_new(SSLv23_client_method());
		if (ctx_ == 0) {
			*err = "SSL_CTX_new failed";
			Close();
			return false;
		}
		SSL_CTX_set_options(ctx_, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
		SSL_CTX_set_default_verify_paths(ctx_);
		SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, 0);

		ssl_ = SSL_new(ctx_);
		if (ssl_ == 0) {
			*err = "SSL_new failed";
			Close();
			return false;
		}
		// SNI plus hostname verification: without both, a connection to a
		// virtual-hosted server either lands on the wrong certificate or
		// accepts whichever certificate it is handed.
		SSL_set_tlsext_host_name(ssl_, host.c_str());
#if OPENSSL_VERSION_NUMBER >= 0x10002000L
		X509_VERIFY_PARAM *param = SSL_get0_param(ssl_);
		X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
		X509_VERIFY_PARAM_set1_host(param, host.c_str(), 0);
#endif
		SSL_set_fd(ssl_, static_cast<int>(fd_));
		SetRecvTimeout(fd_, timeout_ms);
		recv_timeout_ = timeout_ms;
		if (SSL_connect(ssl_) != 1) {
			unsigned long e = ERR_get_error();
			char ebuf[256];
			ERR_error_string_n(e, ebuf, sizeof(ebuf));
			*err = std::string("TLS handshake failed: ") + ebuf;
			Close();
			return false;
		}
		return true;
	}

	virtual bool SendAll(const char *data, size_t len, std::string *err) {
		size_t sent = 0;
		while (sent < len) {
			if (ssl_ == 0) {
				*err = "not connected";
				return false;
			}
			int n = SSL_write(ssl_, data + sent, static_cast<int>(len - sent));
			if (n > 0) {
				sent += static_cast<size_t>(n);
				continue;
			}
			int e = SSL_get_error(ssl_, n);
			if (e == SSL_ERROR_WANT_WRITE || e == SSL_ERROR_WANT_READ) {
				continue;
			}
			*err = "TLS write failed";
			return false;
		}
		return true;
	}

	virtual int Recv(char *buf, size_t len, int timeout_ms, std::string *err) {
		if (ssl_ == 0) {
			*err = "not connected";
			return -1;
		}
		if (timeout_ms != recv_timeout_) {
			SetRecvTimeout(fd_, timeout_ms);
			recv_timeout_ = timeout_ms;
		}
		int n = SSL_read(ssl_, buf, static_cast<int>(len));
		if (n > 0) {
			return n;
		}
		int e = SSL_get_error(ssl_, n);
		if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
			// The socket timeout elapsed mid-record; nothing to report.
			return 0;
		}
		if (e == SSL_ERROR_ZERO_RETURN) {
			*err = "connection closed by peer";
			return -1;
		}
		if (e == SSL_ERROR_SYSCALL && WouldBlock()) {
			return 0;
		}
		*err = "TLS read failed";
		return -1;
	}

	virtual void Close() {
		if (ssl_ != 0) {
			SSL_free(ssl_);
			ssl_ = 0;
		}
		if (ctx_ != 0) {
			SSL_CTX_free(ctx_);
			ctx_ = 0;
		}
		PlainTransport::Close();
	}

private:
	SSL_CTX *ctx_;
	SSL *ssl_;
};

#endif // GORADIO_TLS

} // namespace

bool TlsSupported() {
#if defined(GORADIO_TLS)
	return true;
#else
	return false;
#endif
}

void HttpGlobalInit() {
#if defined(_WIN32)
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
#if defined(GORADIO_TLS) && OPENSSL_VERSION_NUMBER < 0x10100000L
	SSL_library_init();
	SSL_load_error_strings();
#endif
#if defined(GORADIO_TLS) && !defined(_WIN32) && !defined(SO_NOSIGPIPE)
	// OpenSSL writes through the socket itself, so MSG_NOSIGNAL never
	// reaches those calls and Linux offers no per-socket alternative --
	// ignoring the signal process-wide is the only option left. Done only
	// if nothing has claimed SIGPIPE already, so a host that installed its
	// own handler keeps it.
	struct sigaction current;
	if (::sigaction(SIGPIPE, 0, &current) == 0 && current.sa_handler == SIG_DFL) {
		struct sigaction ignore;
		std::memset(&ignore, 0, sizeof(ignore));
		ignore.sa_handler = SIG_IGN;
		::sigaction(SIGPIPE, &ignore, 0);
	}
#endif
}

void HttpGlobalShutdown() {
#if defined(_WIN32)
	WSACleanup();
#endif
}

Transport *NewTransport(const Url &url, std::string *err) {
	if (url.secure()) {
#if defined(GORADIO_TLS)
		return new TlsTransport();
#else
		*err =
		    "this build has no TLS support, so it cannot use an https:// audio server URL "
		    "(rebuild with -DGORADIO_TLS=ON, or point goradio_url at a plain http:// address)";
		return 0;
#endif
	}
	return new PlainTransport();
}

bool ParseUrl(const std::string &input, Url *out, std::string *err) {
	std::string s = input;
	while (!s.empty() && (s[s.size() - 1] == ' ' || s[s.size() - 1] == '/')) {
		s.erase(s.size() - 1);
	}
	if (s.empty()) {
		*err = "empty URL";
		return false;
	}

	out->scheme = "http";
	size_t scheme_end = s.find("://");
	if (scheme_end != std::string::npos) {
		out->scheme = s.substr(0, scheme_end);
		s = s.substr(scheme_end + 3);
		if (out->scheme != "http" && out->scheme != "https") {
			*err = "unsupported URL scheme \"" + out->scheme + "\" (expected http or https)";
			return false;
		}
	}

	std::string hostport = s;
	size_t slash = s.find('/');
	if (slash != std::string::npos) {
		hostport = s.substr(0, slash);
		out->base_path = s.substr(slash);
	} else {
		out->base_path.clear();
	}

	out->port = out->secure() ? 443 : 80;
	// Split on the last colon, and only when what follows is numeric: an
	// IPv6 literal is full of colons and must keep them.
	size_t colon = hostport.rfind(':');
	if (colon != std::string::npos && hostport.find(']') == std::string::npos) {
		std::string port_str = hostport.substr(colon + 1);
		bool numeric = !port_str.empty();
		for (size_t i = 0; i < port_str.size(); ++i) {
			if (port_str[i] < '0' || port_str[i] > '9') {
				numeric = false;
				break;
			}
		}
		if (numeric) {
			out->port = std::atoi(port_str.c_str());
			hostport = hostport.substr(0, colon);
		}
	}
	if (hostport.size() >= 2 && hostport[0] == '[' && hostport[hostport.size() - 1] == ']') {
		hostport = hostport.substr(1, hostport.size() - 2);
	}
	if (hostport.empty()) {
		*err = "URL has no host";
		return false;
	}
	if (out->port <= 0 || out->port > 65535) {
		*err = "URL has an invalid port";
		return false;
	}
	out->host = hostport;
	return true;
}

std::string HttpResponse::Header(const std::string &name) const {
	for (size_t i = 0; i < headers.size(); ++i) {
		if (headers[i].first == name) {
			return headers[i].second;
		}
	}
	return std::string();
}

HttpConnection::HttpConnection(const Url &url, int timeout_ms)
    : url_(url),
      timeout_ms_(timeout_ms),
      transport_(0),
      buf_pos_(0),
      body_mode_(kBodyNone),
      body_remaining_(0),
      chunk_need_size_(true),
      body_done_(true) {
	// Built here rather than lazily on first use: Interrupt() runs on a
	// different thread than the I/O, and this way the pointer it reads is
	// set once, before the connection is ever shared.
	transport_ = NewTransport(url, &transport_err_);
}

HttpConnection::~HttpConnection() {
	Close();
	delete transport_;
	transport_ = 0;
}

bool HttpConnection::connected() const {
	return transport_ != 0 && transport_->connected();
}

void HttpConnection::Close() {
	if (transport_ != 0) {
		transport_->Close();
	}
	buf_.clear();
	buf_pos_ = 0;
	body_mode_ = kBodyNone;
	body_done_ = true;
}

void HttpConnection::Interrupt() {
	if (transport_ != 0) {
		transport_->Interrupt();
	}
}

bool HttpConnection::EnsureConnected(std::string *err) {
	if (transport_ == 0) {
		*err = transport_err_;
		return false;
	}
	if (transport_->connected()) {
		return true;
	}
	buf_.clear();
	buf_pos_ = 0;
	return transport_->Connect(url_.host, url_.port, timeout_ms_, err);
}

bool HttpConnection::SendRequest(const std::string &path, const HeaderList &headers,
                                 const std::string &body, std::string *err) {
	char len_buf[32];
	std::snprintf(len_buf, sizeof(len_buf), "%lu", static_cast<unsigned long>(body.size()));

	std::string host_header = url_.host;
	// Include the port only when it isn't the scheme default -- some
	// reverse proxies match virtual hosts on the literal Host value.
	if (!((url_.secure() && url_.port == 443) || (!url_.secure() && url_.port == 80))) {
		char port_buf[16];
		std::snprintf(port_buf, sizeof(port_buf), ":%d", url_.port);
		host_header += port_buf;
	}

	std::string req = "POST " + url_.base_path + path + " HTTP/1.1\r\n";
	req += "Host: " + host_header + "\r\n";
	req += "User-Agent: goradio-samp/1.0\r\n";
	req += "Connection: keep-alive\r\n";
	req += "Content-Length: ";
	req += len_buf;
	req += "\r\n";
	for (size_t i = 0; i < headers.size(); ++i) {
		req += headers[i].first + ": " + headers[i].second + "\r\n";
	}
	req += "\r\n";
	req += body;

	return transport_->SendAll(req.data(), req.size(), err);
}

int HttpConnection::Fill(int timeout_ms, std::string *err) {
	// Reclaim the consumed prefix once it's worth the copy, so a
	// long-lived event stream doesn't grow its buffer without bound.
	if (buf_pos_ > 8192) {
		buf_.erase(0, buf_pos_);
		buf_pos_ = 0;
	}
	char chunk[4096];
	int n = transport_->Recv(chunk, sizeof(chunk), timeout_ms, err);
	if (n > 0) {
		buf_.append(chunk, static_cast<size_t>(n));
	}
	return n;
}

int HttpConnection::ReadLine(std::string *line, int timeout_ms, std::string *err) {
	for (;;) {
		size_t nl = buf_.find('\n', buf_pos_);
		if (nl != std::string::npos) {
			size_t end = nl;
			if (end > buf_pos_ && buf_[end - 1] == '\r') {
				--end;
			}
			line->assign(buf_, buf_pos_, end - buf_pos_);
			buf_pos_ = nl + 1;
			return 1;
		}
		int n = Fill(timeout_ms, err);
		if (n < 0) {
			return -1;
		}
		if (n == 0) {
			return 0;
		}
	}
}

bool HttpConnection::ReadStatusAndHeaders(HttpResponse *resp, std::string *err) {
	std::string line;
	int r = ReadLine(&line, timeout_ms_, err);
	if (r <= 0) {
		if (r == 0) {
			*err = "timed out waiting for response";
		}
		return false;
	}
	// "HTTP/1.1 200 OK"
	size_t sp = line.find(' ');
	if (line.compare(0, 5, "HTTP/") != 0 || sp == std::string::npos) {
		*err = "malformed status line: " + line;
		return false;
	}
	resp->status = std::atoi(line.c_str() + sp + 1);

	resp->headers.clear();
	std::string content_length;
	std::string transfer_encoding;
	for (;;) {
		r = ReadLine(&line, timeout_ms_, err);
		if (r <= 0) {
			if (r == 0) {
				*err = "timed out reading response headers";
			}
			return false;
		}
		if (line.empty()) {
			break;
		}
		size_t colon = line.find(':');
		if (colon == std::string::npos) {
			continue;
		}
		std::string name = line.substr(0, colon);
		std::string value = line.substr(colon + 1);
		while (!value.empty() && (value[0] == ' ' || value[0] == '\t')) {
			value.erase(0, 1);
		}
		while (!value.empty() && (value[value.size() - 1] == '\r' || value[value.size() - 1] == ' ')) {
			value.erase(value.size() - 1);
		}
		for (size_t i = 0; i < name.size(); ++i) {
			if (name[i] >= 'A' && name[i] <= 'Z') {
				name[i] = static_cast<char>(name[i] - 'A' + 'a');
			}
		}
		if (name == "content-length") {
			content_length = value;
		} else if (name == "transfer-encoding") {
			transfer_encoding = value;
		}
		resp->headers.push_back(std::make_pair(name, value));
	}

	body_done_ = false;
	chunk_need_size_ = true;
	body_remaining_ = 0;
	if (transfer_encoding.find("chunked") != std::string::npos) {
		body_mode_ = kBodyChunked;
	} else if (!content_length.empty()) {
		body_mode_ = kBodyLength;
		body_remaining_ = std::strtoll(content_length.c_str(), 0, 10);
		if (body_remaining_ <= 0) {
			body_mode_ = kBodyNone;
			body_done_ = true;
		}
	} else if (resp->status == 204 || resp->status == 304) {
		body_mode_ = kBodyNone;
		body_done_ = true;
	} else {
		body_mode_ = kBodyUntilClose;
	}
	return true;
}

int HttpConnection::ReadBody(std::string *out, int timeout_ms, std::string *err) {
	if (body_done_) {
		return -1;
	}
	switch (body_mode_) {
		case kBodyNone:
			body_done_ = true;
			return -1;

		case kBodyLength: {
			size_t available = buf_.size() - buf_pos_;
			if (available == 0) {
				int n = Fill(timeout_ms, err);
				if (n < 0) {
					return -1;
				}
				if (n == 0) {
					return 0;
				}
				available = buf_.size() - buf_pos_;
			}
			size_t take = available;
			if (static_cast<long long>(take) > body_remaining_) {
				take = static_cast<size_t>(body_remaining_);
			}
			out->append(buf_, buf_pos_, take);
			buf_pos_ += take;
			body_remaining_ -= static_cast<long long>(take);
			if (body_remaining_ <= 0) {
				body_done_ = true;
			}
			return 1;
		}

		case kBodyChunked: {
			if (chunk_need_size_) {
				std::string line;
				// The CRLF terminating the previous chunk's data shows up
				// here as an empty line; a size line is never empty, so
				// skipping blanks handles it without extra state.
				for (;;) {
					int r = ReadLine(&line, timeout_ms, err);
					if (r < 0) {
						return -1;
					}
					if (r == 0) {
						return 0;
					}
					if (!line.empty()) {
						break;
					}
				}
				size_t semi = line.find(';');
				if (semi != std::string::npos) {
					line = line.substr(0, semi);
				}
				long long size = static_cast<long long>(std::strtoll(line.c_str(), 0, 16));
				if (size <= 0) {
					body_done_ = true;
					return -1;
				}
				body_remaining_ = size;
				chunk_need_size_ = false;
			}
			size_t available = buf_.size() - buf_pos_;
			if (available == 0) {
				int n = Fill(timeout_ms, err);
				if (n < 0) {
					return -1;
				}
				if (n == 0) {
					return 0;
				}
				available = buf_.size() - buf_pos_;
			}
			size_t take = available;
			if (static_cast<long long>(take) > body_remaining_) {
				take = static_cast<size_t>(body_remaining_);
			}
			out->append(buf_, buf_pos_, take);
			buf_pos_ += take;
			body_remaining_ -= static_cast<long long>(take);
			if (body_remaining_ <= 0) {
				chunk_need_size_ = true;
			}
			return 1;
		}

		case kBodyUntilClose:
		default: {
			size_t available = buf_.size() - buf_pos_;
			if (available == 0) {
				int n = Fill(timeout_ms, err);
				if (n < 0) {
					// With no framing to say where the body ends, a close
					// *is* the end -- not a failure.
					body_done_ = true;
					err->clear();
					return -1;
				}
				if (n == 0) {
					return 0;
				}
				available = buf_.size() - buf_pos_;
			}
			out->append(buf_, buf_pos_, available);
			buf_pos_ += available;
			return 1;
		}
	}
}

bool HttpConnection::ReadFullBody(std::string *out, std::string *err) {
	for (;;) {
		int r = ReadBody(out, timeout_ms_, err);
		if (r == 1) {
			continue;
		}
		if (r == 0) {
			*err = "timed out reading response body";
			return false;
		}
		if (body_done_) {
			return true;
		}
		return false;
	}
}

bool HttpConnection::BeginPost(const std::string &path, const HeaderList &headers,
                               const std::string &body, HttpResponse *resp, std::string *err) {
	if (!EnsureConnected(err)) {
		return false;
	}
	if (!SendRequest(path, headers, body, err)) {
		Close();
		return false;
	}
	if (!ReadStatusAndHeaders(resp, err)) {
		Close();
		return false;
	}
	return true;
}

bool HttpConnection::Post(const std::string &path, const HeaderList &headers,
                          const std::string &body, HttpResponse *resp, std::string *err) {
	// Two attempts, but only because of keep-alive: a pooled socket the
	// server closed while idle fails on the *write*, before the request
	// has been seen, so resending it is safe even for non-idempotent
	// RPCs. A failure after the request went out is never retried here.
	for (int attempt = 0; attempt < 2; ++attempt) {
		bool was_connected = connected();
		if (!EnsureConnected(err)) {
			return false;
		}
		*resp = HttpResponse();
		if (!SendRequest(path, headers, body, err)) {
			Close();
			if (was_connected && attempt == 0) {
				continue;
			}
			return false;
		}
		if (!ReadStatusAndHeaders(resp, err)) {
			Close();
			if (was_connected && attempt == 0) {
				continue;
			}
			return false;
		}
		if (!ReadFullBody(&resp->body, err)) {
			Close();
			return false;
		}
		if (resp->Header("connection").find("close") != std::string::npos) {
			Close();
		}
		return true;
	}
	return false;
}

} // namespace goradio
