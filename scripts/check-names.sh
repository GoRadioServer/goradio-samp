#!/bin/sh
# Checks the PAWN API surface for the two mistakes that don't announce
# themselves:
#
#   1. A native declared in goradio.inc but not registered in
#      natives.cpp, or the reverse. The script compiles fine; the native
#      fails at runtime.
#
#   2. A name longer than PAWN's 31-character symbol limit. The compiler
#      silently truncates it, so the name baked into the .amx no longer
#      matches the one the plugin registers, amx_Register never binds it,
#      and again it fails only at runtime.
#
# Both are cheap to check and expensive to debug. Run with: make check-names
set -e

root=$(cd "$(dirname "$0")/.." && pwd)
inc="$root/include/goradio.inc"
natives="$root/src/natives.cpp"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
status=0

grep -oE '^native (GoRadio_[A-Za-z_]+)' "$inc" | awk '{print $2}' | sort > "$tmp/inc_natives"
grep -oE '\{"(GoRadio_[A-Za-z_]+)"' "$natives" | tr -d '{"' | sort > "$tmp/cpp_natives"

grep -oE '^forward (OnGoRadio[A-Za-z]+)' "$inc" | awk '{print $2}' | sort > "$tmp/inc_cbs"
grep -oE 'AmxCallback\("(OnGoRadio[A-Za-z]+)"' "$natives" \
	| sed 's/AmxCallback("//' | tr -d '"' | sort > "$tmp/cpp_cbs"

echo "checking native registration..."
if diff "$tmp/inc_natives" "$tmp/cpp_natives" > "$tmp/d" 2>&1; then
	echo "  ok: $(wc -l < "$tmp/inc_natives" | tr -d ' ') natives declared and registered"
else
	echo "  MISMATCH between goradio.inc (<) and natives.cpp (>):"
	sed 's/^/    /' "$tmp/d"
	status=1
fi

echo "checking callback dispatch..."
if diff "$tmp/inc_cbs" "$tmp/cpp_cbs" > "$tmp/d" 2>&1; then
	echo "  ok: $(wc -l < "$tmp/inc_cbs" | tr -d ' ') callbacks forwarded and dispatched"
else
	echo "  MISMATCH between goradio.inc (<) and natives.cpp (>):"
	sed 's/^/    /' "$tmp/d"
	status=1
fi

echo "checking PAWN's 31-character symbol limit..."
cat "$tmp/inc_natives" "$tmp/inc_cbs" > "$tmp/all_names"
too_long=0
while read -r name; do
	[ -z "$name" ] && continue
	len=$(printf %s "$name" | wc -c | tr -d ' ')
	if [ "$len" -gt 31 ]; then
		printf '  TOO LONG (%s chars): %s\n' "$len" "$name"
		printf '      PAWN truncates this to "%s", which will\n' \
			"$(printf %s "$name" | cut -c1-31)"
		printf '      not match the name the plugin registers.\n'
		too_long=1
		status=1
	fi
done < "$tmp/all_names"
[ "$too_long" -eq 0 ] && echo "  ok: every name fits in 31 characters"

if [ "$status" -ne 0 ]; then
	echo
	echo "FAILED"
	exit 1
fi
echo
echo "all name checks passed"
