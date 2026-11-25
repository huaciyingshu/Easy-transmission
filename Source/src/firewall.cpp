#include "firewall.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <netfw.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <charconv>
#include <string>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
constexpr wchar_t helperSwitch[] = L"--firewall-helper";

std::wstring executablePath() {
    std::vector<wchar_t> buffer(32768);
    const auto size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!size || size >= buffer.size()) return {};
    return std::wstring(buffer.data(), size);
}

struct AutomationString {
    explicit AutomationString(const wchar_t* text) : value(SysAllocString(text)) {}
    ~AutomationString() { SysFreeString(value); }
    AutomationString(const AutomationString&) = delete;
    AutomationString& operator=(const AutomationString&) = delete;
    const BSTR value;
};

bool installRule(int port) {
    const auto path = executablePath();
    if (path.empty()) return false;
    ComPtr<INetFwPolicy2> policy;
    ComPtr<INetFwRules> rules;
    if (FAILED(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(policy.GetAddressOf()))) ||
        FAILED(policy->get_Rules(rules.GetAddressOf()))) return false;

    const auto portText = std::to_wstring(port);
    const auto ruleName = L"File Quick Transfer LAN TCP " + portText + L" - " + path;
    const AutomationString name(ruleName.c_str()), program(path.c_str()), ports(portText.c_str()), subnet(L"LocalSubnet");
    if (!name.value || !program.value || !ports.value || !subnet.value) return false;

    ComPtr<INetFwRule> rule;
    const bool existing = SUCCEEDED(rules->Item(name.value, rule.GetAddressOf())) && rule;
    if (!existing) {
        rule.Reset();
        if (FAILED(CoCreateInstance(__uuidof(NetFwRule), nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(rule.GetAddressOf()))) ||
            FAILED(rule->put_Name(name.value))) return false;
    }
    if (FAILED(rule->put_ApplicationName(program.value)) ||
        FAILED(rule->put_Direction(NET_FW_RULE_DIR_IN)) ||
        FAILED(rule->put_Action(NET_FW_ACTION_ALLOW)) ||
        FAILED(rule->put_Protocol(NET_FW_IP_PROTOCOL_TCP)) ||
        FAILED(rule->put_LocalPorts(ports.value)) ||
        FAILED(rule->put_RemoteAddresses(subnet.value)) ||
        FAILED(rule->put_Profiles(NET_FW_PROFILE2_ALL)) ||
        FAILED(rule->put_Enabled(VARIANT_TRUE))) return false;
    return existing || SUCCEEDED(rules->Add(rule.Get()));
}
}

int firewallHelperMain(int argc, char** argv) {
    if (argc != 3 || std::string_view(argv[1]) != "--firewall-helper") return 1;
    const std::string_view value(argv[2]);
    int port = 0;
    const auto end = value.data() + value.size();
    const auto parsed = std::from_chars(value.data(), end, port);
    if (parsed.ec != std::errc{} || parsed.ptr != end || port < 1 || port > 65535) return 1;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    bool success = false;
    try { success = installRule(port); } catch (...) {}
    CoUninitialize();
    return success ? 0 : 1;
}

FirewallResult allowLanAccess(int port) {
    if (port < 1 || port > 65535) return FirewallResult::failed;
    const auto executable = executablePath();
    if (executable.empty()) return FirewallResult::failed;
    const auto parameters = std::wstring(helperSwitch) + L" " + std::to_wstring(port);
    SHELLEXECUTEINFOW launch{};
    launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    launch.lpVerb = L"runas";
    launch.lpFile = executable.c_str();
    launch.lpParameters = parameters.c_str();
    launch.nShow = SW_HIDE;
    if (!ShellExecuteExW(&launch)) {
        return GetLastError() == ERROR_CANCELLED ? FirewallResult::cancelled : FirewallResult::failed;
    }
    if (!launch.hProcess) return FirewallResult::failed;
    DWORD exitCode = 1;
    const bool completed = WaitForSingleObject(launch.hProcess, 30000) == WAIT_OBJECT_0 &&
                           GetExitCodeProcess(launch.hProcess, &exitCode);
    CloseHandle(launch.hProcess);
    return completed && exitCode == 0 ? FirewallResult::allowed : FirewallResult::failed;
}
