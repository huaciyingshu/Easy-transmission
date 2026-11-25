#pragma once

#include <string>
#include <vector>

struct NetworkAddress {
    std::string ip;
    std::string name;
};

std::vector<NetworkAddress> discoverAddresses();
void announceServer(int port, const std::vector<NetworkAddress>& addresses,
                    const std::string& advertisedIp, bool openBrowser);
