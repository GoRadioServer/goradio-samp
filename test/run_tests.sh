#!/bin/sh
# Builds and runs the host-side tests against a throwaway fake audio
# server. Nothing here needs a SA-MP server, a 32-bit toolchain, or the
# real `radio serve`.
set -e

root=$(cd "$(dirname "$0")/.." && pwd)
out=${TMPDIR:-/tmp}/goradio-tests
mkdir -p "$out"

# Set BITS=32 to run the suite compiled the way the plugin actually ships.
# Worth doing before a release: the 32-bit build is the one SA-MP loads,
# and it is the one where a size_t or pointer assumption would show up.
bitflag=""
[ -n "$BITS" ] && bitflag="-m$BITS"

echo "building tests...${BITS:+ (${BITS}-bit)}"
${CXX:-g++} -std=c++11 -Wall -Wextra -g $bitflag -I"$root/sdk" -I"$root/src" \
	"$root/test/run_tests.cpp" \
	"$root/src/json.cpp" "$root/src/http.cpp" "$root/src/log.cpp" \
	"$root/src/connect_client.cpp" "$root/src/station.cpp" "$root/src/manager.cpp" \
	-o "$out/run_tests" -lpthread

echo "starting the fake audio server..."
python3 "$root/test/fake_audioserver.py" > "$out/url.txt" 2> "$out/server.log" &
server_pid=$!
trap 'kill $server_pid 2>/dev/null || true' EXIT

url=""
i=0
while [ $i -lt 50 ]; do
	url=$(cat "$out/url.txt" 2>/dev/null || true)
	[ -n "$url" ] && break
	i=$((i + 1))
	sleep 0.1
done
if [ -z "$url" ]; then
	echo "fake server never reported a URL; log:" >&2
	cat "$out/server.log" >&2
	exit 1
fi

"$out/run_tests" "$url"
