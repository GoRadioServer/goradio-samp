#!/usr/bin/env python3
"""A stand-in for `radio serve`, just complete enough to exercise the
plugin's Connect client end to end.

It deliberately answers the way protobuf-JSON actually does, since those
are the details a hand-written client gets wrong: int64 fields as quoted
strings, lowerCamelCase names, default-valued fields omitted entirely, and
a SubscribeEvents body that is chunked HTTP/1.1 carrying 5-byte Connect
envelopes.
"""

import json
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PREFIX = "/audioserver.v1.AudioServerService/"
TOKEN = "test-token"

state = {"registered": {}, "queued": [], "skips": 0}


def envelope(flags, payload):
    body = payload.encode()
    return struct.pack(">BI", flags, len(body)) + body


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("[fake] " + (fmt % args) + "\n")

    def _read_body(self):
        length = int(self.headers.get("Content-Length", "0"))
        return self.rfile.read(length) if length else b""

    def _json(self, status, obj):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _error(self, status, code, message):
        self._json(status, {"code": code, "message": message})

    def do_POST(self):
        if not self.path.startswith(PREFIX):
            self.send_error(404)
            return
        rpc = self.path[len(PREFIX):]
        raw = self._read_body()

        if self.headers.get("Authorization") != "Bearer " + TOKEN:
            self._error(401, "unauthenticated", "bad token")
            return

        if rpc == "SubscribeEvents":
            self._subscribe(raw)
            return

        try:
            req = json.loads(raw.decode() or "{}")
        except ValueError:
            self._error(400, "invalid_argument", "malformed JSON body")
            return

        handler = getattr(self, "_rpc_" + rpc, None)
        if handler is None:
            self.send_error(404, "not found")
            return
        handler(req)

    # --- unary RPCs -----------------------------------------------------

    def _rpc_GetServerInfo(self, req):
        self._json(200, {"version": "v9.9.9-fake"})

    def _rpc_RegisterStation(self, req):
        slug = req.get("slug", "")
        if not slug:
            self._error(400, "invalid_argument", "slug is required")
            return
        already = slug in state["registered"]
        state["registered"][slug] = req
        resp = {"slug": slug, "streamUrl": "http://fake/stream/" + slug}
        # proto3 omits false; only send reRegistered when it is true.
        if already:
            resp["reRegistered"] = True
        self._json(200, resp)

    def _rpc_UnregisterStation(self, req):
        state["registered"].pop(req.get("slug", ""), None)
        self._json(200, {})

    def _rpc_QueueTrack(self, req):
        state["queued"].append(req)
        self._json(200, {
            "queueId": "queue-%d" % len(state["queued"]),
            "queuePosition": len(state["queued"]) - 1,
            "status": "queued",
        })

    def _rpc_Skip(self, req):
        state["skips"] += 1
        self._json(200, {"skipped": True})

    def _rpc_ClearQueue(self, req):
        n = len(state["queued"])
        state["queued"] = []
        self._json(200, {"removedCount": n, "stoppedCurrent": bool(req.get("stopCurrent"))})

    def _rpc_Pause(self, req):
        # The "nothing to do" answer: a false flag, not an error.
        self._json(200, {})

    def _rpc_ListStations(self, req):
        self._json(200, {"stations": [
            {"slug": "testfm", "name": "Test FM", "listenerCount": "42"},
            {"slug": "otherfm", "name": "Other FM"},
        ]})

    def _rpc_GetStatus(self, req):
        slug = req.get("slug", "")
        if slug not in state["registered"]:
            self._error(404, "not_found", "no such station")
            return
        self._json(200, {
            "slug": slug,
            "name": "Test FM",
            "isRegistered": True,
            "currentTrack": {
                "queueId": "queue-1",
                "source": {
                    "type": "TRACK_SOURCE_TYPE_LOCAL_FILE",
                    "location": "songs/intro.mp3",
                    "displayTitle": "Intro — \"quoted\" & escaped",
                    "displayArtist": "The Testers",
                },
                "durationSeconds": "212",
            },
            "queue": [
                {"queueId": "queue-2",
                 "source": {"location": "songs/second.mp3", "displayTitle": "Second"},
                 "durationSeconds": "180"},
                {"queueId": "queue-3",
                 "source": {"location": "songs/third.mp3", "displayTitle": "Third"}},
            ],
            "history": [
                {"queueId": "queue-0",
                 "source": {"location": "songs/old.mp3", "displayTitle": "Old One"},
                 "reason": "completed", "endedAtUnixMs": "1787586747047"},
            ],
            "listenerCount": "7",
            "uptimeSeconds": "412",
            "currentTrackElapsedSeconds": "30",
        })

    # --- streaming ------------------------------------------------------

    def _subscribe(self, raw):
        # The request message is itself enveloped.
        if len(raw) < 5:
            self._error(400, "invalid_argument", "short envelope")
            return
        _flags, length = struct.unpack(">BI", raw[:5])
        req = json.loads(raw[5:5 + length].decode())
        slug = req.get("slug", "")
        if slug not in state["registered"]:
            self._error(404, "not_found", "no such station")
            return

        self.send_response(200)
        self.send_header("Content-Type", "application/connect+json")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()

        def emit(data):
            self.wfile.write(b"%x\r\n" % len(data) + data + b"\r\n")
            self.wfile.flush()

        events = [
            {"slug": slug, "type": "EVENT_TYPE_TRACK_STARTED",
             "timestampUnixMs": "1787586747047",
             "trackStarted": {"queueId": "queue-1",
                              "source": {"location": "songs/intro.mp3",
                                         "displayTitle": "Intro — live",
                                         "displayArtist": "The Testers"},
                              "durationSeconds": "212"}},
            {"slug": slug, "type": "EVENT_TYPE_LISTENER_COUNT_CHANGED",
             "listenerCountChanged": {"listenerCount": "13"}},
            {"slug": slug, "type": "EVENT_TYPE_QUEUE_LOW",
             "queueLow": {"queueLength": 1, "threshold": 3}},
            {"slug": slug, "type": "EVENT_TYPE_TRACK_ENDED",
             "trackEnded": {"queueId": "queue-1", "reason": "completed"}},
        ]
        try:
            for ev in events:
                emit(envelope(0, json.dumps(ev)))
                time.sleep(0.05)
            # Two events in one TCP write, to prove the framing survives
            # arriving glued together.
            glued = envelope(0, json.dumps(
                {"slug": slug, "type": "EVENT_TYPE_SILENCE_STARTED"})) + envelope(0, json.dumps(
                {"slug": slug, "type": "EVENT_TYPE_QUEUE_UPDATED",
                 "queueUpdated": {"queueLength": 4}}))
            emit(glued)
            time.sleep(1.0)
            emit(envelope(2, "{}"))  # clean end of stream
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    print("http://127.0.0.1:%d" % server.server_address[1], flush=True)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
