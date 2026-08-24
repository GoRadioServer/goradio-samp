# Playback & Queue

Every native on this page is a **command**: it returns a request id
(`> 0`) immediately, or `0` if the station id was unknown, and answers
later in a callback.

`GoRadio_QueueTrack` answers in
[`OnGoRadioTrackQueued`](callbacks.md#ongoradiotrackqueued). Everything
else answers in
[`OnGoRadioCommandResult`](callbacks.md#ongoradiocommandresult).

## `GoRadio_QueueTrack`

```pawn
native GoRadio_QueueTrack(stationid, const location[], const title[] = "",
    const artist[] = "", const coverArt[] = "", mode = RADIO_QUEUE_APPEND);
```

Queues something to play.

**`location`** is either a path relative to the audio server's configured
audio root, or an `http(s)://` URL. Which one is **inferred from the
string**, not declared — anything starting `http://` or `https://` is a
URL, everything else is a local path.

An HTTP source can be a finite file (fetched and cached like any other) or
a continuous live stream such as an Icecast mountpoint, which is re-encoded
and relayed in real time. The audio server works out which; you don't
declare it. A live source has **no natural end** — it plays until
`GoRadio_Skip` or a `RADIO_QUEUE_PLAY_NOW_INTERRUPT` item cuts it off.

**`title`, `artist`, `coverArt`** are display metadata, carried through
unchanged to status, events and listings. The audio server never fetches or
validates the cover art URL; it just passes it along.

**`mode`** is where the item goes:

| Mode | Effect |
|---|---|
| `RADIO_QUEUE_APPEND` | The back of the queue. The default, and what you want almost always. |
| `RADIO_QUEUE_PLAY_NEXT` | The front — plays when the current item finishes. |
| `RADIO_QUEUE_PLAY_NOW_INTERRUPT` | The front, cutting the current item off immediately. |

```pawn
// Ordinary playlist item
GoRadio_QueueTrack(station, "music/night-drive.mp3", "Night Drive", "My FM");

// A player request, jumping the queue
GoRadio_QueueTrack(station, "music/requested.mp3", "Requested Track", "My FM",
    "", RADIO_QUEUE_PLAY_NEXT);

// A breaking announcement, cutting in right now
GoRadio_QueueTrack(station, "announcements/event-starting.mp3", "Event Starting", "",
    "", RADIO_QUEUE_PLAY_NOW_INTERRUPT);

// A live relay -- plays until skipped
GoRadio_QueueTrack(station, "https://ice.example.com/live.mp3", "Live Show", "DJ Someone");
```

!!! tip "Queue ahead of time"
    The audio server starts prefetching the moment you queue, not when the
    item is about to play. An item queued three tracks early is already
    fetched and decoded when its turn comes; one queued the instant the
    previous track ends risks a gap while it downloads.

**Success means accepted, not playable.** A source that fails to fetch or
decode surfaces later through [`OnGoRadioError`](errors.md), not as a
failure of this call. The signal that something is actually playing is
[`OnGoRadioTrackStarted`](callbacks.md#ongoradiotrackstarted).

The queue id you need for `GoRadio_Dequeue` and `GoRadio_SkipTo` arrives
in `OnGoRadioTrackQueued`:

```pawn
public OnGoRadioTrackQueued(stationid, requestid, const queueId[], queuePosition)
{
    printf("queued at position %d, id %s", queuePosition, queueId);
    return 1;
}
```

## `GoRadio_Dequeue`

```pawn
native GoRadio_Dequeue(stationid, const queueId[]);
```

Removes one still-pending item.

Reports `success = false` — not an error — if the id wasn't in the queue,
because it already played or was already removed.

**You cannot dequeue what is currently playing.** That item has left the
queue by the time it starts. Use [`GoRadio_Skip`](#goradio_skip) for that.

The result arrives as `OnGoRadioCommandResult` with command
`"RemoveFromQueue"` — the RPC's name, not the native's.

## `GoRadio_ClearQueue`

```pawn
native GoRadio_ClearQueue(stationid, bool:stopCurrent = false);
```

Removes every pending item. With `stopCurrent = true` the currently
playing item is cut off as well, dropping the station to silence — since
the queue was just emptied, there's nothing to replace it with.

The usual "clean slate" call:

```pawn
GoRadio_ClearQueue(station, true);
GoRadio_QueueTrack(station, "music/fresh-start.mp3", "Fresh Start", "My FM");
```

`success` is always `true` when the call lands; the result value is how
many pending items were removed.

## `GoRadio_Skip`

```pawn
native GoRadio_Skip(stationid);
```

Cuts the current item short and moves straight to the next pending one, or
to silence if the queue is empty. The rest of the queue is untouched.

`success = false` means there was nothing playing to skip.

This is the **only** way to end a live stream, which otherwise plays
forever.

## `GoRadio_SkipTo`

```pawn
native GoRadio_SkipTo(stationid, const queueId[]);
```

Jumps straight to a specific pending item: everything ahead of it in the
queue is dropped, and the current item is cut off so the target starts
immediately.

Identify it by **queue id, not position** — your view of positions can be
stale by the time the call lands.

The result value is how many items were dropped. Passing an id that isn't
a pending item is an error, not a `false` result — you can't `SkipTo`
whatever is already playing, since it has left the queue.

## `GoRadio_Pause` / `GoRadio_Resume`

```pawn
native GoRadio_Pause(stationid);
native GoRadio_Resume(stationid);
```

`Pause` holds the current item's position and drops the station to the
silence loop, exactly as if the queue were empty. `Resume` picks up from
where it left off.

`Pause` reports `success = false` if nothing was playing, it was already
paused, or the current item is a **live stream** — there's no fixed
position to hold. `Resume` reports `false` if the station wasn't paused.

While paused, [`GoRadio_IsPaused`](state.md#playback-flags) and
[`GoRadio_IsSilence`](state.md#playback-flags) are both `1`, and
`GoRadio_IsPlaying` stays `1` because there is still a current track.

## `GoRadio_Seek` / `GoRadio_SeekBy`

```pawn
native GoRadio_Seek(stationid, positionSeconds);
native GoRadio_SeekBy(stationid, deltaSeconds);
```

`Seek` jumps to an absolute position; `SeekBy` moves by a signed delta
(positive forward, negative back). Both are clamped to `[0, duration]`.

Both work whether or not the station is paused — seeking while paused just
moves where `Resume` will pick up.

The result value is the resulting position after clamping.
`success = false` means nothing seekable was playing: no current track, or
a live stream.

```pawn
GoRadio_SeekBy(station, -15);   // rewind 15 seconds
GoRadio_Seek(station, 0);       // back to the start
```

## Reading a command's result

```pawn
public OnGoRadioCommandResult(stationid, requestid, const command[], bool:success, result)
{
    if (!strcmp(command, "ClearQueue"))
        printf("cleared %d pending items", result);
    return 1;
}
```

| Native | `command` | `success` | `result` |
|---|---|---|---|
| `GoRadio_Dequeue` | `"RemoveFromQueue"` | whether the item was found | `1` / `0` |
| `GoRadio_ClearQueue` | `"ClearQueue"` | always true if it landed | items removed |
| `GoRadio_Skip` | `"Skip"` | whether anything was skipped | `1` / `0` |
| `GoRadio_SkipTo` | `"SkipTo"` | always true if it landed | items dropped |
| `GoRadio_Pause` | `"Pause"` | whether it paused | `1` / `0` |
| `GoRadio_Resume` | `"Resume"` | whether it resumed | `1` / `0` |
| `GoRadio_Seek` | `"Seek"` | whether it seeked | resulting position |
| `GoRadio_SeekBy` | `"SeekBy"` | whether it seeked | resulting position |
| `GoRadio_QueueTrack` | `"QueueTrack"` | **only on failure** | — |

`GoRadio_QueueTrack` is the odd one: on success it answers through
`OnGoRadioTrackQueued` only. It appears here **only when it failed**, so a
`OnGoRadioCommandResult` with command `"QueueTrack"` always means
something went wrong, and `OnGoRadioError` will have fired too.

!!! note "`success = false` usually isn't an error"
    Several of these report "there was nothing to do" that way: nothing
    was playing to skip, the queue id had already gone, the station was
    already paused. A genuine failure — a network problem, a bad token, an
    unknown slug — also fires [`OnGoRadioError`](errors.md). If you only
    care about real problems, watch that callback instead.
