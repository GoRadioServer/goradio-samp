#ifndef GORADIO_SERVERCONFIG_H
#define GORADIO_SERVERCONFIG_H

#include "manager.h"

namespace goradio {

// Reads the goradio_* settings out of server.cfg in the server's working
// directory. SA-MP gives plugins no API for this, so the file is parsed
// directly -- it's a flat "key value" list with the value running to the
// end of the line.
//
// Returns false if the file can't be opened or contains no goradio_* keys
// at all, which is the "this plugin was never configured" case rather
// than an error worth failing a load over. Settings the file doesn't
// mention keep their defaults, so a partial file is fine.
bool ReadServerCfg(ManagerConfig *config, bool *debug);

} // namespace goradio

#endif // GORADIO_SERVERCONFIG_H
