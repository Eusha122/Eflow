#include "core/Firewall.h"

#include <winsock2.h>
#include <windows.h>
#include <netfw.h>

#include <thread>

#include "core/Gfx.h"
#include "core/Log.h"

namespace eflow {

FirewallAccess InboundAccess(const std::wstring& exe) {
    Com<INetFwPolicy2> policy;
    if (FAILED(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&policy)))) return FirewallAccess::Unknown;
    long profiles = 0;
    if (FAILED(policy->get_CurrentProfileTypes(&profiles))) return FirewallAccess::Unknown;
    bool on = false;
    for (NET_FW_PROFILE_TYPE2 p : {NET_FW_PROFILE2_DOMAIN, NET_FW_PROFILE2_PRIVATE, NET_FW_PROFILE2_PUBLIC}) {
        VARIANT_BOOL enabled = VARIANT_FALSE;
        if ((profiles & p) && SUCCEEDED(policy->get_FirewallEnabled(p, &enabled)) && enabled) on = true;
    }
    if (!on) return FirewallAccess::Allowed;                      // the firewall is off for this network

    Com<INetFwRules> rules;
    Com<IUnknown> unknown;
    Com<IEnumVARIANT> items;
    if (FAILED(policy->get_Rules(&rules)) || FAILED(rules->get__NewEnum(&unknown)) || FAILED(unknown->QueryInterface(IID_PPV_ARGS(&items))))
        return FirewallAccess::Unknown;
    bool allowed = false, blocked = false;
    VARIANT item;
    VariantInit(&item);
    ULONG fetched = 0;
    while (items->Next(1, &item, &fetched) == S_OK && fetched == 1) {
        Com<INetFwRule> rule;
        if (item.vt == VT_DISPATCH && item.pdispVal && SUCCEEDED(item.pdispVal->QueryInterface(IID_PPV_ARGS(&rule)))) {
            BSTR app = nullptr;
            NET_FW_RULE_DIRECTION direction = NET_FW_RULE_DIR_OUT;
            VARIANT_BOOL enabled = VARIANT_FALSE;
            long ruleProfiles = 0;
            NET_FW_ACTION action = NET_FW_ACTION_ALLOW;
            if (SUCCEEDED(rule->get_ApplicationName(&app)) && app && _wcsicmp(app, exe.c_str()) == 0 &&
                SUCCEEDED(rule->get_Direction(&direction)) && direction == NET_FW_RULE_DIR_IN && SUCCEEDED(rule->get_Enabled(&enabled)) &&
                enabled && SUCCEEDED(rule->get_Profiles(&ruleProfiles)) && (ruleProfiles & profiles) && SUCCEEDED(rule->get_Action(&action)))
                (action == NET_FW_ACTION_BLOCK ? blocked : allowed) = true;
            SysFreeString(app);
        }
        VariantClear(&item);
    }
    return blocked ? FirewallAccess::Blocked : allowed ? FirewallAccess::Allowed : FirewallAccess::NotAsked;
}

void AskInboundAccess() {
    std::thread([] {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in at{};
        at.sin_family = AF_INET;
        at.sin_addr.s_addr = htonl(INADDR_ANY);
        if (s != INVALID_SOCKET && bind(s, reinterpret_cast<sockaddr*>(&at), sizeof(at)) == 0 && listen(s, 1) == 0) {
            Log("firewall: listening briefly so Windows can ask about incoming connections");
            Sleep(4000);
        }
        if (s != INVALID_SOCKET) closesocket(s);
        WSACleanup();
    }).detach();
}

} // namespace eflow
