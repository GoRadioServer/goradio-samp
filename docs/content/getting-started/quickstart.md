# Quickstart

A station that plays a rotating playlist and announces tracks in chat, in
about twenty lines. This assumes you've done the
[installation](installation.md) and the log says
`connected to audio server`.

## The whole thing

```pawn
#include <a_samp>
#include <goradio>

static gStation = INVALID_RADIO_STATION;

static const gPlaylist[][] = {
    "music/night-drive.mp3",
    "music/san-fierro-nights.mp3",
    "music/back-to-the-grove.mp3"
};
static gCursor = 0;

public OnGameModeInit()
{
    // The 3 is a low-queue threshold: tell me when fewer than 3 items are
    // waiting, so I can top it up.
    gStation = GoRadio_CreateStation("mcnr-main", "MCNR Main", "The main channel", 3);
    return 1;
}

QueueNext()
{
    new i = gCursor++ % sizeof(gPlaylist);
    GoRadio_QueueTrack(gStation, gPlaylist[i], gPlaylist[i], "MCNR Radio");
}

public OnGoRadioStationRegistered(stationid, const streamUrl[], bool:reRegistered,
    bool:afterReconnect)
{
    printf("Radio is live at %s", streamUrl);

    // Also runs after a reconnect -- see below, this matters.
    if (GoRadio_GetQueueLength(stationid) == 0)
        for (new i = 0; i < 3; i++) QueueNext();
    return 1;
}

public OnGoRadioQueueLow(stationid, queueLength, threshold)
{
    for (new i = queueLength; i <= threshold; i++) QueueNext();
    return 1;
}

public OnGoRadioTrackStarted(stationid, const queueId[], const location[], const title[],
    const artist[], const coverArt[], durationSeconds)
{
    new msg[144];
    format(msg, sizeof(msg), "{88CCFF}[Radio]{FFFFFF} %s - %s", artist, title);
    SendClientMessageToAll(-1, msg);
    return 1;
}
```

That's a complete, self-sustaining station. The rest of this page explains
what each part is doing and why.

## Step by step

### 1. Create the station

```pawn
gStation = GoRadio_CreateStation("mcnr-main", "MCNR Main", "The main channel", 3);
```

Returns a station id immediately — but the station is **not registered
yet**. Registration happens on a background thread and retries with
backoff until the audio server accepts it, so this works fine even if the
audio server isn't up yet.

The slug (`mcnr-main`) is the station's identity on the audio server and
the last part of its stream URL. The `3` enables
[`OnGoRadioQueueLow`](../pawn-api/callbacks.md#ongoradioqueuelow).

### 2. Wait for registration before queueing

```pawn
public OnGoRadioStationRegistered(stationid, const streamUrl[], bool:reRegistered,
    bool:afterReconnect)
```

Queueing against a slug the audio server hasn't accepted yet fails with
`not_found`, so this callback — not `OnGameModeInit` — is where a
station's first track belongs.

!!! danger "This callback fires again after every reconnect, and that is the point"
    Station registration lives only in the audio server's memory. If it
    restarts, the station re-registered for you comes back with an **empty
    queue**.

    Re-registering does not refill it, and `OnGoRadioQueueLow` will not
    save you: it is edge-triggered, firing on the transition *into* "low"
    from "not low", which a queue that has been empty since the moment it
    was recreated never makes.

    So the check in the example is load-bearing:

    ```pawn
    if (GoRadio_GetQueueLength(stationid) == 0)
        // queue something
    ```

    Without it, your station goes permanently quiet after every
    audio-server restart, while still reporting itself as connected and
    registered. This is the single easiest way to get this wrong; it has
    its own page in [Station Lifecycle](../how-it-works/station-lifecycle.md).

### 3. Queue tracks

```pawn
GoRadio_QueueTrack(gStation, "music/night-drive.mp3", "Night Drive", "MCNR Radio");
```

The location is either a path relative to the audio server's configured
audio root, or an `http(s)://` URL — which is worked out from the string,
not declared. A URL can be a file to fetch or a live stream to relay; the
audio server figures out which.

The call returns a request id, not a result. The queue id you need for
[`GoRadio_SkipTo`](../pawn-api/playback.md#goradio_skipto) or
[`GoRadio_Dequeue`](../pawn-api/playback.md#goradio_dequeue) arrives in
[`OnGoRadioTrackQueued`](../pawn-api/callbacks.md#ongoradiotrackqueued).

Queue *ahead* of when you need it. The audio server prefetches from the
moment you queue, so an item queued three tracks early is already decoded
and ready when its turn comes.

### 4. Keep the queue fed

```pawn
public OnGoRadioQueueLow(stationid, queueLength, threshold)
{
    for (new i = queueLength; i <= threshold; i++) QueueNext();
    return 1;
}
```

Because the event is edge-triggered, top the queue back up past the
threshold rather than adding a single item — otherwise you sit at exactly
the threshold and never get told again.

### 5. React to playback

`OnGoRadioTrackStarted` fires when an item actually begins playing, which
is the only reliable "now playing" signal. `GoRadio_QueueTrack` succeeding
means the item was *accepted*, not that it will play — a source that fails
to fetch or decode surfaces later through
[`OnGoRadioError`](../pawn-api/errors.md).

## Letting players listen

```pawn
new url[160];
if (GoRadio_GetStreamURL(gStation, url))
    PlayAudioStreamForPlayer(playerid, url);
```

The stream URL is empty until the station has registered. See
[Playing the Stream In-Game](../guides/in-game-playback.md) for the
details worth knowing — stopping the stream, players who join mid-track,
and volume.

## Next

- [Keeping a Playlist Running](../guides/playlist-rotation.md) — shuffle
  bags, jingles, requests.
- [PAWN API Overview](../pawn-api/index.md) — the whole surface.
- [How It Works](../how-it-works/index.md) — what's happening underneath.
