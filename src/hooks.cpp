#include "hooks.h"
#include "claudia.h"
#include "config.h"
#include "logger.h"

#include <WinSock2.h>
#include <WS2tcpip.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <safetyhook.hpp>

#include <format>
#include <string>
#include <string_view>

#pragma comment(lib, "ws2_32.lib")

namespace claudia::hooks
{
    namespace
    {
        constexpr std::string_view ORIGINAL_HOST{"onlineconfigservice.ubi.com"};
        
        bool s_active{false};
        std::string s_redirect_host;
        
        SafetyHookInline s_getaddrinfo_hook{};
        SafetyHookInline s_gethostbyname_hook{};

        int WSAAPI hooked_getaddrinfo(
            PCSTR pNodeName,
            PCSTR pServiceName,
            const ADDRINFOA* pHints,
            PADDRINFOA* ppResult)
        {
            if (pNodeName)
            {
                logger::debug(std::format("getaddrinfo: {}", pNodeName));
                
                if (_stricmp(pNodeName, ORIGINAL_HOST.data()) == 0)
                {
                    logger::info(std::format("getaddrinfo MATCHED -> {}", s_redirect_host));
                    return s_getaddrinfo_hook.stdcall<int>(
                        s_redirect_host.c_str(), pServiceName, pHints, ppResult);
                }
            }
            
            return s_getaddrinfo_hook.stdcall<int>(pNodeName, pServiceName, pHints, ppResult);
        }

        hostent* WSAAPI hooked_gethostbyname(const char* name)
        {
            if (name)
            {
                logger::debug(std::format("gethostbyname: {}", name));
                
                if (_stricmp(name, ORIGINAL_HOST.data()) == 0)
                {
                    logger::info(std::format("gethostbyname MATCHED -> {}", s_redirect_host));
                    return s_gethostbyname_hook.stdcall<hostent*>(s_redirect_host.c_str());
                }
            }
            
            return s_gethostbyname_hook.stdcall<hostent*>(name);
        }

        [[nodiscard]] auto install_hooks() -> bool
        {
            const auto ws2_32 = GetModuleHandleW(L"ws2_32.dll");
            if (!ws2_32)
            {
                logger::error("ws2_32.dll not loaded");
                return false;
            }

            bool success{false};

            if (const auto addr = GetProcAddress(ws2_32, "getaddrinfo"))
            {
                s_getaddrinfo_hook = safetyhook::create_inline(addr, hooked_getaddrinfo);
                if (s_getaddrinfo_hook)
                {
                    logger::info("hooked getaddrinfo");
                    success = true;
                }
            }

            if (const auto addr = GetProcAddress(ws2_32, "gethostbyname"))
            {
                s_gethostbyname_hook = safetyhook::create_inline(addr, hooked_gethostbyname);
                if (s_gethostbyname_hook)
                {
                    logger::info("hooked gethostbyname");
                    success = true;
                }
            }

            return success;
        }
    }

    auto initialize() -> bool
    {
        s_redirect_host = config::get().net.redirect_host;

        logger::info("installing hostname redirect");
        logger::info(std::format("  {} -> {}", ORIGINAL_HOST, s_redirect_host));

        if (install_hooks())
        {
            logger::info("hostname redirect active");
            s_active = true;
            return true;
        }

        logger::error("failed to install hostname redirect");
        return false;
    }

    auto shutdown() -> void
    {
        if (s_active)
        {
            logger::info("removing hostname redirect");
            s_getaddrinfo_hook = {};
            s_gethostbyname_hook = {};
            s_active = false;
        }
    }

    [[nodiscard]] auto is_active() -> bool
    {
        return s_active;
    }
}
