# Installation

## What you need first

A running [GoRadio](https://github.com/tmfksoft/goradio) audio server
(`radio serve`) and a token for it. The plugin is a controller — it has no
audio pipeline of its own and does nothing useful without a server to
drive.

Generate a token with write access to the slugs you plan to register:

```sh
radio tokengen --slugs myfm,chillfm --write
```

A read-only token can register nothing and queue nothing; every write RPC
comes back `permission_denied`. See the audio server's
[tokengen docs](https://github.com/tmfksoft/goradio) for the full flag set.

## Installing the plugin

1. Put the binary in your server's `plugins/` directory:

    === "Linux"

        ```
        plugins/goradio.so
        ```

    === "Windows"

        ```
        plugins/goradio.dll
        ```

2. Copy `include/goradio.inc` into your Pawno `include/` directory (next
   to `a_samp.inc`).

3. Add the plugin to `server.cfg`, along with the audio server's address
   and your token:

    ```
    plugins goradio.so

    goradio_url    http://127.0.0.1:9090
    goradio_token  eyJhbGciOiJIUzI1NiIs...
    ```

4. Include it in your gamemode or filterscript:

    ```pawn
    #include <a_samp>
    #include <goradio>
    ```

5. Start the server. The log should show the plugin loading and reaching
   the audio server:

    ```
    Loading plugin: goradio.so
    [goradio] goradio 1.0.0 loaded (http only)
    [goradio] using audio server http://127.0.0.1:9090 (2 workers, status poll every 5s)
    [goradio] connected to audio server v0.9.0
     Loaded.
    ```

    That last line — `connected to audio server` — is the one that matters.
    It means the URL is reachable *and* the token was accepted. If it
    doesn't appear, see [Troubleshooting](../guides/troubleshooting.md).

## Which download

Each release ships eight archives: four platforms, each with and without
TLS.

| Platform | Plain | With TLS |
|---|---|---|
| Linux `samp03svr` (32-bit) | `linux-x86` | `linux-x86-tls` |
| Windows `samp-server.exe` (32-bit) | `windows-x86` | `windows-x86-tls` |
| Linux open.mp (64-bit) | `linux-x86_64` | `linux-x86_64-tls` |
| Windows open.mp (64-bit) | `windows-x86_64` | `windows-x86_64-tls` |

Take a **`-tls`** build if `goradio_url` needs to be an `https://`
address — an audio server behind a TLS-terminating proxy, or reached
across the public internet. OpenSSL is linked in statically, so nothing
needs installing on the server.

Take a **plain** build if the audio server is on the same host or a
private network. It's smaller, and it speaks HTTP only.

The file inside is `goradio.so` / `goradio.dll` either way, so switching
variants later is a file copy with no `server.cfg` change. A plain build
given an `https://` URL refuses it at startup rather than quietly sending
your token in the clear:

```
[goradio] goradio 1.0.0 loaded (http only)
[goradio] ERROR: server.cfg: this build has no TLS support, so it cannot
          use an https:// audio server URL
```

The banner line says which variant you have — `(http only)` or
`(https supported)`.

You can also [build from source](../building/index.md); it takes a few
seconds and needs nothing but a C++11 compiler.

!!! warning "SA-MP is 32-bit"
    `samp03svr` is a 32-bit process and will refuse to load a 64-bit
    plugin — usually with a bare `Failed.` next to the plugin name and no
    explanation. The default `make` target builds 32-bit for this reason.
    open.mp is 64-bit, and wants `make BITS=64`.

## Version compatibility

The plugin speaks `audioserver.v1`, the audio server's stable API version.
New RPCs and fields are additive, so a plugin built against an older
server keeps working against a newer one — it just won't use capabilities
it doesn't know about.

`GoRadio_GetServerVersion` reports the audio server's build once
[`OnGoRadioServerInfo`](../pawn-api/callbacks.md#ongoradioserverinfo) has
fired, which is a useful thing to log if you run more than one server.

## Next

- [Quickstart](quickstart.md) — a working station in twenty lines.
- [Configuration](configuration.md) — every `server.cfg` setting.
