# Configuration

Everything is configured in `server.cfg`. SA-MP gives plugins no API for
reading it, so the plugin parses the file directly at load time — it is a
flat `key value` list, with the value running to the end of the line.

```
plugins goradio.so

goradio_url            http://127.0.0.1:9090
goradio_token          eyJhbGciOiJIUzI1NiIs...
goradio_poll_interval  5
goradio_workers        2
goradio_timeout        10000
goradio_debug          0
```

## Settings

### `goradio_url`

**Default:** `http://127.0.0.1:9090`

The audio server's address — the same `grpc.listen_addr` it logs at
startup. The Connect protocol this plugin uses shares that port with gRPC;
there is no separate HTTP port to enable.

Accepted forms:

| Form | Meaning |
|---|---|
| `http://host:9090` | Plain HTTP. |
| `host:9090` | The same thing; a missing scheme means `http`. |
| `https://radio.example.com` | TLS. Needs a [TLS build](../building/index.md#tls); port defaults to 443. |
| `http://host:9090/prefix` | A path prefix, for a reverse proxy that mounts the server under a subpath. |

A non-TLS build given an `https://` URL refuses it with an explanation
rather than silently falling back to plaintext.

### `goradio_token`

**Default:** none — required.

A JWT from `radio tokengen`. It must have **write** scope and must cover
every slug you intend to register. Without a token the plugin loads but
stays idle, logging:

```
[goradio] WARN: goradio_token is not set in server.cfg -- call GoRadio_SetServer
          from your script before creating stations
```

### `goradio_poll_interval`

**Default:** `5` (seconds)

How often the plugin refreshes each station's cached status. This backs
the `GoRadio_Get*` natives — listener count, uptime, the queue snapshot.

`0` disables the timer, but not snapshots altogether: the plugin also
refreshes a station's status whenever a track starts or ends, and once
after every registration. So with polling off, the queue and history
lists still update at every track change — what goes stale is only what
changes between tracks, most visibly `GoRadio_GetUptime`.

Polling is cheap: one small HTTP request per station per interval, on a
worker thread. There is little reason to raise this above 10s or lower it
below 2s.

### `goradio_workers`

**Default:** `2`

How many threads run outgoing calls. Each holds one keep-alive connection
to the audio server, so this is also the number of RPCs that can be in
flight at once.

Two is enough for most servers. Raise it if you drive many stations that
queue in bursts; there is no benefit in going beyond a handful, since the
audio server answers these calls in milliseconds.

This does not include the per-station event stream threads, which are
separate and not part of the pool — see
[Architecture](../how-it-works/architecture.md).

### `goradio_timeout`

**Default:** `10000` (milliseconds)

Per-request timeout: connect, send, and each read. It bounds how long a
worker can be tied up by an unresponsive server, and how long shutdown
can take in the worst case.

It is **not** a limit on the event stream, which stays open indefinitely
by design. A dead stream is detected by TCP keepalive instead.

### `goradio_debug`

**Default:** `0`

`1` logs every request and response body to the server log. Useful when a
station won't register or an RPC does something unexpected; noisy enough
that you don't want it on permanently.

```
[goradio] debug: -> QueueTrack {"slug":"myfm","source":{...},"mode":"QUEUE_MODE_APPEND"}
[goradio] debug: <- QueueTrack HTTP 200 {"queueId":"7bd96ff0-...","status":"queued"}
```

!!! warning "Debug logging includes request bodies, not headers"
    Your token is sent in an `Authorization` header, which is never
    logged. Request and response *bodies* are, so debug output is safe to
    paste into an issue — but read it first if your track metadata
    contains anything you'd rather not share.

## Configuring from a script instead

If your settings come from somewhere else — a database, a per-server
config file, an environment your host injects — use
[`GoRadio_SetServer`](../pawn-api/index.md#goradio_setserver):

```pawn
public OnGameModeInit()
{
    GoRadio_SetServer("http://127.0.0.1:9090", GetMyTokenFromSomewhere());
    // ... then create stations
    return 1;
}
```

It must be called **before any station is created**. Re-pointing the
plugin while stations are registered is refused: those registrations
belong to the old server, and silently moving would leave stations
running somewhere nothing is watching them.

Prefer `server.cfg` where you can. A token hardcoded in a gamemode is a
token in your source control.

## Precedence

`server.cfg` is read once at plugin load. `GoRadio_SetServer` overrides
whatever it found, provided no station exists yet. There is no reload —
changing `server.cfg` needs a server restart.
