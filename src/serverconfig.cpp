#include "serverconfig.h"

#include <cstdlib>
#include <fstream>
#include <string>

namespace goradio {
namespace {

std::string Trim(const std::string &in) {
	size_t start = 0;
	size_t end = in.size();
	while (start < end && (in[start] == ' ' || in[start] == '\t' || in[start] == '\r')) {
		++start;
	}
	while (end > start && (in[end - 1] == ' ' || in[end - 1] == '\t' || in[end - 1] == '\r')) {
		--end;
	}
	return in.substr(start, end - start);
}

} // namespace

bool ReadServerCfg(ManagerConfig *config, bool *debug) {
	std::ifstream file("server.cfg");
	if (!file.is_open()) {
		return false;
	}
	bool found_any = false;
	std::string line;
	while (std::getline(file, line)) {
		line = Trim(line);
		if (line.empty() || line[0] == '#' || line[0] == ';') {
			continue;
		}
		size_t space = line.find_first_of(" \t");
		if (space == std::string::npos) {
			continue;
		}
		std::string key = line.substr(0, space);
		std::string value = Trim(line.substr(space + 1));
		if (key.compare(0, 8, "goradio_") != 0) {
			continue;
		}
		found_any = true;
		if (key == "goradio_url") {
			config->url = value;
		} else if (key == "goradio_token") {
			config->token = value;
		} else if (key == "goradio_poll_interval") {
			config->poll_interval_ms = std::atoi(value.c_str()) * 1000;
		} else if (key == "goradio_workers") {
			config->workers = std::atoi(value.c_str());
		} else if (key == "goradio_timeout") {
			config->timeout_ms = std::atoi(value.c_str());
		} else if (key == "goradio_debug") {
			*debug = std::atoi(value.c_str()) != 0;
		}
	}
	return found_any;
}

} // namespace goradio
