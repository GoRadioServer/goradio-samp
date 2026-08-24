# Troubleshooting

Start with the server log. The plugin logs everything it does under a
`[goradio]` prefix, and a healthy start looks like this:

```
Loading plugin: goradio.so
[goradio] goradio 1.0.0 loaded (http only)
[goradio] using audio server http://127.0.0.1:9090 (2 workers, status poll every 5s)
[goradio] connected to audio server v0.9.0
 Loaded.
[goradio] station 0 (mcnr-main) created, registering...
[goradio] station 0 (mcnr-main) registered, stream at http://127.0.0.1:9090/stream/mcnr-main
[goradio] station 0 (mcnr-main) subscribed to events
```

If a line is missing, the problem is at that step.

## The plugin won't load

```
Loading plugin: goradio.so
        Failed.
```

**Almost always an architecture mismatch.** `samp03svr` is 32-bit and
cannot load a 64-bit `.so`. Check:

```sh
file plugins/goradio.so
# want: ELF 32-bit LSB shared object, Intel 80386
```

If it says `ELF 64-bit`, rebuild with `make` (the default is 32-bit) rather
than `make BITS=64`. open.mp is the other way round — it wants the 64-bit
build.

The other cause is a missing runtime dependency. The release binaries link
the C++ runtime statically to avoid this, but if you built it yourself:

```sh
ldd plugins/goradio.so    # anything "not found" is your answer
```

## No `connected to audio server` line

The plugin loaded but never reached the server. Look for the warning that
follows.

**`no goradio_* settings in server.cfg`** — the plugin found no
configuration. Check the keys are spelled right and separated by a space,
not `=`.

**`goradio_token is not set`** — add `goradio_token`, or call
`GoRadio_SetServer` before creating stations.

**`connect ... failed: connection refused`** — the audio server isn't
listening there. Confirm with:

```sh
curl -X POST http://127.0.0.1:9090/audioserver.v1.AudioServerService/GetServerInfo \
  -H "Content-Type: application/json" -H "Authorization: Bearer $TOKEN" -d '{}'
```

A version back means the address and token are both good, and the problem
is in the plugin's configuration rather than the server.

**`unauthenticated: ...`** — the token is malformed or expired. Regenerate
it. Watch for a truncated copy-paste; JWTs are long and `server.cfg` values
must be on one line.

**`this build has no TLS support`** — you gave it an `https://` URL and the
binary was built without OpenSSL. Either use `http://` or build with
[TLS](../building/index.md#tls).

## The station never registers

You see `created, registering...` but never `registered`.

**`register failed (unavailable: ...), retrying in 1000ms`** — the audio
server is unreachable. The plugin keeps retrying and will register itself
when the server appears. Nothing to do.

**`cannot register: permission_denied`** — the token doesn't cover this
slug, or is read-only. Reissue it:

```sh
radio tokengen --slugs mcnr-main,mcnr-chill --write
```

This is a **permanent** error: the station stops and will not retry.
Restart the server after fixing the token.

**`cannot register: invalid_argument`** — usually an empty slug. Check what
you passed to `GoRadio_CreateStation`.

## Queueing does nothing

**`not_found: no such station`** — the commonest mistake by far: you queued
before the station finished registering. Move it into
`OnGoRadioStationRegistered`.

```pawn
// Wrong -- the station isn't registered yet
public OnGameModeInit()
{
    new s = GoRadio_CreateStation("mcnr-main");
    GoRadio_QueueTrack(s, "music/intro.mp3");   // not_found
    return 1;
}

// Right
public OnGoRadioStationRegistered(stationid, const streamUrl[], bool:reRegistered,
    bool:afterReconnect)
{
    if (GoRadio_GetQueueLength(stationid) == 0)
        GoRadio_QueueTrack(stationid, "music/intro.mp3");
    return 1;
}
```

**The native returned 0** — the station id was unknown. Check it isn't
`INVALID_RADIO_STATION`.

**`OnGoRadioTrackQueued` fires but nothing plays** — the item was accepted
but failed to fetch or decode. Watch `OnGoRadioError`, which reports it
asynchronously and names the source. Check the path is right relative to
the audio server's audio root, and that the file is a format it can decode.

## The station goes silent after a while

**The most likely cause: the audio server restarted and your queue wasn't
re-primed.** The station reports itself registered, nothing errors, and
nothing plays.

The fix is the queue-length check in `OnGoRadioStationRegistered`:

```pawn
if (GoRadio_GetQueueLength(stationid) == 0)
    QueueSomeTracks(stationid);
```

Full explanation in
[Station Lifecycle](../how-it-works/station-lifecycle.md#why-re-priming-is-your-job).

**The other cause: an edge-triggered `OnGoRadioQueueLow` that stopped
firing.** If your handler adds exactly one item, the queue sits at the
threshold and never dips again, so the callback never comes back. Top up
*past* the threshold:

```pawn
for (new i = queueLength; i <= threshold; i++)
    QueueNextTrack(stationid);
```

`OnGoRadioSilenceStarted` firing unexpectedly is the early warning for
both.

## Repeated reconnects

```
[goradio] WARN: station 0 (mcnr-main) event stream ended (...), reconnecting
```

Occasionally is normal — a restart, a network blip. Constantly means
something is closing the connection.

If the audio server is behind a reverse proxy, that's the usual culprit:
the event stream is a long-lived chunked HTTP/1.1 response, and a proxy
with a short idle or response timeout will cut it. Raise the read timeout
for that route, and make sure response buffering is off — a proxy that
buffers will hold events until the buffer fills, which looks like the
plugin having gone deaf.

## Nothing is wrong but events are late

Check `goradio_poll_interval`. Events themselves are pushed and arrive
within milliseconds, but anything only a snapshot provides — the queue and
history item lists — refreshes on the poll timer and at track changes.

If a proxy sits in front of the audio server, response buffering will also
delay events; see above.

## Getting more detail

```
goradio_debug 1
```

Logs every request and response body:

```
[goradio] debug: -> QueueTrack {"slug":"mcnr-main",...}
[goradio] debug: <- QueueTrack HTTP 404 {"code":"not_found","message":"no such station"}
```

Tokens travel in a header and are never logged, so this is safe to paste
into an issue.

## Reporting a bug

Include:

- the plugin version line from the log,
- `GoRadio_GetServerVersion` or the audio server's own version,
- the `[goradio]` log lines around the problem, with `goradio_debug 1`,
- your OS and whether it's `samp03svr` or open.mp,
- the smallest PAWN snippet that reproduces it.
