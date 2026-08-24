# goradio-samp

A SA-MP server plugin that creates and drives one or more
[GoRadio](../gta-radio-golang) stations from PAWN.

**📖 [Full documentation](https://tmfksoft.github.io/goradio-samp/)** —
installation, how it works, the complete PAWN API, guides and
troubleshooting.

Your gamemode registers stations, queues tracks, skips, pauses and seeks,
and gets told when tracks start and end — the same capabilities the Lua
station controller has, exposed as natives and callbacks.

```pawn
new station = GoRadio_CreateStation("mcnr-main", "MCNR Main", "The main channel", 3);

public OnGoRadioStationRegistered(stationid, const streamUrl[], bool:reRegistered,
	bool:afterReconnect)
{
	if (GoRadio_GetQueueLength(stationid) == 0)
		GoRadio_QueueTrack(stationid, "music/night-drive.mp3", "Night Drive", "MCNR Radio");
	return 1;
}
```

## Why HTTP and not gRPC

SA-MP is a 32-bit process with a C plugin ABI, and getting a gRPC stack
into one is somewhere between painful and impossible. The audio server
serves the Connect protocol — plain HTTP/1.1 with JSON bodies — on the
same port as gRPC, and that is what this plugin speaks. No gRPC library,
no protobuf library, no code generation, no HTTP/2, and no third-party
dependencies at all: the HTTP client, the JSON parser and the Connect
envelope framing are in `src/`, about 2,000 lines in total.

Streaming works too. `SubscribeEvents` arrives as a chunked HTTP/1.1
response carrying Connect envelopes, so the plugin gets pushed events
(track started, queue low, listener count) rather than polling for them.

## Installing

Grab the archive for your platform from
[Releases](https://github.com/tmfksoft/goradio-samp/releases) — it mirrors
a SA-MP server directory, so you can copy the folders straight over your
server root.

| Download | For |
|---|---|
| `goradio-<version>-linux-x86.tar.gz` | Linux `samp03svr` (32-bit) |
| `goradio-<version>-windows-x86.zip` | Windows `samp-server.exe` (32-bit) |
| `goradio-<version>-linux-x86_64.tar.gz` | Linux open.mp (64-bit) |
| `goradio-<version>-windows-x86_64.zip` | Windows open.mp (64-bit) |

SA-MP is a 32-bit process and will not load an x86_64 build — if the log
says `Failed.` next to the plugin, that is almost always why.

Then:

1. Drop `goradio.so` (or `goradio.dll`) into your server's `plugins/`
   directory.
2. Copy `include/goradio.inc` into your Pawno `include/` directory.
3. Add the plugin and its settings to `server.cfg`:

```
plugins goradio.so

goradio_url            http://127.0.0.1:9090
goradio_token          eyJhbGciOiJIUzI1NiIs...
goradio_poll_interval  5
goradio_workers        2
goradio_timeout        10000
goradio_debug          0
```

| Setting | Default | Meaning |
|---|---|---|
| `goradio_url` | `http://127.0.0.1:9090` | The audio server's `grpc.listen_addr`. `https://` needs a TLS build (see below). |
| `goradio_token` | *(none)* | JWT from `radio tokengen`. Needs write scope for every slug you register. |
| `goradio_poll_interval` | `5` | Seconds between status refreshes. `0` disables polling; events still arrive. |
| `goradio_workers` | `2` | Threads for outgoing calls. Each holds one keep-alive connection. |
| `goradio_timeout` | `10000` | Per-request timeout, milliseconds. |
| `goradio_debug` | `0` | `1` logs every request and response body. |

Prefer `server.cfg` over `GoRadio_SetServer` — a token hardcoded in a
gamemode is a token in your source control. `GoRadio_SetServer` exists
for cases where the settings come from somewhere else, and must be
called before any station is created.

## How the API is shaped

**Everything that talks to the audio server is asynchronous.** A command
native queues the request and returns a request id immediately; the
answer comes back in a callback. Nothing blocks a server frame on the
network — the HTTP work happens on worker threads, and callbacks are
delivered on the main thread from `ProcessTick`.

**The `GoRadio_Get*` natives are free.** They read a locally cached
snapshot that the plugin keeps current from the event stream and a
periodic `GetStatus`, so calling `GoRadio_GetListenerCount` in a timer,
or `GoRadio_GetCurrentTrackElapsed` for a progress bar, costs nothing.

**Registration is in the background.** `GoRadio_CreateStation` returns an
id straight away and keeps retrying until the audio server accepts it, so
a station created while the audio server is down comes up on its own.
Wait for `OnGoRadioStationRegistered` before queueing anything — an RPC
against a slug the audio server hasn't seen fails with `not_found`.

### The one thing worth reading twice

`OnGoRadioStationRegistered` fires on the **first** registration and again
after **every reconnect**, and re-priming the queue there is not optional:

```pawn
public OnGoRadioStationRegistered(stationid, const streamUrl[], bool:reRegistered,
	bool:afterReconnect)
{
	if (GoRadio_GetQueueLength(stationid) == 0)
		QueueSomeTracks(stationid);
	return 1;
}
```

Station registration lives only in the audio server's memory. If it
restarts, the station this plugin re-registers for you comes back with an
**empty queue** — `reRegistered` is `false` to tell you it had never heard
of the slug. Re-registering does not refill it, and `OnGoRadioQueueLow`
will not save you: that fires on the transition *into* "low" from "not
low", which a queue that has been empty since it was recreated never
makes. Without this, your station goes quiet after every audio-server
restart while still looking perfectly connected.

## Natives

### Connection

| Native | Returns |
|---|---|
| `GoRadio_SetServer(const url[], const token[])` | 1 if accepted. Before any station exists. |
| `GoRadio_IsReady()` | 1 once a server is configured. |
| `GoRadio_GetServerVersion(dest[], len)` | The audio server's build version. |

### Stations

| Native | Returns |
|---|---|
| `GoRadio_CreateStation(const slug[], const name[], const description[], lowQueueThreshold, const logoUrl[])` | Station id, or `INVALID_RADIO_STATION`. |
| `GoRadio_DestroyStation(stationid)` | Unregisters and frees the id. |
| `GoRadio_IsValidStation(stationid)` | |
| `GoRadio_GetStationBySlug(const slug[])` | Station id, or `INVALID_RADIO_STATION`. |
| `GoRadio_GetStationCount()` / `GoRadio_GetStationAtIndex(index)` | For iterating. |
| `GoRadio_SetStationMetadata(stationid, const key[], const value[])` | Freeform data; sent at next registration. |
| `GoRadio_ClearStationMetadata(stationid)` | |
| `GoRadio_UpdateStation(stationid, const name[], ...)` | Re-registers in place, without disturbing playback. |
| `GoRadio_GetStationSlug(stationid, dest[], len)` | |
| `GoRadio_GetStreamURL(stationid, dest[], len)` | The listening URL, once registered. |
| `GoRadio_IsStationRegistered(stationid)` | |

### Playback

All return a request id (> 0), or 0 for an unknown station.

| Native | Notes |
|---|---|
| `GoRadio_QueueTrack(stationid, const location[], const title[], const artist[], const coverArt[], mode)` | `location` is a path under the audio root or an `http(s)://` URL — inferred, not declared. Answers via `OnGoRadioTrackQueued`. |
| `GoRadio_Dequeue(stationid, const queueId[])` | One pending item. Not the playing one. |
| `GoRadio_ClearQueue(stationid, bool:stopCurrent)` | `stopCurrent` also cuts the current track. |
| `GoRadio_Skip(stationid)` | The only way to end a live stream. |
| `GoRadio_SkipTo(stationid, const queueId[])` | Drops everything ahead of it. |
| `GoRadio_Pause(stationid)` / `GoRadio_Resume(stationid)` | Not applicable to a live stream. |
| `GoRadio_Seek(stationid, positionSeconds)` / `GoRadio_SeekBy(stationid, deltaSeconds)` | Clamped to the track. |

`mode` is `RADIO_QUEUE_APPEND` (default), `RADIO_QUEUE_PLAY_NEXT`, or
`RADIO_QUEUE_PLAY_NOW_INTERRUPT`.

### Cached state

| Native | Notes |
|---|---|
| `GoRadio_RefreshStatus(stationid)` | Forces a refresh; rarely needed. |
| `GoRadio_HasStatus(stationid)` | 1 once a snapshot has arrived. |
| `GoRadio_GetListenerCount(stationid)` | |
| `GoRadio_GetUptime(stationid)` | |
| `GoRadio_IsSilence` / `IsPaused` / `IsPlaying(stationid)` | |
| `GoRadio_GetQueueLength(stationid)` | Kept current by events between polls. |
| `GoRadio_GetCurrentTrackID` / `Title` / `Artist` / `Location` / `CoverArt(stationid, dest[], len)` | 0 when nothing is playing. |
| `GoRadio_GetCurrentTrackDuration(stationid)` | 0 = unknown or live. |
| `GoRadio_GetCurrentTrackElapsed(stationid)` | Advances between polls. |
| `GoRadio_GetQueueItemCount(stationid)` and `GetQueueItem{ID,Title,Artist,Location,Duration}` | From the last snapshot. |
| `GoRadio_GetHistoryCount(stationid)` and `GetHistory{Title,Artist,Reason}` | Recently finished, oldest first. |

### Every station on the audio server

`GoRadio_RequestStationList()` then, once `OnGoRadioStationList` fires,
`GoRadio_GetListedStationCount()`, `GoRadio_GetListedStationSlug/Name/Logo(index, dest[], len)`
and `GoRadio_GetListedStationListeners(index)`. This covers every station
the token authorizes — including ones other servers registered — not just
this plugin's own.

## Callbacks

Offered to the gamemode and every filterscript; return values ignored.

| Callback | When |
|---|---|
| `OnGoRadioServerInfo(const version[])` | The audio server answered for the first time. |
| `OnGoRadioStationRegistered(stationid, const streamUrl[], bool:reRegistered, bool:afterReconnect)` | Registered, and ready for commands. See above. |
| `OnGoRadioTrackStarted(stationid, const queueId[], const location[], const title[], const artist[], const coverArt[], durationSeconds)` | A queued item started. |
| `OnGoRadioTrackEnded(stationid, const queueId[], const reason[])` | `"completed"` or `"interrupted"`. |
| `OnGoRadioTrackQueued(stationid, requestid, const queueId[], queuePosition)` | The answer to `GoRadio_QueueTrack`. |
| `OnGoRadioQueueLow(stationid, queueLength, threshold)` | Edge-triggered; needs `lowQueueThreshold > 0`. |
| `OnGoRadioQueueUpdated(stationid, queueLength)` | |
| `OnGoRadioListenerCountChanged(stationid, listenerCount)` | |
| `OnGoRadioSilenceStarted` / `OnGoRadioSilenceEnded(stationid)` | |
| `OnGoRadioStatusUpdate(stationid)` | A fresh snapshot landed. |
| `OnGoRadioCommandResult(stationid, requestid, const command[], bool:success, result)` | The answer to any other command. |
| `OnGoRadioStationList(count)` | |
| `OnGoRadioError(stationid, const code[], const message[])` | See below. |

`OnGoRadioCommandResult` with `success = false` is usually not a failure:
several RPCs report "there was nothing to do" that way — nothing was
playing to skip, the queue id had already gone. A real failure also fires
`OnGoRadioError`.

`OnGoRadioError` codes come from Connect: `unauthenticated` (bad or
expired token), `permission_denied` (the token doesn't cover this slug),
`not_found`, `invalid_argument`, `unavailable` (couldn't reach the audio
server). The first three are **never retried** — they will fail
identically forever, so the station stays down until the config is
fixed, and the reason is in the server log.

## Building

Linux, with `make`:

```sh
make                # bin/goradio.so, 32-bit -- what samp03svr loads
make BITS=64        # 64-bit, for open.mp or local testing
make TLS=1          # link OpenSSL, so https:// URLs work
make test           # host-side tests, no SA-MP server needed
```

A 32-bit build needs the multilib toolchain (`apt install g++-multilib`,
or `dnf install glibc-devel.i686 libstdc++-devel.i686`).

Or build it in a container that already has that toolchain, so the host
needs nothing but Docker:

```sh
make docker                                   # bin/goradio.so, 32-bit
docker build --target test --build-arg BITS=32 .   # run the suite at 32 bits
```

Windows, or if you'd rather use CMake:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

There is no SDK to fetch: `sdk/` carries a hand-written subset of the
SA-MP plugin ABI, which is all this plugin needs.

### TLS

Plain HTTP is the default, on the assumption the audio server is reachable
over a trusted network. If it sits behind a TLS-terminating proxy, build
with `TLS=1` / `-DGORADIO_TLS=ON` (OpenSSL required) and use an `https://`
URL — certificates and hostnames are verified. A non-TLS build given an
`https://` URL refuses it with an explanation rather than falling back to
plaintext.

## Tests

`make test` builds everything below the AMX layer and runs it against
`test/fake_audioserver.py`, a stand-in for `radio serve` that answers the
way protobuf-JSON actually does — int64 fields as quoted strings,
lowerCamelCase names, default-valued fields omitted entirely, and a
chunked `SubscribeEvents` body carrying Connect envelopes (including two
events glued into one TCP write). It covers URL parsing, the JSON layer,
registration, the event stream, the status cache, commands, and the
reconnect-and-re-register path.

The suite has also been run clean under ThreadSanitizer and
AddressSanitizer/UBSan, which is worth repeating after changes to the
threading in `src/manager.cpp`.

## How it works

```
PAWN script
    |  natives (main thread, never block)
    v
Manager ---- job queue ----> worker threads --- HTTP/1.1 keep-alive ---> audio server
    ^                                                                        |
    |                                                                        |
    +--- callback queue <--- stream thread <--- chunked Connect envelopes ----+
             |
             v
      ProcessTick -> OnGoRadio* publics (main thread)
```

- One **stream thread per station** holds the `SubscribeEvents`
  subscription and owns that station's registration lifecycle, including
  reconnect with exponential backoff.
- A small pool of **worker threads** runs unary RPCs, each on its own
  keep-alive connection.
- Results never touch PAWN directly. They are turned into plain-value
  events, queued, and delivered from `ProcessTick` on the main thread,
  because a SA-MP public may only be called from the server's own thread.
- `Unload` interrupts the sockets rather than waiting out read timeouts,
  so shutdown is prompt even with a station parked on an idle stream.

## Releasing

Tagging `vX.Y.Z` builds and publishes all four binaries as GitHub release
archives, with checksums:

```sh
git tag -a v1.0.0 -m "v1.0.0"
git push origin v1.0.0
```

The version is baked into the binary and reported in the server log. Every
release build runs the test suite at its own architecture and verifies the
binary really is the width it claims before packaging.

## Documentation

The docs in `docs/` are MkDocs + Material, published to GitHub Pages on
every push to `main`.

```sh
pip install -r docs/requirements.txt
cd docs && mkdocs serve      # http://127.0.0.1:8000
mkdocs build --strict        # what CI runs -- fails on a broken link
```

## Related

- [gta-radio-golang](../gta-radio-golang) — the audio server and its Lua
  station controller, whose `radio.*` API this plugin mirrors.
- `docs/content/developer-api/http-json-api.md` in that repo — the
  transport this plugin implements.
- `docs/content/developer-api/protocol-reference.md` — the per-RPC spec.
