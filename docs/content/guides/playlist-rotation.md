# Keeping a Playlist Running

A station is only as good as whatever keeps feeding it. This is the
pattern, and then the refinements worth adding.

## The minimum that works

Two callbacks and a cursor:

```pawn
static const gPlaylist[][] = {
    "music/night-drive.mp3",
    "music/san-fierro-nights.mp3",
    "music/back-to-the-grove.mp3"
};
static const gTitles[][] = {
    "Night Drive",
    "San Fierro Nights",
    "Back to the Grove"
};
static gCursor = 0;

QueueNext(stationid)
{
    new i = gCursor++ % sizeof(gPlaylist);
    GoRadio_QueueTrack(stationid, gPlaylist[i], gTitles[i], "My FM");
}

public OnGoRadioStationRegistered(stationid, const streamUrl[], bool:reRegistered,
    bool:afterReconnect)
{
    if (GoRadio_GetQueueLength(stationid) == 0)
        for (new i = 0; i < 3; i++) QueueNext(stationid);
    return 1;
}

public OnGoRadioQueueLow(stationid, queueLength, threshold)
{
    for (new i = queueLength; i <= threshold; i++) QueueNext(stationid);
    return 1;
}
```

Create the station with a threshold so the second callback fires:

```pawn
GoRadio_CreateStation("myfm", "My FM", "", 3);
```

Two things about this that are easy to get wrong:

**Top up past the threshold.** `OnGoRadioQueueLow` is edge-triggered — it
won't fire again until the queue rises above the threshold and dips back.
Adding one item leaves you sitting at the threshold, and the callback never
comes again.

**The registration path is not optional.** It's what restarts the rotation
after an audio-server restart empties the queue. See
[Station Lifecycle](../how-it-works/station-lifecycle.md#why-re-priming-is-your-job).

## Shuffling without repeats

A plain `random()` will play the same track twice in a row often enough to
be noticed. A shuffle bag — deal the whole playlist in random order, then
reshuffle — never does:

```pawn
static gBag[sizeof(gPlaylist)];
static gBagPos = sizeof(gPlaylist);   // start empty, forcing a shuffle

ShuffleBag()
{
    for (new i = 0; i < sizeof(gBag); i++) gBag[i] = i;

    // Fisher-Yates
    for (new i = sizeof(gBag) - 1; i > 0; i--)
    {
        new j = random(i + 1), tmp = gBag[i];
        gBag[i] = gBag[j];
        gBag[j] = tmp;
    }
    gBagPos = 0;
}

NextIndex()
{
    if (gBagPos >= sizeof(gBag)) ShuffleBag();
    return gBag[gBagPos++];
}
```

Then `QueueNext` uses `NextIndex()` instead of `gCursor++ %`.

## Jingles and idents

Station idents work well every N tracks, queued as an ordinary item so
they take their turn:

```pawn
static gSinceIdent = 0;

QueueNext(stationid)
{
    if (++gSinceIdent >= 4)
    {
        gSinceIdent = 0;
        GoRadio_QueueTrack(stationid, "idents/myfm-ident.mp3", "My FM", "");
        return;
    }

    new i = NextIndex();
    GoRadio_QueueTrack(stationid, gPlaylist[i], gTitles[i], "My FM");
}
```

For something that should play *now* — an event announcement, a server-wide
alert — interrupt instead:

```pawn
GoRadio_QueueTrack(station, "announcements/event-starting.mp3", "Event Starting", "",
    "", RADIO_QUEUE_PLAY_NOW_INTERRUPT);
```

The interrupted track fires `OnGoRadioTrackEnded` with reason
`"interrupted"`, and the queue behind it is untouched — so the rotation
picks up where it was.

## Player requests

Requests belong at the front of the queue, but shouldn't cut off what's
playing:

```pawn
CMD:request(playerid, params[])
{
    if (isnull(params))
        return SendClientMessage(playerid, -1, "Usage: /request <track>");

    new file[128], title[128];
    if (!FindTrackByName(params, file, title))
        return SendClientMessage(playerid, -1, "No such track.");

    new name[MAX_PLAYER_NAME];
    GetPlayerName(playerid, name, sizeof(name));

    new artist[160];
    format(artist, sizeof(artist), "Requested by %s", name);

    GoRadio_QueueTrack(gStation, file, title, artist, "", RADIO_QUEUE_PLAY_NEXT);
    SendClientMessage(playerid, -1, "Queued -- it'll play after this track.");
    return 1;
}
```

Putting the requester's name in the artist field means it shows up
everywhere the track's metadata does, including
`OnGoRadioTrackStarted` when it plays.

Worth adding a per-player cooldown and a cap on pending requests — with
`RADIO_QUEUE_PLAY_NEXT`, ten requests in a row will bury your rotation for
half an hour.

## Tracking what to remove

If you want a `/unrequest`, keep the queue ids:

```pawn
public OnGoRadioTrackQueued(stationid, requestid, const queueId[], queuePosition)
{
    // match requestid against what GoRadio_QueueTrack returned, store queueId
    return 1;
}
```

Then `GoRadio_Dequeue(station, queueId)`. It reports `success = false` if
the item already played, which is the normal race and not worth treating
as an error.

## A live show, then back to the playlist

A live relay never ends on its own, so the rotation stops until something
cuts it off:

```pawn
StartLiveShow(stationid)
{
    GoRadio_ClearQueue(stationid, false);   // drop pending, let the current track finish
    GoRadio_QueueTrack(stationid, "https://ice.example.com/live.mp3", "Live Show", "DJ");
}

EndLiveShow(stationid)
{
    GoRadio_Skip(stationid);                // the only way to end a live stream
    for (new i = 0; i < 3; i++) QueueNext(stationid);
}
```

While the relay is playing, `OnGoRadioQueueLow` will have fired and your
handler will have queued items behind it — which is fine, they wait. If you
don't want that, guard the handler on whether a live show is running.

## Multiple stations

Everything here is per-station. Once you have more than one, the cursors,
bags and counters need to be arrays indexed by station. See
[Running Multiple Stations](multiple-stations.md).
