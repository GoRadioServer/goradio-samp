# Building from Source

No SDK to fetch, no dependencies to install. `sdk/` carries a hand-written
subset of the SA-MP plugin ABI, and the HTTP client, JSON parser and
protocol framing are all in `src/`. You need a C++11 compiler and nothing
else.

## Linux

```sh
make                # bin/goradio.so, 32-bit -- what samp03svr loads
make BITS=64        # 64-bit, for open.mp
make TLS=1          # link OpenSSL, so https:// audio server URLs work
make test           # host-side tests, no SA-MP server needed
make clean
```

The default is **32-bit**, because that's what `samp03svr` loads. That
needs 32-bit runtime libraries, which most distributions no longer install
by default:

=== "Debian / Ubuntu"

    ```sh
    sudo apt install g++-multilib
    ```

=== "Fedora / RHEL"

    ```sh
    sudo dnf install glibc-devel.i686 libstdc++-devel.i686
    ```

=== "Arch"

    ```sh
    sudo pacman -S lib32-gcc-libs
    # multilib must be enabled in /etc/pacman.conf
    ```

If you'd rather not install any of that, use the container build below.

## Building in Docker

The repository ships a build image with the 32-bit toolchain already in
it, so the host needs nothing but Docker:

```sh
make docker                       # bin/goradio.so, 32-bit
make docker BITS=64               # 64-bit
make docker TLS=1                 # with OpenSSL
```

Or directly, if you prefer:

```sh
docker build --target export --output type=local,dest=bin .
```

The build fails rather than exporting if the binary comes out the wrong
width — a 64-bit `.so` handed to `samp03svr` fails to load with no useful
message, so it's better caught here.

You can also run the test suite inside the container, compiled the way the
plugin actually ships:

```sh
docker build --target test --build-arg BITS=32 .
```

## Windows

```sh
cmake -B build -A Win32 -DGORADIO_32BIT=OFF
cmake --build build --config Release
```

`-A Win32` selects the 32-bit target — that's the one `samp-server.exe`
loads. Use `-A x64` for open.mp.

`GORADIO_32BIT` is a GCC-only switch (it adds `-m32`); with MSVC the
architecture comes from `-A`, so turn it off to avoid confusion.

The entry points are `__stdcall` on Windows and `sdk/goradio.def`
re-exports them undecorated, which is how the server resolves them. CMake
applies the `.def` automatically for MSVC builds.

## Setting the version

The version is baked in at build time and reported in the server log:

```sh
make VERSION=v1.0.0
cmake -B build -DGORADIO_VERSION=v1.0.0
```

Left alone, `make` asks git (`git describe`) and falls back to `dev`.
Release builds pass the tag in explicitly.

## TLS

Plain HTTP is the default, on the assumption the audio server is on a
trusted network — often the same host.

If it sits behind a TLS-terminating proxy, build with OpenSSL:

```sh
make TLS=1
cmake -B build -DGORADIO_TLS=ON
```

Certificates and hostnames are both verified, with SNI. A non-TLS build
given an `https://` URL refuses it and says so, rather than quietly
falling back to plaintext with your token in it.

For a 32-bit TLS build you need 32-bit OpenSSL development files
(`libssl-dev:i386` on Debian, which is already in the Docker image).

## Build options

| Option | `make` | CMake | Default |
|---|---|---|---|
| Architecture | `BITS=32` / `BITS=64` | `-A Win32` / `-DGORADIO_32BIT` | 32-bit |
| TLS | `TLS=1` | `-DGORADIO_TLS=ON` | off |
| Version | `VERSION=v1.0.0` | `-DGORADIO_VERSION=v1.0.0` | `git describe`, else `dev` |
| Compiler | `CXX=clang++` | `CMAKE_CXX_COMPILER` | `g++` |

## What gets built

One shared library with exactly **six exported symbols** — the plugin
entry points the server resolves:

```sh
$ nm -D --defined-only bin/goradio.so
00034e60 T AmxLoad
00034e90 T AmxUnload
00034ed0 T Load
00034eb0 T ProcessTick
00034dd0 T Supports
00034de0 T Unload
```

That list staying at six matters. The C++ runtime is linked statically so
one build works across servers with different system libraries, and
without the version script in `sdk/goradio.ver` that would leave thousands
of runtime symbols visible. SA-MP loads several plugins into one process
and the dynamic linker resolves the first definition it finds, so stray
exports are a real way for one plugin to break another. CI asserts the
count.

## Releases

Tagging `vX.Y.Z` and pushing the tag builds and publishes all four
binaries — Linux and Windows, 32- and 64-bit — as GitHub release
archives, each laid out to mirror a SA-MP server directory.

```sh
git tag -a v1.0.0 -m "v1.0.0"
git push origin v1.0.0
```

The same workflow can be run by hand from the Actions tab with an explicit
version, which is useful for a re-run without moving a tag.

Every release build runs the test suite at its own architecture first, and
verifies the binary really is the width it claims before packaging.

## Repository layout

| Path | What |
|---|---|
| `src/` | The plugin. See the table in `CLAUDE.md` for what each file does. |
| `sdk/` | Hand-written SA-MP ABI subset, the Windows `.def`, the linker version script. |
| `include/goradio.inc` | The PAWN API. |
| `examples/` | A worked filterscript. |
| `test/` | Host-side tests and the fake audio server. |
| `docs/` | This documentation. |
| `scripts/package.sh` | Builds a release archive from a binary. |
