# CLAUDE.md

Guidance for Claude Code (or any future contributor) working in this repo.

## Project shape

A SA-MP server plugin that manages GoRadio stations from PAWN. It talks
to the audio server (`radio serve`, in `../gta-radio-golang`) over the
**Connect protocol** — plain HTTP/1.1 with JSON bodies — never gRPC.
That choice is structural, not incidental: SA-MP is a 32-bit process with
a C plugin ABI, and a gRPC stack is not a dependency it can carry. Don't
"upgrade" the transport.

Everything is dependency-free and hand-written:

| Layer | Files | Does |
|---|---|---|
| SA-MP ABI | `sdk/`, `src/plugin.cpp`, `src/amxutil.*` | Entry points, native registration, calling PAWN publics |
| PAWN API | `src/natives.cpp`, `include/goradio.inc` | Every `GoRadio_*` native and `OnGoRadio*` callback |
| Orchestration | `src/manager.*`, `src/station.*` | Stations, worker threads, event streams, reconnect, the status cache |
| Protocol | `src/connect_client.*` | Unary RPCs and the 5-byte Connect envelope framing |
| Transport | `src/http.*`, `src/json.*` | HTTP/1.1 (chunked, keep-alive, optional TLS) and a protobuf-JSON-shaped parser |
| Docs | `docs/` | MkDocs + Material, published to GitHub Pages |
| CI/release | `.github/workflows/`, `scripts/package.sh`, `Dockerfile` | Build, test, package, publish |

## Threading — the thing to be careful about

A SA-MP public may only be called from the server's main thread. So:

- Natives run on the main thread and **must never block on the network**.
  Every one either touches local state or queues a job.
- Worker threads and per-station stream threads do all the I/O. They
  never call into PAWN. They produce `PawnEvent` values — plain strings
  and numbers, nothing pointing back at mutable state — and queue them.
- `ProcessTick` drains that queue and fires the callbacks.

`Manager::state_mutex_` guards stations and caches; never hold it across
I/O. Lock order is state → jobs → dispatch, and nothing takes
`state_mutex_` while holding either of the others. If you add a code path,
check it against that.

After any change here, re-run the tests under both sanitizers — they are
how the SIGPIPE bug in `HttpConnection::Post`'s keep-alive retry was
found, and that one would have killed the game server:

```sh
make test
g++ -std=c++11 -g -fsanitize=thread   -Isdk -Isrc test/run_tests.cpp src/{json,http,log,connect_client,station,manager}.cpp -o /tmp/t -lpthread
g++ -std=c++11 -g -fsanitize=address,undefined -Isdk -Isrc test/run_tests.cpp src/{json,http,log,connect_client,station,manager}.cpp -o /tmp/a -lpthread
```

(Start `test/fake_audioserver.py` and pass it the URL it prints.)

## Adding a native

A native is not done until all four of these are true, or the API is
inconsistent in a way scripters hit before you do:

1. **Implement it** in `src/natives.cpp` and add it to the `kNatives`
   table. Validate the argument count with `CheckArgs` — PAWN passes
   `params[0]` as a *byte* count, not an argument count.
2. **Declare it** in `include/goradio.inc`, with the same name and
   argument order. There is a check for this:
   ```sh
   diff <(grep -oE '^native (GoRadio_[A-Za-z_]+)' include/goradio.inc | awk '{print $2}' | sort) \
        <(grep -oE '\{"(GoRadio_[A-Za-z_]+)"' src/natives.cpp | tr -d '{"' | sort)
   ```
3. **Document it** in the README's native table.
4. **Cover it** in `test/run_tests.cpp` if it has any logic beyond
   reading a cached field, teaching `test/fake_audioserver.py` the RPC if
   it needs a new one.

Adding a callback is the same, plus the `AmxCallback` dispatch in
`DispatchPendingEvents` — and the same `diff` trick works on
`^forward (OnGoRadio...)` versus `AmxCallback("...")`.

Step 3 means **two** places now: the README's native table, and the
matching page under `docs/content/pawn-api/`. The docs are the primary
reference; the README is the summary. A native documented in neither is
one nobody will find.

## Documentation

`docs/` is MkDocs + Material, `docs_dir: content`, deployed to GitHub
Pages by `.github/workflows/docs.yml` on every push to `main`.

```sh
pip install -r docs/requirements.txt
cd docs && mkdocs serve
mkdocs build --strict     # what CI runs
```

`--strict` plus the `validation:` block in `mkdocs.yml` turns broken
internal links, missing nav targets and **bad anchors** into build
failures. Anchors are the ones that break silently, and a heading like
`## \`GoRadio_Pause\` / \`GoRadio_Resume\`` slugifies to
`goradio_pause-goradio_resume`, not to either name alone — check the
anchor rather than assuming it.

Every new page needs a `nav:` entry; a file that isn't in the nav is a
`--strict` failure too.

Where things belong: **how it works** in `how-it-works/`, **per-native
reference** in `pawn-api/`, **task-shaped writing** in `guides/`. Resist
explaining the same behaviour in all three — link instead. The
re-priming-after-reconnect rule is deliberately the exception: it appears
in the quickstart, the lifecycle page, the callback reference and
troubleshooting, because it is the one mistake that silently kills a
station.

## Releasing

Tag `vX.Y.Z` and push it; `.github/workflows/release.yml` builds Linux and
Windows at both widths, packages them with `scripts/package.sh`, and
publishes a GitHub release with checksums.

The version is baked in via `-DGORADIO_VERSION` (see the Makefile and
`CMakeLists.txt`) and reported in the server log — `dev` for a local build
with nothing passed in. Bump minor for new natives or callbacks, patch for
fixes, major for anything that changes an existing native's signature or
behaviour, since gamemodes compile against `goradio.inc`.

`Dockerfile` is how the 32-bit build gets done without a multilib
toolchain on the host (`make docker`), and `--target test` runs the suite
compiled 32-bit — worth doing before a release, since 32-bit is what
ships.

## Mirroring the audio server

The Lua controller in `../gta-radio-golang/internal/luastation/` and its
type stubs in `lua-types/radio.lua` are the reference for what a station
controller should expose; that repo's `CLAUDE.md` lists what has to change
when an RPC does. When a new RPC or field lands there, this plugin is a
downstream consumer of the same protocol — check
`docs/content/developer-api/protocol-reference.md` for the wire shape
rather than guessing from the Go types.

Four protobuf-JSON details are load-bearing in `src/json.cpp`, and all
four have burned hand-written clients before:

- **int64 fields are quoted strings** on the wire; int32 fields are bare
  numbers. `JsonValue::Int()` accepts either — keep it that way.
- **Field names are lowerCamelCase**, not the `.proto`'s snake_case.
- **Fields at their default value are omitted entirely.** Every accessor
  takes a default; "absent" is never an error.
- **Enums are full strings** (`"QUEUE_MODE_APPEND"`), not numbers and not
  short names.

## Registration lifecycle

Station registration is in-memory on the audio server, so a restart wipes
it *and the queue*. `Manager::StreamLoop` re-registers on every dropped
stream and fires `OnGoRadioStationRegistered` again with
`afterReconnect = true`, specifically so a script can re-prime a queue
that a restart emptied. If you touch that loop, keep the callback firing
on reconnects — without it a station goes permanently quiet after an
audio-server restart while still looking connected, and nothing else in
the system notices.

Failures that retrying cannot fix (`unauthenticated`, `permission_denied`,
`invalid_argument`) must stay non-retryable. See `RpcError::permanent()`.

## The vendored SDK

`sdk/amx.h` and `sdk/plugincommon.h` are hand-written subsets of the SA-MP
plugin SDK, so there is nothing to fetch. Two deliberate choices there:

- `AMX` is **opaque**. Everything goes through the `amx_*` entry points
  the server passes in, which is what keeps this immune to the AMX
  struct-layout differences between Pawn versions.
- The `PLUGIN_AMX_EXPORT_*` values are the server's ABI, not ours. They
  are alphabetical by function name — a useful check if one ever looks
  wrong.

`sdk/goradio.ver` restricts the `.so` to six exported symbols. Keep it:
without it, `-static-libstdc++` leaves thousands of C++ runtime symbols
visible, and SA-MP loads several plugins into one process.
