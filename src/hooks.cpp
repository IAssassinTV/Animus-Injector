#include "hooks.h"
#include "animus_injector.h"
#include "config.h"
#include "logger.h"

#include <WinSock2.h>
#include <WS2tcpip.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Psapi.h>

#include <safetyhook.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "psapi.lib")

namespace animus_injector::hooks
{
    namespace
    {
        constexpr std::string_view ORIGINAL_HOST{"onlineconfigservice.ubi.com"};

        using getaddrinfo_fn = int (WSAAPI*)(PCSTR, PCSTR, const ADDRINFOA*, PADDRINFOA*);
        using gethostbyname_fn = hostent* (WSAAPI*)(const char*);
        using getprocaddress_fn = FARPROC (WINAPI*)(HMODULE, LPCSTR);

        bool s_active{false};
        std::string s_redirect_host;

        // exported addresses inside ws2_32.dll, used to recognise import table
        // entries and GetProcAddress results that point at them.
        void* s_getaddrinfo_export{nullptr};
        void* s_gethostbyname_export{nullptr};

        // what the detours call to reach the real function: the inline hook
        // trampoline when the export is patched, the export itself otherwise.
        getaddrinfo_fn s_real_getaddrinfo{nullptr};
        gethostbyname_fn s_real_gethostbyname{nullptr};
        getprocaddress_fn s_real_getprocaddress{nullptr};

        SafetyHookInline s_getaddrinfo_hook{};
        SafetyHookInline s_gethostbyname_hook{};

        struct iat_patch
        {
            void** slot;
            void* original;
            void* replacement;
        };

        std::vector<iat_patch> s_iat_patches;

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
                    return s_real_getaddrinfo(s_redirect_host.c_str(), pServiceName, pHints, ppResult);
                }
            }

            return s_real_getaddrinfo(pNodeName, pServiceName, pHints, ppResult);
        }

        hostent* WSAAPI hooked_gethostbyname(const char* name)
        {
            if (name)
            {
                logger::debug(std::format("gethostbyname: {}", name));

                if (_stricmp(name, ORIGINAL_HOST.data()) == 0)
                {
                    logger::info(std::format("gethostbyname MATCHED -> {}", s_redirect_host));
                    return s_real_gethostbyname(s_redirect_host.c_str());
                }
            }

            return s_real_gethostbyname(name);
        }

        // only installed in import table mode: code that resolves the resolver
        // functions at runtime would otherwise bypass the patched import slots.
        FARPROC WINAPI hooked_getprocaddress(HMODULE module, LPCSTR name)
        {
            const auto result = s_real_getprocaddress(module, name);

            if (result && reinterpret_cast<void*>(result) == s_getaddrinfo_export && !s_getaddrinfo_hook)
                return reinterpret_cast<FARPROC>(&hooked_getaddrinfo);

            if (result && reinterpret_cast<void*>(result) == s_gethostbyname_export && !s_gethostbyname_hook)
                return reinterpret_cast<FARPROC>(&hooked_gethostbyname);

            return result;
        }

        [[nodiscard]] auto module_name_of(const void* address) -> std::string
        {
            HMODULE module{nullptr};
            if (!GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    static_cast<LPCWSTR>(address), &module))
                return "<no module>";

            char path[MAX_PATH]{};
            if (GetModuleFileNameA(module, path, MAX_PATH) == 0)
                return "<unknown module>";

            const char* filename = strrchr(path, '\\');
            return filename ? filename + 1 : path;
        }

        [[nodiscard]] auto hex_bytes(const void* address, std::size_t count) -> std::string
        {
            std::string result;
            const auto* bytes = static_cast<const std::uint8_t*>(address);
            for (std::size_t i = 0; i < count; ++i)
                result += std::format("{}{:02X}", i ? " " : "", bytes[i]);
            return result;
        }

        // another program that already hooked the export leaves a jump at its
        // start; follow it so the log names who owns the function now.
        [[nodiscard]] auto follow_jumps(std::uint8_t* address) -> std::uint8_t*
        {
            for (int depth = 0; depth < 4; ++depth)
            {
                if (address[0] == 0xE9)
                    address = address + 5 + *reinterpret_cast<std::int32_t*>(address + 1);
                else if (address[0] == 0xEB)
                    address = address + 2 + *reinterpret_cast<std::int8_t*>(address + 1);
                else if (address[0] == 0xFF && address[1] == 0x25)
                    address = **reinterpret_cast<std::uint8_t***>(address + 2);
                else
                    break;
            }
            return address;
        }

        [[nodiscard]] auto describe(const safetyhook::InlineHook::Error& error) -> std::string
        {
            using Error = safetyhook::InlineHook::Error;

            switch (error.type)
            {
            case Error::BAD_ALLOCATION:
                return error.allocator_error == safetyhook::Allocator::Error::BAD_VIRTUAL_ALLOC
                    ? "could not allocate executable memory (blocked by exploit protection or security software?)"
                    : "no free memory in range for the trampoline";
            case Error::FAILED_TO_DECODE_INSTRUCTION:
                return std::format("failed to decode instruction at {}", static_cast<void*>(error.ip));
            case Error::SHORT_JUMP_IN_TRAMPOLINE:
                return std::format("short jump in trampoline at {}", static_cast<void*>(error.ip));
            case Error::IP_RELATIVE_INSTRUCTION_OUT_OF_RANGE:
                return std::format("IP-relative instruction out of range at {}", static_cast<void*>(error.ip));
            case Error::UNSUPPORTED_INSTRUCTION_IN_TRAMPOLINE:
                return std::format("unsupported instruction at {}", static_cast<void*>(error.ip));
            case Error::FAILED_TO_UNPROTECT:
                return std::format("failed to unprotect memory at {}", static_cast<void*>(error.ip));
            case Error::NOT_ENOUGH_SPACE:
                return std::format("not enough space at {}", static_cast<void*>(error.ip));
            }

            return std::format("unknown error {}", static_cast<int>(error.type));
        }

        auto log_target(std::string_view name, void* target) -> void
        {
            auto* bytes = static_cast<std::uint8_t*>(target);
            logger::info(std::format("  {} at {} ({}): {}",
                name, target, module_name_of(target), hex_bytes(target, 8)));

            if (auto* destination = follow_jumps(bytes); destination != bytes)
            {
                logger::warn(std::format("  {} is already hooked by {} (jumps to {})",
                    name, module_name_of(destination), static_cast<void*>(destination)));
            }
        }

        template <typename Fn>
        [[nodiscard]] auto install_inline(
            std::string_view name, void* target, void* detour, SafetyHookInline& hook, Fn& real) -> bool
        {
            // start disabled so `real` is set before any thread can reach the detour
            auto result = safetyhook::InlineHook::create(target, detour, safetyhook::InlineHook::StartDisabled);
            if (!result)
            {
                logger::warn(std::format("could not hook {} inline: {} (last error {})",
                    name, describe(result.error()), GetLastError()));
                return false;
            }

            hook = std::move(*result);
            real = hook.original<Fn>();

            if (auto enabled = hook.enable(); !enabled)
            {
                logger::warn(std::format("could not enable {} hook: {}", name, describe(enabled.error())));
                hook = {};
                return false;
            }

            logger::info(std::format("hooked {}", name));
            return true;
        }

        [[nodiscard]] auto write_slot(void** slot, void* value) -> bool
        {
            DWORD old_protect{};
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protect))
                return false;

            *slot = value;
            VirtualProtect(slot, sizeof(void*), old_protect, &old_protect);
            return true;
        }

        // replaces every import table entry holding `original` in every loaded
        // module; entries are matched by address, so ordinal imports and
        // wsock32.dll forwarders are covered as well.
        [[nodiscard]] auto patch_imports(void* original, void* replacement) -> std::size_t
        {
            HMODULE self{nullptr};
            GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&patch_imports), &self);

            std::vector<HMODULE> modules(512);
            DWORD needed{};
            if (!EnumProcessModules(GetCurrentProcess(), modules.data(),
                    static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &needed))
                return 0;
            modules.resize(std::min<std::size_t>(modules.size(), needed / sizeof(HMODULE)));

            std::size_t patched{0};

            for (const auto module : modules)
            {
                if (module == self)
                    continue;

                auto* base = reinterpret_cast<std::uint8_t*>(module);
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
                if (dos->e_magic != IMAGE_DOS_SIGNATURE)
                    continue;

                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
                if (nt->Signature != IMAGE_NT_SIGNATURE)
                    continue;

                const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
                if (directory.VirtualAddress == 0 || directory.Size == 0)
                    continue;

                for (auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
                     descriptor->Name != 0; ++descriptor)
                {
                    if (descriptor->FirstThunk == 0)
                        continue;

                    for (auto** slot = reinterpret_cast<void**>(base + descriptor->FirstThunk); *slot; ++slot)
                    {
                        if (*slot != original)
                            continue;

                        if (!write_slot(slot, replacement))
                        {
                            logger::warn(std::format("  failed to patch import in {} (error {})",
                                module_name_of(module), GetLastError()));
                            continue;
                        }

                        s_iat_patches.push_back({slot, original, replacement});
                        logger::info(std::format("  patched import in {}", module_name_of(module)));
                        ++patched;
                    }
                }
            }

            return patched;
        }

        [[nodiscard]] auto install_import_fallback(std::string_view name, void* original, void* detour) -> bool
        {
            logger::info(std::format("falling back to import table patching for {}", name));
            return patch_imports(original, detour) > 0;
        }

        [[nodiscard]] auto install_hooks() -> bool
        {
            const auto ws2_32 = GetModuleHandleW(L"ws2_32.dll");
            if (!ws2_32)
            {
                logger::error("ws2_32.dll not loaded");
                return false;
            }

            s_getaddrinfo_export = reinterpret_cast<void*>(GetProcAddress(ws2_32, "getaddrinfo"));
            s_gethostbyname_export = reinterpret_cast<void*>(GetProcAddress(ws2_32, "gethostbyname"));

            if (!s_getaddrinfo_export || !s_gethostbyname_export)
            {
                logger::error(std::format("ws2_32.dll exports missing (getaddrinfo {}, gethostbyname {})",
                    s_getaddrinfo_export, s_gethostbyname_export));
                return false;
            }

            log_target("getaddrinfo", s_getaddrinfo_export);
            log_target("gethostbyname", s_gethostbyname_export);

            s_real_getaddrinfo = reinterpret_cast<getaddrinfo_fn>(s_getaddrinfo_export);
            s_real_gethostbyname = reinterpret_cast<gethostbyname_fn>(s_gethostbyname_export);

            // a partial redirect is not good enough: the games resolve the
            // config host through either function, so each one must be covered.
            bool getaddrinfo_ok = install_inline("getaddrinfo", s_getaddrinfo_export,
                reinterpret_cast<void*>(&hooked_getaddrinfo), s_getaddrinfo_hook, s_real_getaddrinfo);
            bool gethostbyname_ok = install_inline("gethostbyname", s_gethostbyname_export,
                reinterpret_cast<void*>(&hooked_gethostbyname), s_gethostbyname_hook, s_real_gethostbyname);

            if (getaddrinfo_ok && gethostbyname_ok)
                return true;

            // import table mode: the detours call the unmodified exports directly.
            if (!getaddrinfo_ok)
            {
                getaddrinfo_ok = install_import_fallback("getaddrinfo",
                    s_getaddrinfo_export, reinterpret_cast<void*>(&hooked_getaddrinfo));
            }

            if (!gethostbyname_ok)
            {
                gethostbyname_ok = install_import_fallback("gethostbyname",
                    s_gethostbyname_export, reinterpret_cast<void*>(&hooked_gethostbyname));
            }

            // catch runtime lookups of whichever function is not inline hooked.
            // modules importing through api sets resolve to kernelbase instead.
            std::size_t runtime_lookups{0};
            for (const auto name : {L"kernel32.dll", L"kernelbase.dll"})
            {
                const auto module = GetModuleHandleW(name);
                const auto get_proc_address = module ? GetProcAddress(module, "GetProcAddress") : nullptr;
                if (!get_proc_address)
                    continue;

                if (!s_real_getprocaddress)
                    s_real_getprocaddress = reinterpret_cast<getprocaddress_fn>(get_proc_address);

                runtime_lookups += patch_imports(reinterpret_cast<void*>(get_proc_address),
                    reinterpret_cast<void*>(&hooked_getprocaddress));
            }

            if (runtime_lookups > 0)
                logger::info("redirecting runtime GetProcAddress lookups");
            if (!getaddrinfo_ok)
                logger::warn("getaddrinfo has no import to patch; only runtime lookups are redirected");
            if (!gethostbyname_ok)
                logger::warn("gethostbyname has no import to patch; only runtime lookups are redirected");

            return getaddrinfo_ok || gethostbyname_ok || runtime_lookups > 0;
        }

        auto restore_imports() -> void
        {
            for (auto it = s_iat_patches.rbegin(); it != s_iat_patches.rend(); ++it)
            {
                // the owning module may have been unloaded or re-patched since
                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQuery(it->slot, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT)
                    continue;

                if (*it->slot == it->replacement)
                    (void)write_slot(it->slot, it->original);
            }

            s_iat_patches.clear();
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
            restore_imports();
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
