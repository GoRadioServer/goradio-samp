# syntax=docker/dockerfile:1
#
# A build image for the 32-bit plugin, so you don't need a multilib
# toolchain on the host. SA-MP is a 32-bit process, and cross-compiling
# 32-bit C++ needs 32-bit runtime libraries that most distributions no
# longer install by default -- this container has them.
#
#   docker build --target export --output type=local,dest=bin .
#   -> bin/goradio.so, 32-bit
#
# Or via the Makefile, which passes the version through for you:
#
#   make docker
#
# Build arguments: VERSION (baked into the binary), BITS (32 or 64),
# TLS (0 or 1, links OpenSSL for https:// audio server URLs).

FROM debian:bookworm-slim AS build

# i386 is enabled so the 32-bit OpenSSL development files are available
# for TLS=1 builds; without it only the 64-bit ones exist.
RUN dpkg --add-architecture i386 \
	&& apt-get update \
	&& apt-get install -y --no-install-recommends \
		ca-certificates \
		g++ \
		g++-multilib \
		git \
		libssl-dev \
		libssl-dev:i386 \
		make \
		python3 \
	&& rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

ARG VERSION=dev
ARG BITS=32
ARG TLS=0

RUN make clean && make BITS="${BITS}" TLS="${TLS}" VERSION="${VERSION}"

# Verify we actually produced what was asked for: a 64-bit binary silently
# handed to samp03svr fails to load with no useful message, so catch it
# here instead.
RUN readelf -h bin/goradio.so | grep -q "ELF${BITS}" \
		|| (echo "expected a ${BITS}-bit binary; got:" >&2 \
			&& readelf -h bin/goradio.so | head -3 >&2 && exit 1)

# Runs the host-side suite compiled 32-bit -- the way the plugin actually
# ships. Not part of the export path, so build it explicitly:
#   docker build --target test .
FROM build AS test
RUN BITS="${BITS}" sh test/run_tests.sh

# Type-checks every source against real Windows headers, at both widths,
# without needing a Windows machine:
#   make check-windows
#
# Not a substitute for the MSVC build in CI, but it catches the class of
# error that actually broke a release -- Winsock's setsockopt takes
# `const char *` where POSIX takes `const void *`, and the compiler is
# the only thing that reliably notices.
FROM debian:bookworm-slim AS windows-check
RUN apt-get update && apt-get install -y --no-install-recommends \
		ca-certificates \
		g++-mingw-w64-i686 \
		g++-mingw-w64-x86-64 \
	&& rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
# Two things this needs to be worth running:
#
#   * the -posix compiler variants, because MinGW's default win32
#     threading model has no <mutex>/<thread> at all;
#   * TCP_KEEPIDLE and friends force-defined to their Windows SDK values,
#     because MinGW's headers do not declare them -- so the code they
#     guard would be preprocessed away unchecked, which is precisely how
#     the setsockopt bug reached a release.
RUN set -e; \
	for cc in i686-w64-mingw32-g++-posix x86_64-w64-mingw32-g++-posix; do \
		echo "=== $cc ==="; \
		for f in src/*.cpp; do \
			$cc -std=c++11 -W -Wall -Wextra -Wno-unused-parameter \
				-DTCP_KEEPIDLE=3 -DTCP_KEEPINTVL=17 -DTCP_KEEPCNT=16 \
				-Isdk -Isrc -DGORADIO_VERSION='"windows-check"' -fsyntax-only "$f"; \
		done; \
	done; \
	echo "all sources compile against Windows headers, 32- and 64-bit"

FROM scratch AS export
COPY --from=build /src/bin/goradio.so /
