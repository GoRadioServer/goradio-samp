# Errors

```pawn
forward OnGoRadioError(stationid, const code[], const message[]);
```

Everything that goes wrong arrives here. `stationid` is
`INVALID_RADIO_STATION` for problems not tied to a particular station.

```pawn
public OnGoRadioError(stationid, const code[], const message[])
{
    printf("[radio] station %d: %s: %s", stationid, code, message);
    return 1;
}
```

Log it somewhere you'll see. A silent radio with no error handler is very
hard to diagnose; the plugin also logs to the server console, but only
your script can put it where your admins look.

## Codes

### Permanent — never retried

These will fail identically no matter how many times they're tried. The
plugin **stops** rather than spinning forever on what is really a config
mistake, so a station hitting one stays down until you fix it and restart.

| Code | Means | Fix |
|---|---|---|
| `unauthenticated` | The token is missing, malformed, or expired. | Regenerate with `radio tokengen` and update `goradio_token`. |
| `permission_denied` | The token doesn't cover this slug, or is read-only and this was a write. | Reissue with `--slugs` covering the slug, and `--write`. |
| `invalid_argument` | The request was malformed — most often an empty slug. | Check what you passed to `GoRadio_CreateStation`. |

!!! danger "A permanent error stops that station for good"
    No retry, no recovery. `GoRadio_IsStationRegistered` stays `0` and
    nothing plays. This is deliberate — the alternative is a server that
    looks busy forever while silently doing nothing — but it does mean a
    bad token is not something you sleep through. Log
    `OnGoRadioError` prominently.

### Transient — retried with backoff

| Code | Means |
|---|---|
| `unavailable` | The audio server couldn't be reached: down, restarting, network problem, timeout. |
| `not_found` | No such station slug, or no such queue item. |
| `internal` | The audio server failed unexpectedly, or answered something unparseable. |

`unavailable` is normal during an audio-server restart and needs no
action; the plugin retries from 1s up to a 30s cap and re-registers when it
comes back.

`not_found` most often means a command was issued **before the station
finished registering**. Queue from
[`OnGoRadioStationRegistered`](callbacks.md#ongoradiostationregistered)
rather than from `OnGameModeInit` and it goes away.

### Playback errors

The audio server also reports problems with individual items through this
callback, with its own codes rather than Connect ones — a source that
couldn't be fetched, or that failed to decode.

These arrive **asynchronously**, well after the `GoRadio_QueueTrack` that
caused them succeeded, because queueing only accepts an item; fetching and
decoding happen later. The `message` names the source.

A useful reflex: if a track never plays but nothing looks wrong, check for
one of these.

## What isn't an error

**`OnGoRadioCommandResult` with `success = false`** usually means "there
was nothing to do" — nothing was playing to skip, the queue id had already
gone, the station was already paused. Real failures fire `OnGoRadioError`
as well.

**A string getter returning `0`** means the station id was invalid, nothing
is playing, or the index was out of range. It is not an error condition and
nothing is logged.

**A native returning `0` instead of a request id** means the station id
was unknown. Check the id before assuming a call went out.

## Diagnosing

Turn on debug logging:

```
goradio_debug 1
```

Every request and response body goes to the server log, which usually
identifies the problem immediately:

```
[goradio] debug: -> QueueTrack {"slug":"mcnr-main",...}
[goradio] debug: <- QueueTrack HTTP 404 {"code":"not_found","message":"no such station"}
```

Your token is sent in a header, which is never logged, so debug output is
safe to share — read it first if your track metadata is sensitive.

Common symptoms and their causes are collected in
[Troubleshooting](../guides/troubleshooting.md).
