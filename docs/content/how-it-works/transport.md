# Transport — Connect over HTTP

## Why not gRPC

The audio server's API is defined as a gRPC service
(`audioserver.v1.AudioServerService`), and a generated gRPC client is the
natural way to call it. This plugin doesn't, for a reason that isn't
about preference:

SA-MP is a **32-bit** process loading a C-ABI shared library. Getting a
gRPC stack into that means a 32-bit build of gRPC, protobuf, BoringSSL and
their transitive dependencies, all statically linked into a plugin that
also has to coexist with whatever else the server loads — and HTTP/2 on
top. It is somewhere between painful and unsupportable.

The audio server anticipates this. It serves **three protocols on the same
port**, picked per request from the headers:

- gRPC (HTTP/2),
- gRPC-Web,
- the **Connect protocol** — plain HTTP/1.1 with JSON bodies.

Nothing needs enabling; it is the same port, the same handlers and the
same auth. So this plugin speaks Connect, and needs no gRPC library, no
protobuf library, no code generation, and no HTTP/2.

What that buys: the entire transport is about 2,000 lines of dependency-free
C++ — an HTTP/1.1 client, a JSON parser, and the Connect framing — which
builds anywhere a C++11 compiler runs and links against nothing but
pthreads.

## Calling an RPC

Every unary call is one POST:

```http
POST /audioserver.v1.AudioServerService/QueueTrack HTTP/1.1
Host: 127.0.0.1:9090
Content-Type: application/json
Connect-Protocol-Version: 1
Authorization: Bearer <jwt>

{"slug":"myfm","source":{"type":"TRACK_SOURCE_TYPE_LOCAL_FILE",
 "location":"music/night-drive.mp3"},"mode":"QUEUE_MODE_APPEND"}
```

```http
HTTP/1.1 200 OK
Content-Type: application/json

{"queueId":"7bd96ff0-da51-4f34-943f-e9046b94b9cb","status":"queued"}
```

The path is always the service name plus the RPC name exactly as spelled
in the `.proto`. The method is always POST, even for reads.

Connections are **keep-alive** and reused. A pooled socket the server
closed while idle fails on the write — before the request was seen — so
the plugin retries it once on a fresh connection. A failure *after* the
request went out is never retried, so a non-idempotent call can't be
applied twice.

## The four protobuf-JSON traps

These are consequences of how protobuf maps to JSON, they aren't
configurable, and every one of them has broken a hand-written client
before. They're handled in the plugin's JSON layer; they're documented
here because they explain otherwise-baffling behaviour if you ever read
the wire.

**1. 64-bit integers are JSON strings.**

```json
{"uptimeSeconds": "412", "durationSeconds": "212"}
```

Not `412`. `int32` fields (`queuePosition`, `lowQueueThreshold`) are
ordinary numbers — only the 64-bit ones are quoted. This is actually
convenient for a 32-bit client: the `*UnixMs` timestamps exceed 2³¹ and
would overflow a native 32-bit parse.

**2. Field names are `lowerCamelCase`.** The `.proto` says
`listener_count`; the wire says `listenerCount`. The server accepts either
on input but always emits camelCase.

**3. Fields at their default value are omitted entirely.** A station with
no listeners has no `listenerCount` key at all — not `"0"`. Same for
`false` booleans and empty strings and lists. A missing key is the zero
value, never an error.

**4. Enums are full strings.** `"QUEUE_MODE_APPEND"`, not `1` and not
`"APPEND"`.

## The event stream

`SubscribeEvents` is a server-streaming RPC, and it works over HTTP/1.1 —
no HTTP/2 required. The response is a **chunked** body carrying Connect
**envelopes**, and both the request message and each response message are
framed:

```
[1 byte flags][4 bytes length, big-endian][payload]
```

The request goes out with `Content-Type: application/connect+json` and its
single message enveloped with flags `0`. Then envelopes arrive as events
happen:

- **flags `0`** — a `StationEvent`; the payload is the event as JSON.
- **flags `2`** — end of stream. The payload is `{}` on a clean close, or
  `{"error":{"code":"...","message":"..."}}` if the stream is ending
  because something went wrong. No more messages follow.

A sample event, one per envelope:

```json
{"slug":"myfm","type":"EVENT_TYPE_TRACK_STARTED","timestampUnixMs":"1787586747047",
 "trackStarted":{"queueId":"15c5...","source":{"location":"music/night-drive.mp3",
 "displayTitle":"Night Drive"},"durationSeconds":"212"}}
```

Envelopes have nothing to do with TCP packets, and the plugin's reader
doesn't assume they do: several can arrive in one read, or one can be
split across several. The tests deliberately send two events glued into a
single write to keep that honest.

## Which events map to which callbacks

| Wire event | Callback |
|---|---|
| `EVENT_TYPE_TRACK_STARTED` | [`OnGoRadioTrackStarted`](../pawn-api/callbacks.md#ongoradiotrackstarted) |
| `EVENT_TYPE_TRACK_ENDED` | [`OnGoRadioTrackEnded`](../pawn-api/callbacks.md#ongoradiotrackended) |
| `EVENT_TYPE_QUEUE_UPDATED` | [`OnGoRadioQueueUpdated`](../pawn-api/callbacks.md#ongoradioqueueupdated) |
| `EVENT_TYPE_LISTENER_COUNT_CHANGED` | [`OnGoRadioListenerCountChanged`](../pawn-api/callbacks.md#ongoradiolistenercountchanged) |
| `EVENT_TYPE_QUEUE_LOW` | [`OnGoRadioQueueLow`](../pawn-api/callbacks.md#ongoradioqueuelow) |
| `EVENT_TYPE_SILENCE_STARTED` | [`OnGoRadioSilenceStarted`](../pawn-api/callbacks.md#ongoradiosilencestarted-ongoradiosilenceended) |
| `EVENT_TYPE_SILENCE_ENDED` | [`OnGoRadioSilenceEnded`](../pawn-api/callbacks.md#ongoradiosilencestarted-ongoradiosilenceended) |
| `EVENT_TYPE_ERROR` | [`OnGoRadioError`](../pawn-api/errors.md) |

## Errors

A failed RPC is a non-200 with a JSON body:

```json
{"code": "permission_denied", "message": "token is read-only"}
```

The HTTP status is derived from the code, so either can be branched on.
Full mapping and what to do about each is in
[Errors](../pawn-api/errors.md).

Two failures happen *before* the RPC is reached and come back as plain
text rather than a JSON error object. The plugin recognises both and says
something more useful than the raw response:

- **404 with a `404 page not found` body** — the RPC name in the path is
  wrong. A genuine `not_found` is also a 404, but has a JSON body; the
  body is how they're told apart.
- **405 with an empty body** — a method other than POST was used.

Neither should happen in normal operation; seeing one means the plugin and
the audio server disagree about the API, which is worth reporting.

## TLS

Plain HTTP is the default, on the assumption the audio server is on a
trusted network — often the same host. For an audio server behind a
TLS-terminating proxy, build with TLS support and use an `https://` URL;
certificates and hostnames are both verified, with SNI. See
[Building](../building/index.md#tls).

A non-TLS build given an `https://` URL refuses it and says so, rather
than quietly falling back to plaintext with your token in it.

## Reading further

The audio server's own documentation is the authority on all of this:

- **HTTP + JSON API** — the transport this plugin implements.
- **Protocol Reference** — every RPC, message and field.
- **Writing a Controller** — the expected call order and reconnect
  behaviour, which this plugin follows.
