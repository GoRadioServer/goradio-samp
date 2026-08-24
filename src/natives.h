#ifndef GORADIO_NATIVES_H
#define GORADIO_NATIVES_H

#include "amx.h"

namespace goradio {

// Registers every GoRadio_* native with a newly loaded script.
int RegisterNatives(AMX *amx);

// Drains the manager's queue and fires the matching OnGoRadio* publics.
// Main thread only -- called from ProcessTick.
void DispatchPendingEvents();

} // namespace goradio

#endif // GORADIO_NATIVES_H
