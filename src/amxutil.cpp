#include "amxutil.h"

#include <algorithm>

#include "log.h"
#include "plugincommon.h"

namespace goradio {

AmxExports g_amx;

namespace {

std::vector<AMX *> g_scripts;

void *ExportAt(void **exports, int index) { return exports[index]; }

} // namespace

bool AmxInit(void **exports) {
	if (exports == 0) {
		return false;
	}
	g_amx.Allot = reinterpret_cast<amx_Allot_t>(ExportAt(exports, PLUGIN_AMX_EXPORT_Allot));
	g_amx.Exec = reinterpret_cast<amx_Exec_t>(ExportAt(exports, PLUGIN_AMX_EXPORT_Exec));
	g_amx.FindPublic =
	    reinterpret_cast<amx_FindPublic_t>(ExportAt(exports, PLUGIN_AMX_EXPORT_FindPublic));
	g_amx.GetAddr = reinterpret_cast<amx_GetAddr_t>(ExportAt(exports, PLUGIN_AMX_EXPORT_GetAddr));
	g_amx.GetString =
	    reinterpret_cast<amx_GetString_t>(ExportAt(exports, PLUGIN_AMX_EXPORT_GetString));
	g_amx.Push = reinterpret_cast<amx_Push_t>(ExportAt(exports, PLUGIN_AMX_EXPORT_Push));
	g_amx.Register = reinterpret_cast<amx_Register_t>(ExportAt(exports, PLUGIN_AMX_EXPORT_Register));
	g_amx.Release = reinterpret_cast<amx_Release_t>(ExportAt(exports, PLUGIN_AMX_EXPORT_Release));
	g_amx.SetString =
	    reinterpret_cast<amx_SetString_t>(ExportAt(exports, PLUGIN_AMX_EXPORT_SetString));
	g_amx.StrLen = reinterpret_cast<amx_StrLen_t>(ExportAt(exports, PLUGIN_AMX_EXPORT_StrLen));

	return g_amx.Allot != 0 && g_amx.Exec != 0 && g_amx.FindPublic != 0 && g_amx.GetAddr != 0 &&
	       g_amx.GetString != 0 && g_amx.Push != 0 && g_amx.Register != 0 && g_amx.Release != 0 &&
	       g_amx.SetString != 0 && g_amx.StrLen != 0;
}

void AmxTrackScript(AMX *amx) {
	if (std::find(g_scripts.begin(), g_scripts.end(), amx) == g_scripts.end()) {
		g_scripts.push_back(amx);
	}
}

void AmxForgetScript(AMX *amx) {
	std::vector<AMX *>::iterator it = std::find(g_scripts.begin(), g_scripts.end(), amx);
	if (it != g_scripts.end()) {
		g_scripts.erase(it);
	}
}

void AmxForgetAllScripts() { g_scripts.clear(); }

std::string AmxReadString(AMX *amx, cell param) {
	cell *addr = 0;
	if (g_amx.GetAddr(amx, param, &addr) != AMX_ERR_NONE || addr == 0) {
		return std::string();
	}
	int length = 0;
	if (g_amx.StrLen(addr, &length) != AMX_ERR_NONE || length <= 0) {
		return std::string();
	}
	std::vector<char> buf(static_cast<size_t>(length) + 1, '\0');
	if (g_amx.GetString(&buf[0], addr, 0, buf.size()) != AMX_ERR_NONE) {
		return std::string();
	}
	return std::string(&buf[0]);
}

void AmxWriteString(AMX *amx, cell param, const std::string &value, cell max_cells) {
	if (max_cells <= 0) {
		return;
	}
	cell *addr = 0;
	if (g_amx.GetAddr(amx, param, &addr) != AMX_ERR_NONE || addr == 0) {
		return;
	}
	g_amx.SetString(addr, value.c_str(), 0, 0, static_cast<size_t>(max_cells));
}

void AmxWriteCell(AMX *amx, cell param, cell value) {
	cell *addr = 0;
	if (g_amx.GetAddr(amx, param, &addr) != AMX_ERR_NONE || addr == 0) {
		return;
	}
	*addr = value;
}

AmxCallback &AmxCallback::Int(cell value) {
	Arg arg;
	arg.num = value;
	args_.push_back(arg);
	return *this;
}

AmxCallback &AmxCallback::Int64(long long value) {
	const long long kMax = 2147483647LL;
	const long long kMin = -2147483647LL - 1;
	if (value > kMax) {
		value = kMax;
	} else if (value < kMin) {
		value = kMin;
	}
	return Int(static_cast<cell>(value));
}

AmxCallback &AmxCallback::Str(const std::string &value) {
	Arg arg;
	arg.is_string = true;
	arg.str = value;
	args_.push_back(arg);
	return *this;
}

void AmxCallback::Invoke() {
	for (size_t i = 0; i < g_scripts.size(); ++i) {
		AMX *amx = g_scripts[i];
		int index = -1;
		if (g_amx.FindPublic(amx, name_.c_str(), &index) != AMX_ERR_NONE) {
			continue; // this script doesn't implement the callback
		}

		// Allocate every string argument before pushing anything. There
		// is no way to pop a partially pushed argument list back off the
		// AMX stack, so a heap failure has to be discovered while the
		// stack is still untouched.
		std::vector<cell> addrs(args_.size(), 0);
		bool allocated = false;
		cell first_addr = 0;
		bool alloc_failed = false;
		for (size_t a = args_.size(); a-- > 0;) {
			if (!args_[a].is_string) {
				continue;
			}
			cell amx_addr = 0;
			cell *phys = 0;
			int cells = static_cast<int>(args_[a].str.size()) + 1;
			if (g_amx.Allot(amx, cells, &amx_addr, &phys) != AMX_ERR_NONE) {
				alloc_failed = true;
				break;
			}
			// The heap grows upward, so the first allocation sits lowest;
			// releasing that one address at the end frees all of them.
			if (!allocated) {
				first_addr = amx_addr;
				allocated = true;
			}
			g_amx.SetString(phys, args_[a].str.c_str(), 0, 0, static_cast<size_t>(cells));
			addrs[a] = amx_addr;
		}

		if (alloc_failed) {
			if (allocated) {
				g_amx.Release(amx, first_addr);
			}
			LogWarn(Format("callback %s skipped: the script's heap is full", name_.c_str()));
			continue;
		}

		// PAWN reads arguments off the stack in reverse, so the last one
		// is pushed first.
		for (size_t a = args_.size(); a-- > 0;) {
			g_amx.Push(amx, args_[a].is_string ? addrs[a] : args_[a].num);
		}

		cell ret = 0;
		int err = g_amx.Exec(amx, &ret, index);
		if (err != AMX_ERR_NONE) {
			LogWarn(Format("callback %s failed with AMX error %d", name_.c_str(), err));
		}
		if (allocated) {
			g_amx.Release(amx, first_addr);
		}
	}
}

} // namespace goradio
