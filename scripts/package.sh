#!/bin/sh
# Packages one built plugin binary into a release archive.
#
#   scripts/package.sh <version> <platform> <binary>
#   scripts/package.sh v1.0.0 linux-x86 bin/goradio.so
#
# The archive is laid out to mirror a SA-MP server directory, so
# installing is a matter of copying the folders over the server root
# rather than working out where each file goes. Windows binaries are
# zipped, everything else tarred.
set -e

version=$1
platform=$2
binary=$3

if [ -z "$version" ] || [ -z "$platform" ] || [ -z "$binary" ]; then
	echo "usage: $0 <version> <platform> <binary>" >&2
	exit 2
fi
if [ ! -f "$binary" ]; then
	echo "no such binary: $binary" >&2
	exit 1
fi

root=$(cd "$(dirname "$0")/.." && pwd)
name="goradio-${version}-${platform}"
stage="$root/dist/stage/$name"

rm -rf "$stage"
mkdir -p "$stage/plugins" "$stage/pawno/include" "$stage/filterscripts"

cp "$binary" "$stage/plugins/"
cp "$root/include/goradio.inc" "$stage/pawno/include/"
cp "$root/examples/goradio_example.pwn" "$stage/filterscripts/"
cp "$root/README.md" "$stage/"
[ -f "$root/LICENSE" ] && cp "$root/LICENSE" "$stage/"

plugin_file=$(basename "$binary")

cat > "$stage/INSTALL.txt" <<EOF
goradio ${version} (${platform})

A SA-MP plugin for creating and driving GoRadio stations from PAWN.
Documentation: https://tmfksoft.github.io/goradio-samp/

INSTALLING

  1. Copy the folders in this archive over your SA-MP server directory:

       plugins/${plugin_file}
           -> your server's plugins/
       pawno/include/goradio.inc
           -> your Pawno include directory
       filterscripts/goradio_example.pwn
           -> optional, a worked example filterscript

  2. Add the plugin and its settings to server.cfg:

       plugins ${plugin_file}

       goradio_url    http://127.0.0.1:9090
       goradio_token  <a JWT from: radio tokengen --slugs your-slug --write>

  3. Include it in your gamemode or filterscript:

       #include <goradio>

  4. Start the server. You should see:

       [goradio] goradio ${version} loaded (http only)
       [goradio] connected to audio server v0.9.0

     If that second line is missing, the URL or the token is wrong --
     see the Troubleshooting page in the documentation.

REQUIREMENTS

  A running GoRadio audio server (radio serve) and a token authorized
  for the slugs you plan to register. This plugin is a controller; it
  does not serve audio itself.

  This build is ${platform}. samp03svr is 32-bit and will not load an
  x86_64 build; open.mp is 64-bit and will not load an x86 one.
EOF

mkdir -p "$root/dist"
cd "$root/dist/stage"

case "$plugin_file" in
	*.dll)
		archive="$root/dist/$name.zip"
		rm -f "$archive"
		zip -qr "$archive" "$name"
		;;
	*)
		archive="$root/dist/$name.tar.gz"
		rm -f "$archive"
		tar czf "$archive" "$name"
		;;
esac

echo "packaged $archive"
