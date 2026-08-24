# Stations

## `GoRadio_CreateStation`

```pawn
native GoRadio_CreateStation(const slug[], const name[] = "", const description[] = "",
    lowQueueThreshold = 0, const logoUrl[] = "");
```

Creates a station and registers it with the audio server.

| Argument | Meaning |
|---|---|
| `slug` | The station's identity on the audio server, and the last part of its stream URL. Required. |
| `name` | Display name. Defaults to the slug. |
| `description` | Free text, shown in dashboards. |
| `lowQueueThreshold` | `> 0` enables [`OnGoRadioQueueLow`](callbacks.md#ongoradioqueuelow) when the pending queue drops to or below this many items. `0` disables it. |
| `logoUrl` | Station artwork, carried through to the audio server's status and listings. |

Returns a station id, or `INVALID_RADIO_STATION` if the slug is empty,
already in use on this server, or no audio server is configured. The
reason is logged.

**The id is valid immediately; the station is not.** Registration happens
in the background and retries until it succeeds, so creating a station
while the audio server is down is fine — it comes up on its own. Wait for
[`OnGoRadioStationRegistered`](callbacks.md#ongoradiostationregistered)
before queueing anything.

```pawn
new station = GoRadio_CreateStation("myfm", "My FM", "The main channel", 3,
    "https://cdn.example.com/art/main.png");

if (station == INVALID_RADIO_STATION)
    print("could not create the station -- check the log");
```

!!! tip "Set metadata before creating"
    Metadata is sent with the registration, so
    [`GoRadio_SetStationMetadata`](#goradio_setstationmetadata) before
    `GoRadio_CreateStation` saves a second round trip. Setting it
    afterwards works too — follow it with
    [`GoRadio_UpdateStation`](#goradio_updatestation) to push it.

### Re-attaching to a running station

If the slug is already registered on the audio server — because your
gamemode restarted, or another server registered it — this **re-attaches**
rather than replacing it. `OnGoRadioStationRegistered` reports
`reRegistered = true`, and the existing queue keeps playing untouched.

That's usually what you want. For a clean start:

```pawn
public OnGoRadioStationRegistered(stationid, const streamUrl[], bool:reRegistered,
    bool:afterReconnect)
{
    if (reRegistered && !afterReconnect)
        GoRadio_ClearQueue(stationid, true);   // stop whatever the last run left playing
    // ... then queue your own
    return 1;
}
```

## `GoRadio_DestroyStation`

```pawn
native GoRadio_DestroyStation(stationid);
```

Unregisters the station and frees the id. The audio server stops its
player, drops the queue, and disconnects anyone listening. Nothing is
persisted — re-creating the same slug later starts empty.

Returns `1` if the station existed.

Whether to call this on shutdown is a judgement call — see
[Station Lifecycle](../how-it-works/station-lifecycle.md#destroying-a-station).
The plugin's own unload does **not** unregister stations, so leaving them
alone keeps music playing across a server restart.

## `GoRadio_UpdateStation`

```pawn
native GoRadio_UpdateStation(stationid, const name[], const description[] = "",
    lowQueueThreshold = 0, const logoUrl[] = "");
```

Re-registers the station with new details. The audio server updates it in
place: playback continues, the queue is untouched, listeners stay
connected.

Every argument is **replaced, not merged**, and most have defaults — so
passing only a name clears the description, threshold and logo. Pass
everything you want to keep.

Metadata is the exception: the plugin remembers it and re-sends it on
every registration.

```pawn
GoRadio_SetStationMetadata(station, "event", "christmas");
GoRadio_UpdateStation(station, "My FM — Festive", "Seasonal music", 3,
    "https://cdn.example.com/art/festive.png");
```

Returns `1` if the station existed. The re-registration itself is
asynchronous and fires `OnGoRadioStationRegistered` when it lands.

## `GoRadio_IsValidStation`

```pawn
native GoRadio_IsValidStation(stationid);
```

`1` if the id refers to a station this server currently has. Says nothing
about whether it has registered yet — for that, use
[`GoRadio_IsStationRegistered`](#goradio_isstationregistered).

## `GoRadio_IsStationRegistered`

```pawn
native GoRadio_IsStationRegistered(stationid);
```

`1` once the audio server has confirmed the registration. Goes back to `0`
while a dropped connection is being re-established, and returns to `1` when
it comes back.

Useful as a guard in a timer, or to show connection state in an admin
command. You don't need it before every queue call — a command issued
while disconnected simply fails and fires
[`OnGoRadioError`](errors.md).

## `GoRadio_GetStationBySlug`

```pawn
native GoRadio_GetStationBySlug(const slug[]);
```

The station id for a slug, or `INVALID_RADIO_STATION`. Searches only this
server's own stations — for everything on the audio server, see
[`GoRadio_RequestStationList`](state.md#every-station-on-the-audio-server).

Handy for command handlers that take a station name:

```pawn
new station = GoRadio_GetStationBySlug(params);
if (station == INVALID_RADIO_STATION)
    return SendClientMessage(playerid, -1, "No such station.");
```

## `GoRadio_GetStationCount` / `GoRadio_GetStationAtIndex`

```pawn
native GoRadio_GetStationCount();
native GoRadio_GetStationAtIndex(index);
```

For iterating every station this server owns. Ids are **not contiguous**
once a station has been destroyed, so walk by index rather than counting
up from zero:

```pawn
for (new i = 0, n = GoRadio_GetStationCount(); i < n; i++)
{
    new station = GoRadio_GetStationAtIndex(i), slug[64];
    GoRadio_GetStationSlug(station, slug);
    printf("%s: %d listening", slug, GoRadio_GetListenerCount(station));
}
```

`GoRadio_GetStationAtIndex` returns `INVALID_RADIO_STATION` for an
out-of-range index.

## `GoRadio_GetStationSlug`

```pawn
native GoRadio_GetStationSlug(stationid, dest[], len = sizeof dest);
```

The slug the station was created with. Returns `0` for an unknown station.

## `GoRadio_GetStreamURL`

```pawn
native GoRadio_GetStreamURL(stationid, dest[], len = sizeof dest);
```

The listening URL the audio server assigned — what you hand to
`PlayAudioStreamForPlayer`, or show players so they can listen in a
browser.

Empty, returning `0`, until the station has registered. See
[Playing the Stream In-Game](../guides/in-game-playback.md).

## `GoRadio_SetStationMetadata`

```pawn
native GoRadio_SetStationMetadata(stationid, const key[], const value[]);
```

Attaches freeform key/value data to the station — a group name for
clustering stations in a dashboard, a genre tag, an operator id. The audio
server stores and returns these without interpreting them.

Passing an empty value removes the key.

```pawn
GoRadio_SetStationMetadata(station, "group", "myserver");
GoRadio_SetStationMetadata(station, "genre", "mixed");
GoRadio_SetStationMetadata(station, "server", "eu-1");
```

Metadata is sent with the **next registration**, so set it before
`GoRadio_CreateStation`, or follow a later change with
`GoRadio_UpdateStation`. It is remembered by the plugin and re-sent
automatically after a reconnect.

Returns `1` if the station existed.

## `GoRadio_ClearStationMetadata`

```pawn
native GoRadio_ClearStationMetadata(stationid);
```

Removes every key. Like setting metadata, it takes effect at the next
registration.
