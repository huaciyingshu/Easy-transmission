#pragma once

#include <string>

enum class FirewallResult { allowed, cancelled, failed };

// Requests an inbound TCP exception for this executable on the local subnet.
FirewallResult allowLanAccess(int port);

#ifdef _WIN32
int firewallHelperMain(int argc, char** argv);
#endif
