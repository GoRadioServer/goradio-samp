# Running the Tests

```sh
make test
```

That's it — no SA-MP server, no audio server, no network. It builds
everything below the AMX layer and runs it against a fake audio server.

```
building tests...
starting the fake audio server...
  ok   plain http URL parses
  ...
  ok   re-registered after the stream ended
  ok   Shutdown returned

90 checks, 0 failures
```

## What is actually tested

The suite drives the real `Manager` — the same code the plugin runs —
against `test/fake_audioserver.py`, a stand-in for `radio serve`. So the
HTTP client, the JSON layer, the Connect envelope framing, the worker
pool, the event stream and the reconnect logic are all exercised for real.
Only the AMX glue is out of scope, since that needs a Pawn VM.

The fake server deliberately answers the way protobuf-JSON **actually**
does, because those details are what break hand-written clients:

- `int64` fields as quoted strings, `int32` as bare numbers
- `lowerCamelCase` field names
- fields at their default value omitted entirely — a station with no
  listeners has no `listenerCount` key at all
- enums spelled out in full
- a chunked HTTP/1.1 `SubscribeEvents` body carrying 5-byte Connect
  envelopes

It also sends **two events glued into a single TCP write**, so the
envelope reader can't get away with assuming one frame per read.

Covered end to end: URL parsing, JSON parsing and escaping, registration,
duplicate-slug rejection, reconfiguration, the event stream, the status
cache, every command shape, `ListStations`, and the
reconnect-and-re-register path.

## Testing at 32 bits

The plugin ships 32-bit, and that's where a `size_t` or pointer assumption
would surface. Run the suite the same way:

```sh
BITS=32 sh test/run_tests.sh
```

Or in the container, which already has the toolchain:

```sh
docker build --target test --build-arg BITS=32 .
```

CI runs both widths on every push.

## Sanitizers

Worth running after **any** change to the threading in
`src/manager.cpp` — this is how the SIGPIPE bug in the keep-alive retry
path was found, and that one would have killed the game server.

```sh
# ThreadSanitizer
g++ -std=c++11 -g -fsanitize=thread -Isdk -Isrc \
  test/run_tests.cpp src/json.cpp src/http.cpp src/log.cpp \
  src/connect_client.cpp src/station.cpp src/manager.cpp \
  -o /tmp/run_tests -lpthread

# AddressSanitizer + UBSan + LeakSanitizer
g++ -std=c++11 -g -fsanitize=address,undefined -Isdk -Isrc \
  test/run_tests.cpp src/json.cpp src/http.cpp src/log.cpp \
  src/connect_client.cpp src/station.cpp src/manager.cpp \
  -o /tmp/run_tests -lpthread
```

Then start the fake server and pass it the URL it prints:

```sh
python3 test/fake_audioserver.py > /tmp/url.txt &
sleep 1
/tmp/run_tests "$(cat /tmp/url.txt)"
```

Both are clean, and CI runs them on every push. A sanitizer finding is a
release blocker, not a warning: this code runs inside someone's game
server.

## Adding a test

Tests live in `test/run_tests.cpp` and use three helpers — `Check`,
`CheckEq` and `CheckEqInt` — which record a failure and carry on rather
than aborting, so one break doesn't hide the rest.

For anything driven by the manager, `WaitFor(kind, timeout_ms, &ev)` pumps
`Manager::Tick` until an event of that kind shows up, or the timeout
expires:

```cpp
int req = mgr.Skip(station);
Check(WaitFor(kEvCommandResult, 5000, &ev), "Skip answered");
CheckEq(ev.text, "Skip", "command name");
Check(ev.flag, "Skip reports success");
```

If the behaviour needs an RPC the fake server doesn't implement yet, add a
`_rpc_<RpcName>` method to `test/fake_audioserver.py` — the handler is
found by name.

Two conventions worth keeping:

- **Answer the way the real server does.** Omit default-valued fields,
  quote your `int64`s. A fake that is tidier than reality tests the wrong
  thing.
- **Assert on observable behaviour**, not internals — the `PawnEvent`s a
  script would see, and what the `GoRadio_Get*` natives would read.

## Checking the PAWN API surface

```sh
make check-names
```

Two mistakes on this boundary compile perfectly and then fail at runtime,
so they get a dedicated check:

- **A native declared in `goradio.inc` but not registered in
  `natives.cpp`** (or the reverse). Scripts compile; calling it throws.
- **A name longer than 31 characters.** PAWN's symbol limit is 31, and the
  compiler silently *truncates* past it — so the name baked into the
  `.amx` no longer matches the one the plugin registers, `amx_Register`
  never binds it, and the native is simply missing. This is not
  hypothetical: `GoRadio_GetListedStationListeners` was 33 characters and
  had to be renamed to `GoRadio_GetListedListenerCount`.

## Compiling the PAWN include

`make check-names` catches the name problems, but only a compiler catches
a malformed declaration or a default argument the Pawn parser rejects. CI
compiles `examples/goradio_example.pwn` on every push.

Locally you need three things, none of which ship here — the compiler, and
**two** separate standard libraries (`pawn-stdlib` has `core.inc` and
`float.inc`; `samp-stdlib` has `a_samp.inc`; the compiler tarball has
neither):

```sh
mkdir -p /tmp/pawn && cd /tmp/pawn

curl -fsSL -O https://github.com/pawn-lang/compiler/releases/download/v3.10.10/pawnc-3.10.10-linux.tar.gz
tar xzf pawnc-3.10.10-linux.tar.gz

git clone --depth 1 https://github.com/pawn-lang/pawn-stdlib
git clone --depth 1 https://github.com/pawn-lang/samp-stdlib
```

Then, from the repository root:

```sh
export LD_LIBRARY_PATH=/tmp/pawn/pawnc-3.10.10-linux/lib

/tmp/pawn/pawnc-3.10.10-linux/bin/pawncc examples/goradio_example.pwn \
    -i/tmp/pawn/pawn-stdlib -i/tmp/pawn/samp-stdlib -iinclude \
    -o/tmp/example.amx -Z+ -d3
```

`-Z+` is compatibility mode, which is what Pawno uses; the SA-MP standard
library doesn't compile cleanly without it. `LD_LIBRARY_PATH` matters
because `pawncc` loads `libpawnc.so` from the tarball's `lib/`.

Treat **warning 200** as an error if you see it — that's the symbol
truncation described above.

## Checking the Windows build without Windows

```sh
make check-windows
```

Compiles every source against real Windows headers, at both widths, using
MinGW in a container. It is not MSVC, but it catches the class of error
that broke a release build: Winsock's `setsockopt` takes `const char *`
where POSIX takes `const void *`, and only a compiler reliably notices a
missing cast.

Two details make it worth running rather than decorative, and both were
learned the hard way:

- It uses the **`-posix` compiler variants**. MinGW's default win32
  threading model has no `<mutex>` or `<thread>` at all, so the default
  compilers fail on this code for reasons that say nothing about Windows.
- It **force-defines `TCP_KEEPIDLE`** and friends to their Windows SDK
  values. MinGW's headers don't declare them, so the code they guard
  would be preprocessed away and never checked — which is exactly how the
  original bug slipped through a first pass of this check.

The MSVC build in CI remains the authority; this is the fast local
pre-flight.

## Checking the Windows export table

```sh
make check-exports
```

A Windows DLL that exports decorated names — `_Supports@0` rather than
`Supports` — loads as a bare `Failed.` in the server log with nothing
else said, so `scripts/check-dll-exports.ps1` verifies the six entry
points on every Windows build and before every release.

This target tests **the checker itself**, against captured `dumpbin`
output, so it runs anywhere PowerShell does — no Windows, no Visual
Studio, no DLL. It falls back to Microsoft's PowerShell container if the
host has no `pwsh`.

It exists because the checker shipped broken: `dumpbin` returns an
*array* of lines, and PowerShell's `-match`/`-notmatch` on a collection
**filters the collection** rather than returning a boolean. So
`$lines -notmatch "Supports"` yielded every line lacking the word — a
non-empty, and therefore truthy, array — and the check reported a missing
export against a perfectly good DLL. The tests pin that behaviour down,
including the case where the raw array is passed in: it now fails loudly
at the parameter instead of misreporting.

## Testing your own script## Checking the Windows export table

```sh
make check-exports
```

A Windows DLL that exports decorated names — `_Supports@0` rather than
`Supports` — loads as a bare `Failed.` in the server log with nothing
else said, so `scripts/check-dll-exports.ps1` verifies the six entry
points on every Windows build and before every release.

This target tests **the checker itself**, against captured `dumpbin`
output, so it runs anywhere PowerShell does — no Windows, no Visual
Studio, no DLL. It falls back to Microsoft's PowerShell container if the
host has no `pwsh`.

It exists because the checker shipped broken: `dumpbin` returns an
*array* of lines, and PowerShell's `-match`/`-notmatch` on a collection
**filters the collection** rather than returning a boolean. So
`$lines -notmatch "Supports"` yielded every line lacking the word — a
non-empty, and therefore truthy, array — and the check reported a missing
export against a perfectly good DLL. The tests pin that behaviour down,
including the case where the raw array is passed in: it now fails loudly
at the parameter instead of misreporting.

## Testing your own script

The plugin's tests don't cover your PAWN. For that, `goradio_debug 1` and
the server log are the tools — see
[Troubleshooting](../guides/troubleshooting.md).

The fake audio server is also useful on its own: point a development
server at it and you get a station that registers, accepts queues and
emits events, without needing real audio or a real `radio serve`.

```sh
python3 test/fake_audioserver.py
# prints e.g. http://127.0.0.1:41319
```

Then in `server.cfg`:

```
goradio_url    http://127.0.0.1:41319
goradio_token  test-token
```

It only implements enough for the tests — it plays no audio and its
responses are canned — but it's enough to exercise your callback wiring.
