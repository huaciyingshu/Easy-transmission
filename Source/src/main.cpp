#include "network.hpp"
#include "transfer.hpp"
#include "firewall.hpp"

#include <charconv>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {

#ifdef _WIN32
class Utf8ConsoleOutput {
public:
    Utf8ConsoleOutput() : previous_(GetConsoleOutputCP()) {
        changed_ = previous_ != 0 && previous_ != CP_UTF8 && SetConsoleOutputCP(CP_UTF8);
    }
    ~Utf8ConsoleOutput() {
        if (changed_) {
            std::cout.flush();
            std::cerr.flush();
            SetConsoleOutputCP(previous_);
        }
    }

private:
    UINT previous_;
    bool changed_;
};
#endif

struct Options {
    int port = -1;
    std::string advertisedIp;
    bool openBrowser = true;
    bool listAddresses = false;
    bool help = false;
};

bool parseNumber(std::string_view value, int& number) {
    if (value.empty() || value.front() < '0' || value.front() > '9') {
        return false;
    }
    const auto* end = value.data() + value.size();
    const auto result = std::from_chars(value.data(), end, number);
    return result.ec == std::errc{} && result.ptr == end;
}

bool isIpv4(std::string_view address) {
    for (int octet = 0; octet < 4; ++octet) {
        const auto separator = address.find('.');
        if ((separator == std::string_view::npos) != (octet == 3)) {
            return false;
        }
        const auto part = address.substr(0, separator);
        int value = 0;
        if (part.empty() || part.size() > 3 || (part.size() > 1 && part.front() == '0') ||
            !parseNumber(part, value) || value > 255 ||
            (octet == 0 && (value == 0 || value >= 224))) {
            return false;
        }
        if (octet < 3) {
            address.remove_prefix(separator + 1);
        }
    }
    return true;
}

bool parseOptions(int argc, char* argv[], Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        const auto separator = argument.find('=');
        const auto option = argument.substr(0, separator);
        if (option == "--port" || option == "--ip" || option == "--advertise-ip") {
            std::string_view value;
            if (separator != std::string_view::npos) {
                value = argument.substr(separator + 1);
            } else if (index + 1 < argc) {
                value = argv[++index];
            }
            if (option == "--port") {
                if (!parseNumber(value, options.port) || options.port > 65535) {
                    std::cerr << "--port requires an integer from 0 to 65535.\n";
                    return false;
                }
            } else {
                if (!isIpv4(value)) {
                    std::cerr << option << " requires a dotted-decimal IPv4 unicast address.\n";
                    return false;
                }
                options.advertisedIp = value;
            }
        } else if (argument == "--no-browser") {
            options.openBrowser = false;
        } else if (argument == "--list-ips" || argument == "--list-addresses") {
            options.listAddresses = true;
        } else if (argument == "--help" || argument == "-h" || argument == "/?") {
            options.help = true;
        } else {
            std::cerr << "Unknown option: " << argument << "\nUse --help to see available options.\n";
            return false;
        }
    }
    return true;
}

void printHelp() {
    std::cout << "文件快转 · File Quick Transfer\n\n"
              << "Usage: file-quick-transfer [options]\n"
              << "  --port PORT           TCP port (default searches 3000–4000, 0 for an available port).\n"
              << "  --ip IPV4             Override the advertised IPv4 address. Alias: --advertise-ip.\n"
              << "  --no-browser          Do not open the browser automatically.\n"
              << "  --list-ips            Print discovered IPv4 addresses in priority order and exit.\n"
              << "  --list-addresses      Alias for --list-ips.\n"
              << "  --help                Print this help and exit.\n";
}

} // namespace

int main(int argc, char* argv[]) {
#ifdef _WIN32
    if (argc >= 2 && std::string_view(argv[1]) == "--firewall-helper") {
        return firewallHelperMain(argc, argv);
    }
#endif
#ifdef _WIN32
    const Utf8ConsoleOutput consoleOutput;
#endif
    try {
        Options options;
        if (!parseOptions(argc, argv, options)) {
            return 2;
        }
        if (options.help) {
            printHelp();
            return 0;
        }

        const auto addresses = discoverAddresses();
        if (options.listAddresses) {
            for (const auto& address : addresses) {
                std::cout << address.ip << '\t' << address.name << '\n';
            }
            return 0;
        }
        return runServer(options.port, addresses, options.advertisedIp, options.openBrowser);
    } catch (const std::exception& error) {
        std::cerr << "文件快转启动失败: " << error.what() << '\n';
        return 1;
    }
}
