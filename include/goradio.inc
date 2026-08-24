/*
 * goradio.inc -- PAWN API for the goradio SA-MP plugin.
 *
 * Drives one or more stations on a GoRadio audio server (`radio serve`)
 * over its HTTP+JSON Connect API. Keep this file in sync with
 * src/natives.cpp; every native here is registered there under the same
 * name and argument order.
 *
 * The shape of the API, in one paragraph: everything that talks to the
 * audio server is asynchronous. A command native returns a *request id*
 * immediately and the answer arrives later in a callback -- nothing here
 * ever blocks a server frame on the network. The GoRadio_Get* natives are
 * the exception: they read a locally cached snapshot of the station, so
 * they answer instantly and are safe to call as often as you like.
 */

#if defined _INC_goradio
	#endinput
#endif
#define _INC_goradio

#define GORADIO_INC_VERSION 100

/* Returned by GoRadio_CreateStation / GoRadio_GetStationBySlug when there
 * is no such station. */
#define INVALID_RADIO_STATION           (-1)

/* Where a queued item goes. Mirrors audioserver.v1.QueueMode. */
#define RADIO_QUEUE_APPEND              (0)  /* the back of the queue (default) */
#define RADIO_QUEUE_PLAY_NEXT           (1)  /* the front; plays when the current item finishes */
#define RADIO_QUEUE_PLAY_NOW_INTERRUPT  (2)  /* the front, cutting the current item off now */


/* =====================================================================
 * Connection
 * ===================================================================== */

/*
 * Points the plugin at an audio server. Only needed if you haven't set
 * goradio_url / goradio_token in server.cfg, which is the usual way --
 * a token in a gamemode is a token in your source control.
 *
 * Must be called before any station is created; changing servers with
 * stations already running is refused, since those registrations belong
 * to the old server.
 *
 * url may be "http://host:port" (or https:// on a TLS-enabled build).
 * Returns 1 if the settings were accepted -- not that the server is
 * reachable, which you learn from OnGoRadioServerInfo.
 */
native GoRadio_SetServer(const url[], const token[]);

/* 1 once an audio server has been configured (from server.cfg or
 * GoRadio_SetServer). Says nothing about reachability. */
native GoRadio_IsReady();

/* The audio server's build version, once OnGoRadioServerInfo has fired.
 * Returns 0 (and an empty string) before that. */
native GoRadio_GetServerVersion(dest[], len = sizeof dest);


/* =====================================================================
 * Creating and managing stations
 * ===================================================================== */

/*
 * Creates a station and registers it with the audio server. Returns a
 * station id immediately, or INVALID_RADIO_STATION if the slug is empty,
 * already in use here, or no server is configured.
 *
 * Registration itself happens in the background and retries until it
 * succeeds, so a station created while the audio server is down comes up
 * on its own once it's back. Wait for OnGoRadioStationRegistered before
 * queueing anything: an RPC against a slug the audio server doesn't know
 * yet fails with not_found.
 *
 *   slug                the station's id on the audio server, and the
 *                       last part of its stream URL
 *   name                display name (defaults to slug)
 *   description         free text, shown in dashboards
 *   lowQueueThreshold   > 0 enables OnGoRadioQueueLow when the pending
 *                       queue drops to or below this many items; 0 off
 *   logoUrl             station artwork, surfaced via the audio server
 */
native GoRadio_CreateStation(const slug[], const name[] = "", const description[] = "",
	lowQueueThreshold = 0, const logoUrl[] = "");

/*
 * Unregisters the station and frees the id. The audio server stops its
 * player and disconnects anyone listening; the queue is not kept
 * anywhere, so re-creating the same slug later starts empty.
 */
native GoRadio_DestroyStation(stationid);

native GoRadio_IsValidStation(stationid);

/* The station id for a slug this server created, or
 * INVALID_RADIO_STATION. Only searches this plugin's own stations -- use
 * GoRadio_RequestStationList for everything on the audio server. */
native GoRadio_GetStationBySlug(const slug[]);

/* Iterating this server's stations. Ids are not contiguous once a
 * station has been destroyed, so walk them by index rather than
 * counting up from 0. */
native GoRadio_GetStationCount();
native GoRadio_GetStationAtIndex(index);

/*
 * Freeform key/value data attached to the station -- a group name, a
 * genre tag, whatever your dashboard wants. The audio server stores and
 * returns these without interpreting them. Passing an empty value
 * removes the key.
 *
 * Metadata is sent with the next registration, so set it before
 * GoRadio_CreateStation, or follow a later change with
 * GoRadio_UpdateStation to push it.
 */
native GoRadio_SetStationMetadata(stationid, const key[], const value[]);
native GoRadio_ClearStationMetadata(stationid);

/*
 * Re-registers the station with new details. The audio server updates it
 * in place -- playback, the queue and current listeners are undisturbed.
 * Every field is replaced, including metadata, so pass the values you
 * want to keep rather than only the ones you're changing.
 */
native GoRadio_UpdateStation(stationid, const name[], const description[] = "",
	lowQueueThreshold = 0, const logoUrl[] = "");

native GoRadio_GetStationSlug(stationid, dest[], len = sizeof dest);

/* The listening URL the audio server assigned. Empty until the station
 * has registered. */
native GoRadio_GetStreamURL(stationid, dest[], len = sizeof dest);

/* 1 once the audio server has confirmed the registration. Goes back to 0
 * while a dropped connection is being re-established. */
native GoRadio_IsStationRegistered(stationid);


/* =====================================================================
 * Playback
 *
 * Each of these returns a request id (> 0) you can match against
 * OnGoRadioCommandResult, or 0 if the station id is unknown. They return
 * as soon as the request is queued locally -- never wait on one.
 * ===================================================================== */

/*
 * Queues something to play. location is either a path relative to the
 * audio server's audio root, or an http(s):// URL -- which is inferred
 * from the string, not declared.
 *
 * An http(s) source can be a finite file or a continuous live stream
 * (an Icecast mountpoint, say); the audio server works out which and
 * relays a live one in real time. A live source has no natural end: it
 * plays until GoRadio_Skip or a RADIO_QUEUE_PLAY_NOW_INTERRUPT item cuts
 * it off.
 *
 * The answer arrives as OnGoRadioTrackQueued, which carries the queue id
 * you need for GoRadio_Dequeue and GoRadio_SkipTo. Note that a queued
 * item is only *accepted*, not yet proven playable -- a source that
 * fails to fetch or decode surfaces later through OnGoRadioError.
 */
native GoRadio_QueueTrack(stationid, const location[], const title[] = "", const artist[] = "",
	const coverArt[] = "", mode = RADIO_QUEUE_APPEND);

/* Removes one still-pending item. Reports success = false (not an error)
 * if the id wasn't in the queue -- because it already played, or was
 * already removed. You cannot dequeue what is currently playing; it has
 * left the queue by then. Use GoRadio_Skip for that. */
native GoRadio_Dequeue(stationid, const queueId[]);

/* Removes every pending item. With stopCurrent = 1 the current item is
 * cut off too, which drops the station to silence -- the usual "clean
 * slate" call. The result value is how many pending items were removed. */
native GoRadio_ClearQueue(stationid, bool:stopCurrent = false);

/* Cuts the current item short and moves straight to the next pending one
 * (or silence). success = false means nothing was playing. This is the
 * only way to end a live stream, which otherwise never finishes. */
native GoRadio_Skip(stationid);

/* Jumps straight to a pending item: everything ahead of it is dropped
 * and the current item is cut off so it starts now. Identify it by queue
 * id rather than position -- your idea of positions can be stale by the
 * time the call lands. The result value is how many items were dropped. */
native GoRadio_SkipTo(stationid, const queueId[]);

/* Holds the current item's position and drops to silence until
 * GoRadio_Resume. success = false if nothing was playing, it was already
 * paused, or it's a live stream (no fixed position to hold). */
native GoRadio_Pause(stationid);
native GoRadio_Resume(stationid);

/* Absolute and relative seeking within the current item, clamped to the
 * track. Both work while paused -- that just moves where Resume picks
 * up. The result value is the resulting position in seconds.
 * success = false if nothing seekable is playing. */
native GoRadio_Seek(stationid, positionSeconds);
native GoRadio_SeekBy(stationid, deltaSeconds);


/* =====================================================================
 * Station state
 *
 * These read a cached snapshot, refreshed automatically (every
 * goradio_poll_interval seconds, and immediately whenever a track
 * starts or ends), so they cost nothing and never block.
 * ===================================================================== */

/* Asks for a fresh snapshot now. OnGoRadioStatusUpdate fires when it
 * lands. You rarely need this -- the plugin already refreshes on the
 * events that matter. */
native GoRadio_RefreshStatus(stationid);

/* 1 once at least one snapshot has arrived. Before that, the getters
 * below all read as empty/zero. */
native GoRadio_HasStatus(stationid);

native GoRadio_GetListenerCount(stationid);
native GoRadio_GetUptime(stationid);

/* Playing the silence loop -- because the queue is empty, or because
 * playback is paused. */
native GoRadio_IsSilence(stationid);
native GoRadio_IsPaused(stationid);
/* 1 when there is a current track (paused counts as playing). */
native GoRadio_IsPlaying(stationid);

/* Pending items, from the most recent event or snapshot -- whichever was
 * later, so this stays current between polls. */
native GoRadio_GetQueueLength(stationid);

/* The current track. Each returns 0, and writes an empty string, when
 * nothing is playing. */
native GoRadio_GetCurrentTrackID(stationid, dest[], len = sizeof dest);
native GoRadio_GetCurrentTrackTitle(stationid, dest[], len = sizeof dest);
native GoRadio_GetCurrentTrackArtist(stationid, dest[], len = sizeof dest);
native GoRadio_GetCurrentTrackLocation(stationid, dest[], len = sizeof dest);
native GoRadio_GetCurrentTrackCoverArt(stationid, dest[], len = sizeof dest);

/* 0 means unknown or indefinite -- always the case for a live stream. */
native GoRadio_GetCurrentTrackDuration(stationid);
/* Seconds into the current track. Carried forward between snapshots, so
 * it advances smoothly enough to drive a progress bar. */
native GoRadio_GetCurrentTrackElapsed(stationid);

/*
 * The pending queue as of the last snapshot -- so this can lag
 * GoRadio_GetQueueLength by up to one poll. Index 0 is next up.
 */
native GoRadio_GetQueueItemCount(stationid);
native GoRadio_GetQueueItemID(stationid, index, dest[], len = sizeof dest);
native GoRadio_GetQueueItemTitle(stationid, index, dest[], len = sizeof dest);
native GoRadio_GetQueueItemArtist(stationid, index, dest[], len = sizeof dest);
native GoRadio_GetQueueItemLocation(stationid, index, dest[], len = sizeof dest);
native GoRadio_GetQueueItemDuration(stationid, index);

/*
 * Recently finished items, oldest first, capped by the audio server at a
 * small fixed number. Meant to seed a "recently played" display on
 * startup; keep it current from OnGoRadioTrackEnded afterwards.
 * Reason is "completed" or "interrupted".
 */
native GoRadio_GetHistoryCount(stationid);
native GoRadio_GetHistoryTitle(stationid, index, dest[], len = sizeof dest);
native GoRadio_GetHistoryArtist(stationid, index, dest[], len = sizeof dest);
native GoRadio_GetHistoryReason(stationid, index, dest[], len = sizeof dest);


/* =====================================================================
 * Every station on the audio server
 *
 * Not just this server's -- every station the token authorizes,
 * including ones other game servers or controllers registered.
 * ===================================================================== */

/* Asks for the list; OnGoRadioStationList fires when it arrives, after
 * which the getters below read from it. */
native GoRadio_RequestStationList();
native GoRadio_GetListedStationCount();
native GoRadio_GetListedStationSlug(index, dest[], len = sizeof dest);
native GoRadio_GetListedStationName(index, dest[], len = sizeof dest);
native GoRadio_GetListedStationLogo(index, dest[], len = sizeof dest);
native GoRadio_GetListedStationListeners(index);


/* =====================================================================
 * Callbacks
 *
 * All of these are offered to the gamemode and to every filterscript, so
 * a filterscript can react to a station the gamemode created. Return
 * values are ignored.
 * ===================================================================== */

/* The audio server answered for the first time. Also your cue that the
 * URL and token are good. */
forward OnGoRadioServerInfo(const version[]);

/*
 * The station is registered and ready for commands. Fires on the first
 * registration and again after every reconnect, so it is the right place
 * to queue a station's opening track.
 *
 * reRegistered is 0 when the audio server had never heard of this slug --
 * which after a reconnect means it restarted and lost the station's
 * whole queue. afterReconnect distinguishes a reconnect from the initial
 * registration at startup.
 *
 * The safe rule: if GoRadio_GetQueueLength(stationid) is 0 here, queue
 * something. Nothing else will notice an empty queue -- OnGoRadioQueueLow
 * only fires on the way *into* "low" from "not low", which a freshly
 * recreated station never does.
 */
forward OnGoRadioStationRegistered(stationid, const streamUrl[], bool:reRegistered,
	bool:afterReconnect);

/* A queued item started playing. durationSeconds is 0 for a live stream
 * or an item whose length isn't known yet. */
forward OnGoRadioTrackStarted(stationid, const queueId[], const location[], const title[],
	const artist[], const coverArt[], durationSeconds);

/* reason is "completed" or "interrupted". */
forward OnGoRadioTrackEnded(stationid, const queueId[], const reason[]);

/* The answer to GoRadio_QueueTrack. queuePosition is 0 when the item is
 * about to play immediately. */
forward OnGoRadioTrackQueued(stationid, requestid, const queueId[], queuePosition);

/* The pending queue dropped to or below the station's
 * lowQueueThreshold. Edge-triggered: it fires once, and not again until
 * the queue climbs back above the threshold and dips again. */
forward OnGoRadioQueueLow(stationid, queueLength, threshold);

forward OnGoRadioQueueUpdated(stationid, queueLength);
forward OnGoRadioListenerCountChanged(stationid, listenerCount);

/* The station fell back to (or came off) the silence loop. */
forward OnGoRadioSilenceStarted(stationid);
forward OnGoRadioSilenceEnded(stationid);

/* A fresh status snapshot landed; the GoRadio_Get* natives now reflect
 * it. */
forward OnGoRadioStatusUpdate(stationid);

/*
 * The answer to a playback command. command is the RPC name ("Skip",
 * "Pause", "ClearQueue", ...) and result carries whatever number that
 * command reports -- items removed, the resulting seek position, and so
 * on. GoRadio_QueueTrack answers through OnGoRadioTrackQueued instead.
 *
 * success = false is not necessarily a failure: several commands report
 * "there was nothing to do" that way (nothing was playing to skip, the
 * queue id wasn't there any more). A genuine error also fires
 * OnGoRadioError.
 */
forward OnGoRadioCommandResult(stationid, requestid, const command[], bool:success, result);

/* GoRadio_RequestStationList finished; count entries are readable. */
forward OnGoRadioStationList(count);

/*
 * Something went wrong. code is the Connect error code
 * ("unauthenticated", "permission_denied", "not_found", "unavailable",
 * ...) or the audio server's own code for a playback failure, such as a
 * track that could not be fetched or decoded.
 *
 * stationid is INVALID_RADIO_STATION for errors that aren't tied to one
 * station. "unauthenticated" and "permission_denied" are never retried --
 * they mean the token is wrong or doesn't cover the slug, and the
 * station stays down until that's fixed.
 */
forward OnGoRadioError(stationid, const code[], const message[]);
