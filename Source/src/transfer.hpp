#pragma once

#include "network.hpp"

int runServer(int preferredPort, const std::vector<NetworkAddress>& addresses,
              const std::string& advertisedIp, bool openBrowser);
