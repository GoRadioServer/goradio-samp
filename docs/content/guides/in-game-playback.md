# Playing the Stream In-Game

The plugin controls *what plays*. Getting it into a player's ears is
ordinary SA-MP: `PlayAudioStreamForPlayer` with the station's stream URL.

## The basics

```pawn
CMD:radio(playerid, params[])
{
    new url[160];
    if (!GoRadio_GetStreamURL(gStation, url))
        return SendClientMessage(playerid, -1, "The radio isn't ready yet.");

    PlayAudioStreamForPlayer(playerid, url);
    SendClientMessage(playerid, -1, "Radio on. /radiooff to stop.");
    return 1;
}

CMD:radiooff(playerid, params[])
{
    StopAudioStreamForPlayer(playerid);
    return 1;
}
```

`GoRadio_GetStreamURL` returns `0` with an empty string until the station
has registered, so the check is worth keeping — handing an empty URL to
`PlayAudioStreamForPlayer` does nothing visible and is confusing to debug.

## Players who join mid-track

The stream is continuous and live, so a player who tunes in halfway
through a track hears it from wherever it currently is. That's correct
radio behaviour, but it means you should tell them what's playing rather
than relying on having caught `OnGoRadioTrackStarted`:

```pawn
AnnounceNowPlaying(playerid, stationid)
{
    new title[128], artist[128], msg[300];
    if (!GoRadio_GetCurrentTrackTitle(stationid, title))
        return SendClientMessage(playerid, -1, "Nothing playing right now.");

    GoRadio_GetCurrentTrackArtist(stationid, artist);

    new elapsed  = GoRadio_GetCurrentTrackElapsed(stationid);
    new duration = GoRadio_GetCurrentTrackDuration(stationid);

    if (duration > 0)
        format(msg, sizeof(msg), "Now playing: %s - %s [%d:%02d / %d:%02d]",
            artist, title, elapsed / 60, elapsed % 60, duration / 60, duration % 60);
    else
        format(msg, sizeof(msg), "Now playing: %s - %s [live]", artist, title);

    return SendClientMessage(playerid, -1, msg);
}
```

The cached getters are what make this cheap — no request, no waiting.

## Auto-tuning on connect

```pawn
public OnPlayerConnect(playerid)
{
    gPlayerRadioOn[playerid] = false;
    return 1;
}

public OnPlayerSpawn(playerid)
{
    if (!gPlayerRadioOn[playerid] && GetPVarInt(playerid, "AutoRadio"))
    {
        new url[160];
        if (GoRadio_GetStreamURL(gStation, url))
        {
            PlayAudioStreamForPlayer(playerid, url);
            gPlayerRadioOn[playerid] = true;
        }
    }
    return 1;
}
```

Make it opt-in and remembered. Audio starting unbidden is the fastest way
to make players turn it off permanently.

## Positional audio

`PlayAudioStreamForPlayer` takes an optional position and radius, which
makes the stream audible only near a point — a club, a car radio, a
stage:

```pawn
// Audible within 30 units of the club
PlayAudioStreamForPlayer(playerid, url, 2000.0, 1500.0, 25.0, 30.0, 1);
```

Combine with `OnPlayerEnterCheckpoint`/area checks, or a streamer plugin's
area callbacks, to start and stop it as players move. Note the client
buffers a few seconds, so it fades in rather than starting instantly.

## Per-vehicle radio

```pawn
public OnPlayerStateChange(playerid, newstate, oldstate)
{
    if (newstate == PLAYER_STATE_DRIVER || newstate == PLAYER_STATE_PASSENGER)
    {
        new vehicleid = GetPlayerVehicleID(playerid);
        new station = GetVehicleRadioStation(vehicleid);   // your own mapping

        if (station != INVALID_RADIO_STATION)
        {
            new url[160];
            if (GoRadio_GetStreamURL(station, url))
                PlayAudioStreamForPlayer(playerid, url);
        }
    }
    else if (oldstate == PLAYER_STATE_DRIVER || oldstate == PLAYER_STATE_PASSENGER)
    {
        StopAudioStreamForPlayer(playerid);
    }
    return 1;
}
```

Consider disabling the client's built-in radio for the vehicle so the two
don't overlap.

## A now-playing textdraw

Update it from the callbacks rather than a fast timer — the data only
changes when a track does:

```pawn
public OnGoRadioTrackStarted(stationid, const queueId[], const location[], const title[],
    const artist[], const coverArt[], durationSeconds)
{
    new text[160];
    format(text, sizeof(text), "~w~%s~n~~y~%s", title, artist);
    TextDrawSetString(gNowPlayingTD, text);
    return 1;
}
```

For a progress bar, a one-second timer reading
`GoRadio_GetCurrentTrackElapsed` is fine — it extrapolates between polls,
so it advances a second at a time rather than jumping.

## What the player hears versus what the plugin knows

They are not quite in step, and it shows up if you look for it:

- The **client buffers** several seconds. `OnGoRadioTrackStarted` fires
  when the audio server starts a track; the player hears it a few seconds
  later. A now-playing display is slightly *ahead* of the audio.
- `GoRadio_GetCurrentTrackElapsed` tracks the **audio server's** position,
  not the player's.
- Players who tuned in at different times are all hearing the same live
  point in the stream; it isn't per-player.

None of this is worth engineering around for a radio station. It matters if
you try to sync anything visual tightly to the audio, which this isn't
built for.

## Listening outside the game

The stream URL is an ordinary HTTP audio stream — it works in VLC, in a
browser, on a phone. Showing it in a `/radio` command lets players listen
while they're not in game:

```pawn
new url[160], msg[200];
GoRadio_GetStreamURL(gStation, url);
format(msg, sizeof(msg), "Listen anywhere: %s", url);
SendClientMessage(playerid, -1, msg);
```

Bear in mind those listeners count towards
`GoRadio_GetListenerCount`, which is a station-wide number and not a count
of in-game players.
