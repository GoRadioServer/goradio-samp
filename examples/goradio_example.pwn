/*
 * A worked example: two stations, a rotating playlist, and player
 * commands. Drop it in filterscripts/, add "goradio_example" to your
 * server.cfg filterscripts line, and make sure the plugin is configured:
 *
 *   plugins goradio
 *   goradio_url http://127.0.0.1:9090
 *   goradio_token eyJhbGciOi...
 *
 * The token must be authorized for both slugs used below.
 */

#include <a_samp>
#include <goradio>

#define STATION_MAIN_SLUG   "myfm"
#define STATION_CHILL_SLUG  "chillfm"

/* How many items we try to keep queued ahead of the current track. */
#define QUEUE_TARGET        3

static gMainStation  = INVALID_RADIO_STATION;
static gChillStation = INVALID_RADIO_STATION;

/* Locations are relative to the audio server's audio_root -- except the
 * last one, which is a live relay: an http(s) URL is detected as a URL,
 * and a continuous stream plays until something cuts it off. */
static const gPlaylist[][] = {
	"music/intro-jingle.mp3",
	"music/night-drive.mp3",
	"music/san-fierro-nights.mp3",
	"music/back-to-the-grove.mp3"
};

static const gPlaylistTitles[][] = {
	"Station Ident",
	"Night Drive",
	"San Fierro Nights",
	"Back to the Grove"
};

static gPlaylistCursor = 0;


public OnFilterScriptInit()
{
	if (!GoRadio_IsReady())
	{
		print("[example] goradio has no audio server configured -- check server.cfg");
		return 1;
	}

	/* Metadata is freeform and opaque to the audio server; it is sent
	 * with the registration, so set it before creating the station. */
	gMainStation = GoRadio_CreateStation(STATION_MAIN_SLUG, "My FM", "The main channel",
		QUEUE_TARGET, "https://cdn.example.com/art/main.png");
	if (gMainStation != INVALID_RADIO_STATION)
	{
		GoRadio_SetStationMetadata(gMainStation, "group", "myserver");
		GoRadio_SetStationMetadata(gMainStation, "genre", "mixed");
	}

	gChillStation = GoRadio_CreateStation(STATION_CHILL_SLUG, "Chill FM", "Slower stuff", 2);

	print("[example] stations created; waiting for registration");
	return 1;
}

public OnFilterScriptExit()
{
	/* Destroying a station unregisters it and disconnects its listeners.
	 * Leave them running instead if you want playback to survive a
	 * filterscript reload. */
	if (gMainStation != INVALID_RADIO_STATION) GoRadio_DestroyStation(gMainStation);
	if (gChillStation != INVALID_RADIO_STATION) GoRadio_DestroyStation(gChillStation);
	return 1;
}


/* ---------------------------------------------------------------------
 * Keeping the queue fed
 * ------------------------------------------------------------------ */

QueueNextTrack(stationid)
{
	new index = gPlaylistCursor % sizeof(gPlaylist);
	gPlaylistCursor++;

	GoRadio_QueueTrack(stationid, gPlaylist[index], gPlaylistTitles[index], "My FM");
}

public OnGoRadioStationRegistered(stationid, const streamUrl[], bool:reRegistered,
	bool:afterReconnect)
{
	new slug[64];
	GoRadio_GetStationSlug(stationid, slug);
	printf("[example] station %s registered -- listen at %s", slug, streamUrl);

	/*
	 * This fires again after every reconnect, and that is the point: if
	 * the audio server restarted while we were disconnected, the station
	 * it recreated for us has an empty queue. Nothing else would notice --
	 * OnGoRadioQueueLow only fires on the way *into* "low", which a queue
	 * that has been empty since it was created never does.
	 */
	if (GoRadio_GetQueueLength(stationid) == 0)
	{
		if (afterReconnect && !reRegistered)
			printf("[example] the audio server restarted; re-priming %s", slug);

		for (new i = 0; i < QUEUE_TARGET; i++)
			QueueNextTrack(stationid);
	}
	return 1;
}

public OnGoRadioQueueLow(stationid, queueLength, threshold)
{
	/* Edge-triggered, so top the queue back up rather than adding one. */
	for (new i = queueLength; i < threshold + 1; i++)
		QueueNextTrack(stationid);
	return 1;
}


/* ---------------------------------------------------------------------
 * Reacting to playback
 * ------------------------------------------------------------------ */

public OnGoRadioTrackStarted(stationid, const queueId[], const location[], const title[],
	const artist[], const coverArt[], durationSeconds)
{
	if (stationid != gMainStation) return 1;

	new msg[144];
	format(msg, sizeof(msg), "{88CCFF}[Radio]{FFFFFF} Now playing: %s - %s", artist, title);
	SendClientMessageToAll(-1, msg);
	return 1;
}

public OnGoRadioError(stationid, const code[], const message[])
{
	/* A bad token or a slug the token doesn't cover is never retried --
	 * the station stays down until server.cfg is fixed, so this wants to
	 * be loud rather than a debug line. */
	printf("[example] radio error on station %d: %s: %s", stationid, code, message);
	return 1;
}


/* ---------------------------------------------------------------------
 * Player commands
 * ------------------------------------------------------------------ */

public OnPlayerCommandText(playerid, cmdtext[])
{
	if (!strcmp(cmdtext, "/radio", true))
	{
		new url[160], msg[200];
		if (!GoRadio_GetStreamURL(gMainStation, url))
		{
			SendClientMessage(playerid, -1, "The radio isn't connected yet -- try again shortly.");
			return 1;
		}
		format(msg, sizeof(msg), "Tune in: %s", url);
		SendClientMessage(playerid, -1, msg);

		/* Or play it in-game -- the stream URL works with
		 * PlayAudioStreamForPlayer directly. */
		PlayAudioStreamForPlayer(playerid, url);
		return 1;
	}

	if (!strcmp(cmdtext, "/nowplaying", true))
	{
		new title[128], artist[128], msg[300];
		if (!GoRadio_GetCurrentTrackTitle(gMainStation, title))
		{
			SendClientMessage(playerid, -1, "Nothing is playing right now.");
			return 1;
		}
		GoRadio_GetCurrentTrackArtist(gMainStation, artist);

		new elapsed  = GoRadio_GetCurrentTrackElapsed(gMainStation);
		new duration = GoRadio_GetCurrentTrackDuration(gMainStation);

		if (duration > 0)
			format(msg, sizeof(msg), "%s - %s [%d:%02d / %d:%02d] (%d listening)",
				artist, title, elapsed / 60, elapsed % 60, duration / 60, duration % 60,
				GoRadio_GetListenerCount(gMainStation));
		else
			/* A live relay has no length, so there is no progress to show. */
			format(msg, sizeof(msg), "%s - %s [live] (%d listening)",
				artist, title, GoRadio_GetListenerCount(gMainStation));

		SendClientMessage(playerid, -1, msg);
		return 1;
	}

	if (!strcmp(cmdtext, "/skip", true))
	{
		if (!IsPlayerAdmin(playerid)) return 0;
		GoRadio_Skip(gMainStation);
		SendClientMessage(playerid, -1, "Skipping...");
		return 1;
	}

	if (!strcmp(cmdtext, "/queue", true))
	{
		new count = GoRadio_GetQueueItemCount(gMainStation), title[128], msg[160];
		if (count == 0)
		{
			SendClientMessage(playerid, -1, "The queue is empty.");
			return 1;
		}
		for (new i = 0; i < count; i++)
		{
			GoRadio_GetQueueItemTitle(gMainStation, i, title);
			format(msg, sizeof(msg), "%d. %s", i + 1, title);
			SendClientMessage(playerid, -1, msg);
		}
		return 1;
	}

	return 0;
}

public OnGoRadioCommandResult(stationid, requestid, const command[], bool:success, result)
{
	/* success = false is often just "there was nothing to do" -- nothing
	 * was playing to skip, the queue id had already gone. */
	if (!success)
		printf("[example] %s on station %d did nothing", command, stationid);
	return 1;
}
