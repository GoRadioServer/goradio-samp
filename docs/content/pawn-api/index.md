# PAWN API — Overview

Everything the plugin exposes, in one place. The full declarations live in
`include/goradio.inc`, which carries the same documentation as comments.

```pawn
#include <a_samp>
#include <goradio>
```

## The shape of the API

**Commands are asynchronous.** Anything that talks to the audio server
returns a *request id* immediately and answers later in a callback.
Nothing blocks a server frame.

```pawn
new requestid = GoRadio_Skip(station);   // returns at once
// ... the answer arrives in OnGoRadioCommandResult with this requestid
```

A request id is `> 0`, or `0` if the station id was unknown. You can
ignore it — most code does — but it's how you match a result to the call
that caused it when several are in flight.

**Reads are free.** The `GoRadio_Get*` natives read a locally cached
snapshot, so they never block and can be called as often as you like. See
[Station State](state.md).

**String getters follow one convention:**

```pawn
new title[128];
if (GoRadio_GetCurrentTrackTitle(station, title))
    // title is filled in
```

They return `1` on success, or `0` having written an empty string —
because the station id was invalid, nothing was playing, or the index was
out of range. The length argument defaults to `sizeof dest`, so you rarely
pass it.

## The reference

<div class="grid cards" markdown>

-   :material-radio-tower: **[Stations](stations.md)**

    Creating, destroying, updating and finding stations.

-   :material-playlist-music: **[Playback & Queue](playback.md)**

    Queueing, skipping, pausing, seeking.

-   :material-information: **[Station State](state.md)**

    What's playing, what's queued, who's listening.

-   :material-bell: **[Callbacks](callbacks.md)**

    Everything the plugin tells you about.

-   :material-alert: **[Errors](errors.md)**

    Error codes, what's retried, and what to do.

</div>

## Connection natives

These three don't belong to a station.

### `GoRadio_SetServer`

```pawn
native GoRadio_SetServer(const url[], const token[]);
```

Points the plugin at an audio server, as an alternative to `goradio_url` /
`goradio_token` in `server.cfg`. Returns `1` if the settings were accepted
— not that the server is reachable, which you learn from
[`OnGoRadioServerInfo`](callbacks.md#ongoradioserverinfo).

Must be called **before any station is created**. Re-pointing the plugin
with stations registered is refused, since those registrations belong to
the old server.

Prefer `server.cfg`; see [Configuration](../getting-started/configuration.md#configuring-from-a-script-instead).

### `GoRadio_IsReady`

```pawn
native GoRadio_IsReady();
```

`1` once an audio server has been configured, from either source. Says
nothing about whether it is reachable — a station created before the audio
server is up is legitimate and will register when it appears.

### `GoRadio_ReloadConfig`

```pawn
native GoRadio_ReloadConfig();
```

Re-reads the `goradio_*` settings from `server.cfg` and applies them,
without restarting the server. Returns:

| Constant | Meaning |
|---|---|
| `GORADIO_RELOAD_APPLIED` | Everything in the file is now in effect. |
| `GORADIO_RELOAD_PARTIAL` | `goradio_debug` and `goradio_poll_interval` applied; the audio server URL, token, worker count and timeout were left alone because stations are registered. |
| `GORADIO_RELOAD_FAILED` | `server.cfg` was unreadable, had no `goradio_*` settings, or the new ones were rejected. Nothing connection-related changed; the log says why. |

A changed poll interval takes effect at the poller's next wake-up, so
allow up to one old interval for it to settle.

See [Configuration → Reloading without a restart](../getting-started/configuration.md#reloading-without-a-restart)
for which settings are reloadable and why, and a worked admin command.

### `GoRadio_GetServerVersion`

```pawn
native GoRadio_GetServerVersion(dest[], len = sizeof dest);
```

The audio server's build version, e.g. `"v0.9.0"` (or `"dev"` for a
locally built binary). Returns `0` with an empty string until
`OnGoRadioServerInfo` has fired.

## Constants

```pawn
#define INVALID_RADIO_STATION           (-1)

#define RADIO_QUEUE_APPEND              (0)
#define RADIO_QUEUE_PLAY_NEXT           (1)
#define RADIO_QUEUE_PLAY_NOW_INTERRUPT  (2)
```

## Complete native list

| Native | Page |
|---|---|
| `GoRadio_SetServer`, `GoRadio_IsReady`, `GoRadio_ReloadConfig`, `GoRadio_GetServerVersion` | this page |
| `GoRadio_CreateStation`, `GoRadio_DestroyStation`, `GoRadio_UpdateStation` | [Stations](stations.md) |
| `GoRadio_IsValidStation`, `GoRadio_IsStationRegistered` | [Stations](stations.md) |
| `GoRadio_GetStationBySlug`, `GoRadio_GetStationCount`, `GoRadio_GetStationAtIndex` | [Stations](stations.md) |
| `GoRadio_GetStationSlug`, `GoRadio_GetStreamURL` | [Stations](stations.md) |
| `GoRadio_SetStationMetadata`, `GoRadio_ClearStationMetadata` | [Stations](stations.md) |
| `GoRadio_QueueTrack`, `GoRadio_Dequeue`, `GoRadio_ClearQueue` | [Playback](playback.md) |
| `GoRadio_Skip`, `GoRadio_SkipTo` | [Playback](playback.md) |
| `GoRadio_Pause`, `GoRadio_Resume`, `GoRadio_Seek`, `GoRadio_SeekBy` | [Playback](playback.md) |
| `GoRadio_RefreshStatus`, `GoRadio_HasStatus` | [State](state.md) |
| `GoRadio_GetListenerCount`, `GoRadio_GetUptime`, `GoRadio_GetQueueLength` | [State](state.md) |
| `GoRadio_IsSilence`, `GoRadio_IsPaused`, `GoRadio_IsPlaying` | [State](state.md) |
| `GoRadio_GetCurrentTrack*` (5 strings + duration + elapsed) | [State](state.md) |
| `GoRadio_GetQueueItem*` (count + 4 strings + duration) | [State](state.md) |
| `GoRadio_GetHistory*` (count + 3 strings) | [State](state.md) |
| `GoRadio_RequestStationList`, `GoRadio_GetListedStation*` | [State](state.md#every-station-on-the-audio-server) |
