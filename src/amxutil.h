#ifndef GORADIO_AMXUTIL_H
#define GORADIO_AMXUTIL_H

#include <string>
#include <vector>

#include "amx.h"

namespace goradio {

// The AMX instances currently loaded (the gamemode plus any
// filterscripts). Callbacks are offered to every one of them, which is
// how a filterscript can react to a station the gamemode created.
void AmxTrackScript(AMX *amx);
void AmxForgetScript(AMX *amx);
void AmxForgetAllScripts();

// Reads a PAWN string argument.
std::string AmxReadString(AMX *amx, cell param);

// Writes into a PAWN string argument. max_cells is the destination's
// declared size; the value is truncated to fit, always NUL-terminated.
void AmxWriteString(AMX *amx, cell param, const std::string &value, cell max_cells);

// Writes a single cell through a PAWN reference argument (&foo).
void AmxWriteCell(AMX *amx, cell param, cell value);

// Builds a call to a PAWN public and invokes it on every loaded script
// that defines it. Arguments are added left to right; the reversed push
// order PAWN needs is handled here.
class AmxCallback {
public:
	explicit AmxCallback(const char *name) : name_(name) {}

	AmxCallback &Int(cell value);
	AmxCallback &Bool(bool value) { return Int(value ? 1 : 0); }
	// Clamped to a cell: PAWN has no 64-bit integer, and a duration or
	// listener count that overflowed one would be nonsense anyway.
	AmxCallback &Int64(long long value);
	AmxCallback &Str(const std::string &value);

	void Invoke();

private:
	struct Arg {
		bool is_string;
		cell num;
		std::string str;
		Arg() : is_string(false), num(0) {}
	};

	std::string name_;
	std::vector<Arg> args_;
};

} // namespace goradio

#endif // GORADIO_AMXUTIL_H
