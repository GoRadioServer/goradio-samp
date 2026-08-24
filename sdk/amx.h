// Minimal AMX (Pawn abstract machine) interface for SA-MP plugins.
//
// Deliberately narrow: the AMX struct itself is left opaque, because this
// plugin never reads its fields -- everything goes through the amx_*
// entry points the server hands us in ppData[PLUGIN_DATA_AMX_EXPORTS].
// That keeps us immune to the struct-layout differences between Pawn
// versions, which is the usual reason a hand-rolled SDK header breaks.
#ifndef GORADIO_SDK_AMX_H
#define GORADIO_SDK_AMX_H

#include <stddef.h>

#if defined(_MSC_VER)
	typedef __int32 amx_int32_t;
	typedef unsigned __int32 amx_uint32_t;
#else
	#include <stdint.h>
	typedef int32_t amx_int32_t;
	typedef uint32_t amx_uint32_t;
#endif

// SA-MP is built with PAWN_CELL_SIZE=32: one cell is a signed 32-bit word,
// on 32-bit and 64-bit hosts alike.
typedef amx_int32_t cell;
typedef amx_uint32_t ucell;

#define AMXAPI

struct tagAMX;
typedef struct tagAMX AMX;

typedef cell(AMXAPI *AMX_NATIVE)(AMX *amx, cell *params);

typedef struct tagAMX_NATIVE_INFO {
	const char *name;
	AMX_NATIVE func;
} AMX_NATIVE_INFO;

enum {
	AMX_ERR_NONE = 0,
	AMX_ERR_EXIT,
	AMX_ERR_ASSERT,
	AMX_ERR_STACKERR,
	AMX_ERR_BOUNDS,
	AMX_ERR_MEMACCESS,
	AMX_ERR_INVINSTR,
	AMX_ERR_STACKLOW,
	AMX_ERR_HEAPLOW,
	AMX_ERR_CALLBACK,
	AMX_ERR_NATIVE,
	AMX_ERR_DIVIDE,
	AMX_ERR_SLEEP,
	AMX_ERR_INVSTATE
};

typedef int(AMXAPI *amx_Allot_t)(AMX *amx, int cells, cell *amx_addr, cell **phys_addr);
typedef int(AMXAPI *amx_Exec_t)(AMX *amx, cell *retval, int index);
typedef int(AMXAPI *amx_FindPublic_t)(AMX *amx, const char *funcname, int *index);
typedef int(AMXAPI *amx_GetAddr_t)(AMX *amx, cell amx_addr, cell **phys_addr);
typedef int(AMXAPI *amx_GetString_t)(char *dest, const cell *source, int use_wchar, size_t size);
typedef int(AMXAPI *amx_Push_t)(AMX *amx, cell value);
typedef int(AMXAPI *amx_Register_t)(AMX *amx, const AMX_NATIVE_INFO *nativelist, int number);
typedef int(AMXAPI *amx_Release_t)(AMX *amx, cell amx_addr);
typedef int(AMXAPI *amx_SetString_t)(cell *dest, const char *source, int pack, int use_wchar, size_t size);
typedef int(AMXAPI *amx_StrLen_t)(const cell *cstring, int *length);

namespace goradio {

// The amx_* entry points, resolved once in Load() from the server's export
// table. Every one is non-null after AmxInit() returns true.
struct AmxExports {
	amx_Allot_t Allot;
	amx_Exec_t Exec;
	amx_FindPublic_t FindPublic;
	amx_GetAddr_t GetAddr;
	amx_GetString_t GetString;
	amx_Push_t Push;
	amx_Register_t Register;
	amx_Release_t Release;
	amx_SetString_t SetString;
	amx_StrLen_t StrLen;
};

extern AmxExports g_amx;

// Resolves g_amx from ppData[PLUGIN_DATA_AMX_EXPORTS]. Returns false if the
// table or any entry point we need is missing.
bool AmxInit(void **exports);

} // namespace goradio

#endif // GORADIO_SDK_AMX_H
