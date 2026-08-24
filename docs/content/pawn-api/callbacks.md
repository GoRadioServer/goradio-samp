# Callbacks

Every callback is offered to the **gamemode and every loaded
filterscript**, so a filterscript can react to a station the gamemode
created. Return values are ignored — returning `0` does not stop other
scripts from seeing the callback.

All of them run on the main thread, from `ProcessTick`. Normal PAWN rules
apply; you can call any SA-MP function from inside them.

## `OnGoRadioServerInfo`

```pawn
forward OnGoRadioServerInfo(const version[]);
```

The audio server answered for the first time. This is the signal that the
URL is reachable **and** the token was accepted — if it never fires, see
[Troubleshooting](../guides/troubleshooting.md).

`version` is the audio server's build, e.g. `"v0.9.0"`, or `"dev"` for a
locally built binary.

## `OnGoRadioStationRegistered`

```pawn
forward OnGoRadioStationRegistered(stationid, const streamUrl[], bool:reRegistered,
    bool:afterReconnect);
```

The station is registered and ready for commands. **Fires on the first
registration and again after every reconnect.**

This is where a station's opening track belongs — and where you must
re-prime the queue, because an audio-server restart brings the station
back empty:

```pawn
public OnGoRadioStationRegistered(stationid, const streamUrl[], bool:reRegistered,
    bool:afterReconnect)
{
    if (GoRadio_GetQueueLength(stationid) == 0)
        QueueSomeTracks(stationid);
    return 1;
}
```

| Argument | Meaning |
|---|---|
| `streamUrl` | Where listeners connect. Also readable later via `GoRadio_GetStreamURL`. |
| `reRegistered` | `true` if the audio server already knew this slug. `false` means it was created fresh — so its queue is empty. |
| `afterReconnect` | `true` if this was a reconnect rather than the initial registration at startup. |

The four combinations, and the reason the queue-length check is preferred
over reading the flags, are in
[Station Lifecycle](../how-it-works/station-lifecycle.md#why-re-priming-is-your-job).

## `OnGoRadioTrackStarted`

```pawn
forward OnGoRadioTrackStarted(stationid, const queueId[], const location[], const title[],
    const artist[], const coverArt[], durationSeconds);
```

A queued item actually started playing. This — not `GoRadio_QueueTrack`
returning — is the reliable "now playing" signal.

`durationSeconds` is `0` when the length is unknown or indefinite, which is
always the case for a live stream.

```pawn
public OnGoRadioTrackStarted(stationid, const queueId[], const location[], const title[],
    const artist[], const coverArt[], durationSeconds)
{
    new msg[144];
    format(msg, sizeof(msg), "{88CCFF}[Radio]{FFFFFF} Now playing: %s - %s", artist, title);
    SendClientMessageToAll(-1, msg);
    return 1;
}
```

!!! tip "Filter by station"
    With more than one station, guard on `stationid` — otherwise a track
    starting on your quiet background station announces itself to everyone.

## `OnGoRadioTrackEnded`

```pawn
forward OnGoRadioTrackEnded(stationid, const queueId[], const reason[]);
```

A track finished. `reason` is:

- `"completed"` — it played to its natural end.
- `"interrupted"` — it was cut short by a skip, a
  `RADIO_QUEUE_PLAY_NOW_INTERRUPT` item, or a queue clear.

Good place to maintain your own "recently played" list, which can be
longer than the audio server's capped history.

## `OnGoRadioTrackQueued`

```pawn
forward OnGoRadioTrackQueued(stationid, requestid, const queueId[], queuePosition);
```

The answer to [`GoRadio_QueueTrack`](playback.md#goradio_queuetrack).

`requestid` matches what the native returned, so you can correlate when
several queue calls are in flight. `queueId` is the id you need for
`GoRadio_Dequeue` and `GoRadio_SkipTo`. `queuePosition` is `0` when the
item is about to play immediately.

Only fires on **success**. A failed queue call fires
[`OnGoRadioCommandResult`](#ongoradiocommandresult) with command
`"QueueTrack"` and [`OnGoRadioError`](errors.md) instead.

## `OnGoRadioQueueLow`

```pawn
forward OnGoRadioQueueLow(stationid, queueLength, threshold);
```

The pending queue dropped to or below the station's `lowQueueThreshold`.
Requires that threshold to have been set `> 0` at creation.

**Edge-triggered**: it fires once on the way into "low", and not again
until the queue climbs back above the threshold and dips a second time. So
top the queue up *past* the threshold rather than adding one item:

```pawn
public OnGoRadioQueueLow(stationid, queueLength, threshold)
{
    for (new i = queueLength; i <= threshold; i++)
        QueueNextTrack(stationid);
    return 1;
}
```

This does **not** cover a station whose queue was emptied by an
audio-server restart — that queue never transitions into "low", because it
was never anything else. See
[`OnGoRadioStationRegistered`](#ongoradiostationregistered).

## `OnGoRadioQueueUpdated`

```pawn
forward OnGoRadioQueueUpdated(stationid, queueLength);
```

The pending queue length changed, for any reason. Fires more often than
`OnGoRadioQueueLow` — on every queue, dequeue and track start — so it's
suited to keeping a live display current, not to driving playlist logic.

## `OnGoRadioListenerCountChanged`

```pawn
forward OnGoRadioListenerCountChanged(stationid, listenerCount);
```

Someone connected to or disconnected from the station's stream.

## `OnGoRadioSilenceStarted` / `OnGoRadioSilenceEnded`

```pawn
forward OnGoRadioSilenceStarted(stationid);
forward OnGoRadioSilenceEnded(stationid);
```

The station fell back to, or came off, the silence loop. Silence means the
queue ran dry or playback is paused.

`OnGoRadioSilenceStarted` firing unexpectedly is usually the first visible
symptom of a playlist that stopped feeding itself.

## `OnGoRadioStatusUpdate`

```pawn
forward OnGoRadioStatusUpdate(stationid);
```

A fresh status snapshot landed and the `GoRadio_Get*` natives now reflect
it. Fires on every poll, so at `goradio_poll_interval` seconds by default —
don't do heavy work here.

Useful for refreshing a textdraw or admin panel exactly when the data
behind it changed.

## `OnGoRadioCommandResult`

```pawn
forward OnGoRadioCommandResult(stationid, requestid, const command[], bool:success, result);
```

The answer to any playback command except a successful `GoRadio_QueueTrack`.

`command` is the RPC name — `"Skip"`, `"Pause"`, `"ClearQueue"`,
`"RemoveFromQueue"`, `"SkipTo"`, `"Resume"`, `"Seek"`, `"SeekBy"`,
`"QueueTrack"` — and `result` carries whatever number that command
reports. The full table is in
[Playback](playback.md#reading-a-commands-result).

`success = false` is usually not an error: several commands report "there
was nothing to do" that way. A genuine failure also fires
[`OnGoRadioError`](errors.md).

## `OnGoRadioStationList`

```pawn
forward OnGoRadioStationList(count);
```

[`GoRadio_RequestStationList`](state.md#every-station-on-the-audio-server)
finished; `count` entries are now readable through the
`GoRadio_GetListedStation*` getters.

## `OnGoRadioError`

```pawn
forward OnGoRadioError(stationid, const code[], const message[]);
```

Something went wrong. `stationid` is `INVALID_RADIO_STATION` for errors not
tied to a station.

This covers both transport problems (bad token, unreachable server) and
playback problems reported by the audio server (a track that couldn't be
fetched or decoded). Codes, what's retried and what to do about each are on
[Errors](errors.md).

## Callback order

For a typical track change you'll see, in order:

1. `OnGoRadioTrackEnded` — the previous item finished
2. `OnGoRadioTrackStarted` — the next one began
3. `OnGoRadioQueueUpdated` — the queue shrank
4. `OnGoRadioQueueLow` — if that took it to the threshold
5. `OnGoRadioStatusUpdate` — the refresh triggered by the track change

Events from one station always arrive in the order the audio server sent
them. Ordering between an event and an unrelated command's result is not
guaranteed — they travel on different connections.
