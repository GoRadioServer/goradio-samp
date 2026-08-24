# Running Multiple Stations

Nothing about the plugin limits you to one station. Each gets its own
queue, its own event subscription, its own playlist logic and its own
listeners — a chill channel and a main channel, a station per faction, one
per language.

## Creating them

```pawn
static gStations[3];

enum { STATION_MAIN, STATION_CHILL, STATION_TALK };

public OnGameModeInit()
{
    gStations[STATION_MAIN]  = GoRadio_CreateStation("mcnr-main",  "MCNR Main",  "", 3);
    gStations[STATION_CHILL] = GoRadio_CreateStation("mcnr-chill", "MCNR Chill", "", 3);
    gStations[STATION_TALK]  = GoRadio_CreateStation("mcnr-talk",  "MCNR Talk",  "", 2);
    return 1;
}
```

Each registers independently, on its own thread, and gets its own stream
URL. One failing to register — a slug the token doesn't cover, say —
doesn't affect the others.

The token must authorize **every** slug:

```sh
radio tokengen --slugs mcnr-main,mcnr-chill,mcnr-talk --write
```

## Per-station state

The single biggest change from one station to several: playlist state has
to be per-station. Anything that was a global becomes an array.

```pawn
static gCursor[3];
static gSinceIdent[3];

QueueNext(index)
{
    new i = gCursor[index]++ % sizeof(gPlaylists[index]);
    GoRadio_QueueTrack(gStations[index], gPlaylists[index][i], gTitles[index][i], "MCNR");
}
```

Callbacks give you the **station id**, not your index, so you need to map
back. For a handful of stations a linear search is fine:

```pawn
StationIndex(stationid)
{
    for (new i = 0; i < sizeof(gStations); i++)
        if (gStations[i] == stationid) return i;
    return -1;
}

public OnGoRadioQueueLow(stationid, queueLength, threshold)
{
    new index = StationIndex(stationid);
    if (index == -1) return 1;      // not one of ours

    for (new i = queueLength; i <= threshold; i++) QueueNext(index);
    return 1;
}
```

!!! warning "Always guard on the station id"
    Callbacks fire for every station, including any a *filterscript*
    created. A `OnGoRadioTrackStarted` handler that announces to everyone
    without checking will broadcast your background station's every track.

## Iterating stations

For anything generic — an admin listing, a stats dump — walk them without
your own array. Ids aren't contiguous after a station is destroyed, so go
by index:

```pawn
CMD:radiostats(playerid, params[])
{
    new slug[64], title[128], msg[200];

    for (new i = 0, n = GoRadio_GetStationCount(); i < n; i++)
    {
        new station = GoRadio_GetStationAtIndex(i);
        GoRadio_GetStationSlug(station, slug);

        if (!GoRadio_GetCurrentTrackTitle(station, title))
            format(title, sizeof(title), "(silent)");

        format(msg, sizeof(msg), "%s: %d listening -- %s", slug,
            GoRadio_GetListenerCount(station), title);
        SendClientMessage(playerid, -1, msg);
    }
    return 1;
}
```

`GoRadio_GetStationBySlug` is the other way round, and useful for commands
that take a station name.

## Letting players choose

```pawn
CMD:tune(playerid, params[])
{
    new station = GoRadio_GetStationBySlug(params);
    if (station == INVALID_RADIO_STATION)
        return SendClientMessage(playerid, -1, "Unknown station. Try: main, chill, talk");

    new url[160];
    if (!GoRadio_GetStreamURL(station, url))
        return SendClientMessage(playerid, -1, "That station isn't ready yet.");

    StopAudioStreamForPlayer(playerid);
    PlayAudioStreamForPlayer(playerid, url);
    return 1;
}
```

Track which station each player is on if you want to announce track
changes only to its listeners:

```pawn
static gPlayerStation[MAX_PLAYERS];

public OnGoRadioTrackStarted(stationid, const queueId[], const location[], const title[],
    const artist[], const coverArt[], durationSeconds)
{
    new msg[144];
    format(msg, sizeof(msg), "{88CCFF}[Radio]{FFFFFF} %s - %s", artist, title);

    for (new i = 0; i < MAX_PLAYERS; i++)
        if (IsPlayerConnected(i) && gPlayerStation[i] == stationid)
            SendClientMessage(i, -1, msg);
    return 1;
}
```

Reset `gPlayerStation[playerid]` to `INVALID_RADIO_STATION` in
`OnPlayerConnect`.

## Grouping with metadata

If several servers share one audio server, metadata is how a dashboard
tells them apart:

```pawn
GoRadio_SetStationMetadata(station, "group", "mcnr");
GoRadio_SetStationMetadata(station, "server", "eu-1");
GoRadio_SetStationMetadata(station, "genre", "mixed");
```

The audio server stores these without interpreting them and returns them in
status and listings. Set them before `GoRadio_CreateStation`, or push a
later change with `GoRadio_UpdateStation`.

## Cost

Each station adds:

- one thread, parked on its event subscription almost all the time;
- one HTTP connection;
- one `GetStatus` per poll interval, shared across the worker pool.

That's cheap — a handful of stations is unremarkable. The worker pool
(`goradio_workers`, default 2) is shared, so if you drive many stations
that queue in bursts, raising it to 3 or 4 is reasonable. There's little
point beyond that: these calls complete in milliseconds.
