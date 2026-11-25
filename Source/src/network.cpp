#include "network.hpp"
#include "formats.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <shellapi.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <ifaddrs.h>
#include <net/if.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
extern char** environ;
#endif

namespace {

using Metric = std::uint64_t;
constexpr auto noDefaultRoute = std::numeric_limits<Metric>::max();

struct Candidate {
    NetworkAddress address;
    std::uint32_t ipv4;
    bool virtualAdapter;
    bool hotspot;
    bool wifi;
    Metric defaultRouteMetric;
    Metric interfaceMetric;
    bool skipAsSource;
    std::uint64_t interfaceId;
};

std::string ipv4Text(std::uint32_t ipv4) {
    return std::to_string(ipv4 >> 24) + "." + std::to_string((ipv4 >> 16) & 255) + "." +
           std::to_string((ipv4 >> 8) & 255) + "." + std::to_string(ipv4 & 255);
}

bool isLanAddress(std::uint32_t ipv4) {
    const auto first = ipv4 >> 24;
    return first != 0 && first != 127 && first < 224 && (ipv4 >> 16) != 0xa9fe;
}

std::string lowerAscii(std::string text) {
    for (auto& character : text) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return text;
}

bool containsAny(std::string_view text, std::initializer_list<std::string_view> parts) {
    return std::any_of(parts.begin(), parts.end(), [text](auto part) {
        return text.find(part) != std::string_view::npos;
    });
}

#ifdef _WIN32
std::string utf8(const wchar_t* text) {
    if (!text || !*text) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) {
        return {};
    }
    std::string converted(static_cast<std::size_t>(length), '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, text, -1, converted.data(), length, nullptr, nullptr)) {
        return {};
    }
    converted.pop_back();
    return converted;
}

std::map<std::uint64_t, Metric> defaultRouteMetrics() {
    MIB_IPFORWARD_TABLE2* table = nullptr;
    if (GetIpForwardTable2(AF_INET, &table) != NO_ERROR) {
        return {};
    }
    const std::unique_ptr<MIB_IPFORWARD_TABLE2, decltype(&FreeMibTable)> tableOwner(table, FreeMibTable);
    std::map<std::uint64_t, Metric> metrics;
    for (ULONG index = 0; index < table->NumEntries; ++index) {
        const auto& route = table->Table[index];
        if (route.DestinationPrefix.PrefixLength == 0 && route.ValidLifetime != 0 && !route.Loopback &&
            route.Metric != std::numeric_limits<ULONG>::max()) {
            const auto [entry, inserted] = metrics.emplace(route.InterfaceLuid.Value, route.Metric);
            if (!inserted) {
                entry->second = std::min(entry->second, static_cast<Metric>(route.Metric));
            }
        }
    }
    return metrics;
}

bool isVirtualAdapter(const IP_ADAPTER_ADDRESSES& adapter, std::string_view description) {
    if (adapter.IfType == IF_TYPE_TUNNEL || adapter.IfType == IF_TYPE_PPP ||
        containsAny(description, {"virtual", "hyper-v", "vethernet", "vmware", "virtualbox",
                                  "vpn", "wireguard", "wintun", "tailscale", "zerotier", "tap-", "tun-"})) {
        return true;
    }
    MIB_IF_ROW2 row{};
    row.InterfaceLuid = adapter.Luid;
    return GetIfEntry2(&row) == NO_ERROR && !row.InterfaceAndOperStatusFlags.HardwareInterface;
}

bool shouldSkipAsSource(const IP_ADAPTER_ADDRESSES& adapter,
                        const IP_ADAPTER_UNICAST_ADDRESS& unicast) {
    MIB_UNICASTIPADDRESS_ROW row{};
    row.InterfaceLuid = adapter.Luid;
    row.Address.Ipv4 = *reinterpret_cast<const sockaddr_in*>(unicast.Address.lpSockaddr);
    return GetUnicastIpAddressEntry(&row) == NO_ERROR && row.SkipAsSource;
}

std::vector<Candidate> collectCandidates() {
    // IP Helper requires a suitably aligned buffer and can change its size between calls.
    std::vector<IP_ADAPTER_ADDRESSES> buffer;
    ULONG bytes = 15'000;
    ULONG status = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && status == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize((bytes + sizeof(IP_ADAPTER_ADDRESSES) - 1) / sizeof(IP_ADAPTER_ADDRESSES));
        status = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                                  GAA_FLAG_SKIP_DNS_SERVER,
                                      nullptr, buffer.data(), &bytes);
    }
    if (status != NO_ERROR) {
        return {};
    }

    const auto routeMetrics = defaultRouteMetrics();
    std::vector<Candidate> candidates;
    for (const auto* adapter = buffer.data(); adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        auto name = utf8(adapter->FriendlyName);
        if (name.empty()) {
            name = utf8(adapter->Description);
        }
        const auto description = lowerAscii(name + " " + utf8(adapter->Description));
        const bool virtualAdapter = isVirtualAdapter(*adapter, description);
        const bool hostedWifi = containsAny(description, {"wi-fi direct", "hosted network", "无线托管"});
        const bool wifi = adapter->IfType == IF_TYPE_IEEE80211 && !virtualAdapter;
        const auto route = routeMetrics.find(adapter->Luid.Value);
        const auto effectiveMetric = route == routeMetrics.end()
                                         ? noDefaultRoute
                                         : route->second + adapter->Ipv4Metric;
        for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
            const auto* address = unicast->Address.lpSockaddr;
            if (!address || address->sa_family != AF_INET ||
                unicast->Address.iSockaddrLength < static_cast<int>(sizeof(sockaddr_in)) ||
                (unicast->DadState != IpDadStatePreferred && unicast->DadState != IpDadStateDeprecated)) {
                continue;
            }
            const auto ipv4 = ntohl(reinterpret_cast<const sockaddr_in*>(address)->sin_addr.s_addr);
            if (isLanAddress(ipv4)) {
                // This is the ICS address assigned to Windows Mobile Hotspot's hosted interface.
                const bool hotspot = hostedWifi && ipv4 == 0xc0a88901;
                candidates.push_back({{ipv4Text(ipv4), name}, ipv4, virtualAdapter, hotspot, wifi,
                                      effectiveMetric, adapter->Ipv4Metric,
                                      shouldSkipAsSource(*adapter, *unicast) || unicast->DadState == IpDadStateDeprecated,
                                      adapter->IfIndex});
            }
        }
    }
    return candidates;
}

class ConsoleQrMode {
public:
    ConsoleQrMode() : handle_(GetStdHandle(STD_OUTPUT_HANDLE)) {
        enabled_ = GetConsoleMode(handle_, &previous_) &&
                   SetConsoleMode(handle_, previous_ | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    ~ConsoleQrMode() {
        if (enabled_) {
            SetConsoleMode(handle_, previous_);
        }
    }
    bool available() const { return enabled_; }

private:
    HANDLE handle_;
    DWORD previous_ = 0;
    bool enabled_ = false;
};

bool launchBrowser(const std::string& url) {
    const std::wstring wideUrl(url.begin(), url.end());
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", wideUrl.c_str(), nullptr, nullptr,
                                                   SW_SHOWNORMAL)) > 32;
}
#else
std::vector<Candidate> collectCandidates() {
    ifaddrs* entries = nullptr;
    if (getifaddrs(&entries) != 0) {
        return {};
    }
    const std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> entriesOwner(entries, freeifaddrs);
    std::vector<Candidate> candidates;
    for (const auto* entry = entries; entry; entry = entry->ifa_next) {
        if (!(entry->ifa_flags & IFF_UP) || !(entry->ifa_flags & IFF_RUNNING) ||
            (entry->ifa_flags & IFF_LOOPBACK) || !entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        const auto ipv4 = ntohl(reinterpret_cast<const sockaddr_in*>(entry->ifa_addr)->sin_addr.s_addr);
        if (isLanAddress(ipv4)) {
            const auto name = lowerAscii(entry->ifa_name);
            const bool virtualAdapter = containsAny(name, {"docker", "veth", "virbr", "tun", "tap", "utun",
                                                           "tailscale", "wireguard", "wg"});
            candidates.push_back({{ipv4Text(ipv4), entry->ifa_name}, ipv4, virtualAdapter, false, false,
                                  noDefaultRoute, 0, false, if_nametoindex(entry->ifa_name)});
        }
    }
    return candidates;
}

class ConsoleQrMode {
public:
    bool available() const {
        const auto* term = std::getenv("TERM");
        return isatty(STDOUT_FILENO) && term && std::string_view(term) != "dumb";
    }
};

bool launchBrowser(const std::string& url) {
#ifdef __APPLE__
    char browser[] = "open";
#else
    char browser[] = "xdg-open";
#endif
    char* args[] = {browser, const_cast<char*>(url.c_str()), nullptr};
    pid_t child = 0;
    if (posix_spawnp(&child, browser, nullptr, nullptr, args, environ) != 0) {
        return false;
    }
    std::thread([child] {
        while (waitpid(child, nullptr, 0) == -1 && errno == EINTR) {
        }
    }).detach();
    return true;
}
#endif

std::string urlFor(const std::string& ip, int port) {
    return "http://" + ip + ":" + std::to_string(port);
}

} // namespace

std::vector<NetworkAddress> discoverAddresses() {
    auto candidates = collectCandidates();
    const auto rank = [](const Candidate& candidate) {
        return std::make_tuple(!candidate.hotspot, candidate.virtualAdapter, !candidate.wifi, candidate.defaultRouteMetric,
                               candidate.interfaceMetric, candidate.skipAsSource, candidate.interfaceId,
                               candidate.ipv4);
    };
    std::sort(candidates.begin(), candidates.end(), [&rank](const auto& left, const auto& right) {
        return rank(left) < rank(right);
    });

    std::set<std::uint32_t> seen;
    std::vector<NetworkAddress> addresses;
    for (auto& candidate : candidates) {
        if (seen.insert(candidate.ipv4).second) {
            addresses.push_back(std::move(candidate.address));
        }
    }
    return addresses;
}

void announceServer(int port, const std::vector<NetworkAddress>& addresses,
                    const std::string& advertisedIp, bool openBrowser) {
    const std::string localIp = "127.0.0.1";
    const auto& primaryIp = advertisedIp.empty() ? (addresses.empty() ? localIp : addresses.front().ip)
                                                  : advertisedIp;
    const auto localUrl = urlFor(localIp, port);
    const auto primaryUrl = urlFor(primaryIp, port);

    std::cout << "\n文件快转 · 局域网文件互传\n\n"
              << "  本机访问    " << localUrl << '\n';
    if (primaryIp != localIp) {
        std::cout << "  手机访问    " << primaryUrl;
        const auto primary = std::find_if(addresses.begin(), addresses.end(), [&primaryIp](const auto& address) {
            return address.ip == primaryIp;
        });
        if (primary != addresses.end() && !primary->name.empty()) {
            std::cout << "  [" << primary->name << ']';
        }
        std::cout << '\n';
    } else if (addresses.empty()) {
        std::cout << "  未检测到可用的局域网 IPv4 地址。\n";
    }
    for (const auto& address : addresses) {
        if (address.ip != primaryIp) {
            std::cout << "  其他地址    " << urlFor(address.ip, port) << "  [" << address.name << "]\n";
        }
    }
    if (primaryIp != localIp) {
        const ConsoleQrMode qrMode;
        if (qrMode.available()) {
            const auto qr = makeConsoleQr(primaryUrl);
            if (!qr.empty()) {
                std::cout << '\n' << qr << std::flush;
            }
        }
        std::cout << "\n  手机连接同一 Wi-Fi 或热点后，打开上面的手机访问地址，或扫描首页二维码。\n"
                  << "  若手机无法连接，可在电脑本机首页点击「允许局域网访问」，然后确认 Windows 管理员提示。\n";
    }
    std::cout << "  按 Ctrl+C 停止服务。\n\n" << std::flush;

    if (openBrowser && !launchBrowser(localUrl)) {
        std::cerr << "无法自动打开浏览器，请手动访问 " << localUrl << '\n';
    }
}
