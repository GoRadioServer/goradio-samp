# Station State

Everything on this page reads a **locally cached snapshot**. No native
here makes a request or blocks — call them in a timer, per player, in a
loop, it doesn't matter.

The cache is kept current from two sources: the live event stream, which
pushes changes as they happen, and a periodic `GetStatus`, every
`goradio_poll_interval` seconds and after every track change. See
[Architecture](../how-it-works/architecture.md#the-status-cache).

## `GoRadio_HasStatus`

```pawn
native GoRadio_HasStatus(stationid);
```

`1` once at least one snapshot has arrived. Before that every getter below
reads as empty or zero, which is indistinguishable from a genuinely idle
station — so check this if the difference matters.

## `GoRadio_RefreshStatus`

```pawn
native GoRadio_RefreshStatus(stationid);
```

Asks for a fresh snapshot now, rather than waiting for the next poll.
[`OnGoRadioStatusUpdate`](callbacks.md#ongoradiostatusupdate) fires when it
lands.

You rarely need this — the plugin already refreshes on the events that
matter. It's occasionally useful right before rendering a detailed admin
display.

## Listeners and uptime

```pawn
native GoRadio_GetListenerCount(stationid);
native GoRadio_GetUptime(stationid);
```

`GoRadio_GetListenerCount` is how many clients are currently connected to
the station's stream. It's updated by events as well as polls, so it
tracks closely.

`GoRadio_GetUptime` is how long the station has been registered on the
audio server, in seconds. Note it survives your gamemode restarting — it's
the *station's* uptime, not your script's.

## Playback flags

```pawn
native GoRadio_IsPlaying(stationid);
native GoRadio_IsSilence(stationid);
native GoRadio_IsPaused(stationid);
```

| | `IsPlaying` | `IsSilence` | `IsPaused` |
|---|---|---|---|
| Playing a track | `1` | `0` | `0` |
| Paused | `1` | `1` | `1` |
| Queue empty, silence loop | `0` | `1` | `0` |

`IsPlaying` means "there is a current track", which a paused station still
has. `IsSilence` means the silence loop is what listeners are hearing,
which covers both an empty queue and a pause.

## The current track

```pawn
native GoRadio_GetCurrentTrackID(stationid, dest[], len = sizeof dest);
native GoRadio_GetCurrentTrackTitle(stationid, dest[], len = sizeof dest);
native GoRadio_GetCurrentTrackArtist(stationid, dest[], len = sizeof dest);
native GoRadio_GetCurrentTrackLocation(stationid, dest[], len = sizeof dest);
native GoRadio_GetCurrentTrackCoverArt(stationid, dest[], len = sizeof dest);

native GoRadio_GetCurrentTrackDuration(stationid);
native GoRadio_GetCurrentTrackElapsed(stationid);
```

The string getters return `0` and write an empty string when nothing is
playing. `GoRadio_GetCurrentTrackID` is the queue id — the same one
`OnGoRadioTrackQueued` gave you.

`GoRadio_GetCurrentTrackDuration` is `0` when the length is unknown or
indefinite — **always** the case for a live stream, and briefly true for an
item whose prefetch hasn't resolved yet.

`GoRadio_GetCurrentTrackElapsed` **extrapolates between snapshots**: the
plugin records when the snapshot was taken and adds the wall time since,
clamped to the duration. So it advances smoothly, second by second, rather
than jumping once per poll — good enough to drive a progress bar.

```pawn
new title[128], artist[128], msg[300];
if (GoRadio_GetCurrentTrackTitle(station, title))
{
    GoRadio_GetCurrentTrackArtist(station, artist);

    new elapsed  = GoRadio_GetCurrentTrackElapsed(station);
    new duration = GoRadio_GetCurrentTrackDuration(station);

    if (duration > 0)
        format(msg, sizeof(msg), "%s - %s [%d:%02d / %d:%02d]", artist, title,
            elapsed / 60, elapsed % 60, duration / 60, duration % 60);
    else
        format(msg, sizeof(msg), "%s - %s [live]", artist, title);

    SendClientMessage(playerid, -1, msg);
}
```

Always branch on `duration > 0` before showing progress — a live relay has
no length, and formatting `x / 0:00` looks broken.

## The queue

```pawn
native GoRadio_GetQueueLength(stationid);

native GoRadio_GetQueueItemCount(stationid);
native GoRadio_GetQueueItemID(stationid, index, dest[], len = sizeof dest);
native GoRadio_GetQueueItemTitle(stationid, index, dest[], len = sizeof dest);
native GoRadio_GetQueueItemArtist(stationid, index, dest[], len = sizeof dest);
native GoRadio_GetQueueItemLocation(stationid, index, dest[], len = sizeof dest);
native GoRadio_GetQueueItemDuration(stationid, index);
```

There are two counts here, and the difference matters:

- **`GoRadio_GetQueueLength`** is the live count. Queue-updated events
  carry a fresh length, so this is current to within network latency.
- **`GoRadio_GetQueueItemCount`** is how many items are in the last
  *snapshot* — the ones you can actually read details for. It can lag
  `GoRadio_GetQueueLength` by up to one poll interval.

Use `GoRadio_GetQueueLength` for decisions ("is the queue getting
short?"), and `GoRadio_GetQueueItemCount` to bound a loop over item
details.

Index `0` is next up.

```pawn
new count = GoRadio_GetQueueItemCount(station), title[128], msg[160];
for (new i = 0; i < count; i++)
{
    GoRadio_GetQueueItemTitle(station, i, title);
    format(msg, sizeof(msg), "%d. %s", i + 1, title);
    SendClientMessage(playerid, -1, msg);
}
```

`GoRadio_GetQueueItemDuration` is `0` for a live stream, and also for an
item whose prefetch hasn't resolved a duration yet — a pending item can
legitimately report `0` and then a real length a moment later.

## History

```pawn
native GoRadio_GetHistoryCount(stationid);
native GoRadio_GetHistoryTitle(stationid, index, dest[], len = sizeof dest);
native GoRadio_GetHistoryArtist(stationid, index, dest[], len = sizeof dest);
native GoRadio_GetHistoryReason(stationid, index, dest[], len = sizeof dest);
```

Recently finished items, **oldest first**, capped by the audio server at a
small fixed count.

`reason` is `"completed"` or `"interrupted"` — the same values
[`OnGoRadioTrackEnded`](callbacks.md#ongoradiotrackended) reports.

This is meant to **seed** a "recently played" display when your script
starts, since it covers tracks that played before you were watching. Keep
it current from `OnGoRadioTrackEnded` afterwards rather than re-reading it
— your own list can be as long as you like, where this one is capped.

## Every station on the audio server

```pawn
native GoRadio_RequestStationList();
native GoRadio_GetListedStationCount();
native GoRadio_GetListedStationSlug(index, dest[], len = sizeof dest);
native GoRadio_GetListedStationName(index, dest[], len = sizeof dest);
native GoRadio_GetListedStationLogo(index, dest[], len = sizeof dest);
native GoRadio_GetListedListenerCount(index);
```

Unlike everything else on this page, these are not scoped to a station
this server created. `GoRadio_RequestStationList` asks the audio server for
**every station the token authorizes** — including ones registered by
other game servers, by Lua controllers, or by hand.

It's a request, so it answers in
[`OnGoRadioStationList`](callbacks.md#ongoradiostationlist); the getters
read from the cached result afterwards.

```pawn
public OnGoRadioStationList(count)
{
    new slug[64], name[128];
    for (new i = 0; i < count; i++)
    {
        GoRadio_GetListedStationSlug(i, slug);
        GoRadio_GetListedStationName(i, name);
        printf("%s (%s): %d listening", name, slug, GoRadio_GetListedListenerCount(i));
    }
    return 1;
}
```

The listing is deliberately light — slug, name, listener count, logo — with
no queue or history. For that detail on a station, you need one you own.
