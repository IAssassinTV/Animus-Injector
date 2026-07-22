#include "fixes.h"
#include "config.h"
#include "logger.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Psapi.h>
#include <Xinput.h>

#include <array>
#include <span>
#include <string_view>
#include <optional>
#include <format>
#include <bit>
#include <cstdint>
#include <cstring>
#include <ranges>
#include <algorithm>

#pragma comment(lib, "xinput.lib")
#pragma comment(lib, "psapi.lib")

namespace claudia::fixes
{
    namespace
    {
        bool s_active = false;

        struct signature
        {
            std::string_view pattern;
            std::string_view mask;

            [[nodiscard]] constexpr auto length() const noexcept -> std::size_t
            {
                return mask.length();
            }
        };

        [[nodiscard]] auto scan_signature(
            std::span<const std::uint8_t> memory,
            const signature& sig) noexcept -> std::optional<std::uintptr_t>
        {
            const auto* pattern_bytes = reinterpret_cast<const std::uint8_t*>(sig.pattern.data());
            const auto pattern_len = sig.length();

            if (memory.size() < pattern_len)
                return std::nullopt;

            // use ranges to find matching pattern
            auto matches_at = [&](std::size_t offset) {
                return std::ranges::all_of(
                    std::views::iota(std::size_t{0}, pattern_len),
                    [&](std::size_t j) {
                        return sig.mask[j] != 'x' || memory[offset + j] == pattern_bytes[j];
                    }
                );
            };

            for (std::size_t i = 0; i <= memory.size() - pattern_len; ++i)
            {
                if (matches_at(i))
                    return reinterpret_cast<std::uintptr_t>(memory.data() + i);
            }
            return std::nullopt;
        }

        [[nodiscard]] auto find_pattern(HMODULE module, const signature& sig) 
            -> std::optional<std::uintptr_t>
        {
            MODULEINFO mod_info{};
            if (!GetModuleInformation(GetCurrentProcess(), module, &mod_info, sizeof(mod_info)))
                return std::nullopt;

            std::span<const std::uint8_t> memory{
                static_cast<const std::uint8_t*>(mod_info.lpBaseOfDll),
                mod_info.SizeOfImage
            };

            return scan_signature(memory, sig);
        }

        // xinput fix

        namespace signatures
        {
            // AddPad function
            constexpr signature add_pad{
                "\x55\x8B\xEC\x51\x57\x89\x4D",
                "xxxxxxx"
            };

            // PadXenon constructor
            constexpr signature pad_xenon_ctor{
                "\x55\x8B\xEC\x56\x8B\xF1\xE8\x00\x00\x00\x00\x8B\x45\x00\x89\x86\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x66\x0F\xEF\xC0",
                "xxxxxxx????xx?xx??????????xxxx"
            };
            
            // DirectInput enum
            constexpr signature enum_callback{
                "\x55\x8B\xEC\x83\xEC\x00\x53\x8B\x5D\x00\x00\x00\x00\x00\x56\x33\xF6",
                "xxxxx?xxx?????xxx"
            };
        }

        // function typedefs
        using pad_xenon_ctor_t = void*(__thiscall*)(void* this_ptr, int pad_index);
        using add_pad_t = char(__thiscall*)(void* this_ptr, void* pad, int mode, const wchar_t* name, short vendor_id, short product_id);

        // constants
        inline constexpr std::size_t PADXENON_SIZE = 1632;
        inline constexpr int PAD_MODE_XINPUT = 2;

        inline constexpr std::array<const wchar_t*, XUSER_MAX_COUNT> CONTROLLER_NAMES = {
            L"XInput Controller 1",
            L"XInput Controller 2",
            L"XInput Controller 3",
            L"XInput Controller 4"
        };

        // globals for xinput fix
        pad_xenon_ctor_t s_pad_xenon_ctor = nullptr;
        add_pad_t s_add_pad = nullptr;
        void* s_pad_proxy_pc = nullptr;
        void* s_enum_callback_addr = nullptr;
        BYTE s_original_bytes[5] = {};
        bool s_hook_installed = false;

        // helpers
        void* call_pad_xenon_ctor_safe(void* pad_memory, int pad_index, bool& success)
        {
            void* result = nullptr;
            success = false;

            __try
            {
                result = s_pad_xenon_ctor(pad_memory, pad_index);
                success = true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                result = nullptr;
            }

            return result;
        }

        char call_add_pad_safe(void* pad_proxy, void* pad, int mode, const wchar_t* name, 
                               short vid, short pid, bool& success)
        {
            char result = 0;
            success = false;

            __try
            {
                result = s_add_pad(pad_proxy, pad, mode, name, vid, pid);
                success = true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                result = 0;
            }

            return result;
        }

        void inject_xinput_controllers(void* pad_proxy_pc)
        {
            if (!s_pad_xenon_ctor || !s_add_pad || !pad_proxy_pc)
            {
                logger::error("cannot inject xinput controllers functions not resolved or no PadProxyPC");
                return;
            }

            logger::info(std::format("injecting xinput controllers with PadProxyPC at 0x{:X}...", 
                reinterpret_cast<std::uintptr_t>(pad_proxy_pc)));

            int controllers_found = 0;
            int first_controller_index = -1;

            for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i)
            {
                XINPUT_CAPABILITIES caps{};
                if (XInputGetCapabilities(i, XINPUT_FLAG_GAMEPAD, &caps) != ERROR_SUCCESS)
                    continue;

                logger::info(std::format("xinput controller found on port {}", i));

                auto* pad_memory = VirtualAlloc(nullptr, PADXENON_SIZE, 
                                                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (!pad_memory)
                {
                    logger::error(std::format("failed to allocate memory for PadXenon port {}", i));
                    continue;
                }

                logger::debug(std::format("allocated PadXenon memory at 0x{:X}", 
                    reinterpret_cast<std::uintptr_t>(pad_memory)));

                std::memset(pad_memory, 0, PADXENON_SIZE);

                bool ctor_success = false;
                void* pad = call_pad_xenon_ctor_safe(pad_memory, static_cast<int>(i), ctor_success);

                logger::debug(std::format("PadXenon ctor result: success={}, pad=0x{:X}", 
                    ctor_success, reinterpret_cast<std::uintptr_t>(pad)));

                if (!ctor_success || !pad)
                {
                    logger::error(std::format("failed to construct PadXenon for port {}", i));
                    VirtualFree(pad_memory, 0, MEM_RELEASE);
                    continue;
                }

                bool add_success = false;
                char add_result = call_add_pad_safe(pad_proxy_pc, pad, PAD_MODE_XINPUT, 
                                                     CONTROLLER_NAMES[i], 0, 0, add_success);

                logger::debug(std::format("AddPad result: call_success={}, returned={}", 
                    add_success, static_cast<int>(add_result)));

                if (!add_success)
                {
                    logger::error(std::format("exception while registering PadXenon for port {}", i));
                    VirtualFree(pad_memory, 0, MEM_RELEASE);
                    continue;
                }

                if (add_result)
                {
                    logger::info(std::format("registered xinput controller {}", i));
                    if (first_controller_index < 0)
                        first_controller_index = controllers_found;
                    ++controllers_found;
                }
                else
                {
                    logger::warn(std::format("AddPad returned false for port {}", i));
                }
            }

            if (controllers_found > 0)
            {
                logger::info(std::format("xinput fix: registered {} controller(s)", controllers_found));
                
                // HACK: try to autoselect the first xinput controller
                auto* proxy_dwords = static_cast<DWORD*>(pad_proxy_pc);
                
                logger::debug(std::format("PadProxyPC[400] = {}", proxy_dwords[400]));
                logger::debug(std::format("PadProxyPC[401] = {}", proxy_dwords[401]));
                logger::debug(std::format("PadProxyPC[402] = {} (pad count)", proxy_dwords[402]));
                
                proxy_dwords[401] = 0;
                logger::info("attempted to auto-select controller at index 0");
            }
            else
            {
                logger::warn("xinput fix: no controllers were successfully registered");
            }
        }

        void __cdecl do_xinput_injection(void* a3)
        {
            static bool s_injected = false;
            
            if (s_injected || !a3)
                return;
                
            logger::debug(std::format("naked hook triggered with a3=0x{:X}", 
                reinterpret_cast<std::uintptr_t>(a3)));
            
            auto** context = static_cast<void**>(a3);
            
            logger::debug(std::format("  context[0]=0x{:X}", reinterpret_cast<std::uintptr_t>(context[0])));
            logger::debug(std::format("  context[1]=0x{:X}", reinterpret_cast<std::uintptr_t>(context[1])));
            logger::debug(std::format("  context[2]=0x{:X}", reinterpret_cast<std::uintptr_t>(context[2])));
            
            if (context[1])
            {
                s_pad_proxy_pc = context[1];
                logger::info(std::format("captured PadProxyPC at 0x{:X}", 
                    reinterpret_cast<std::uintptr_t>(s_pad_proxy_pc)));
                    
                inject_xinput_controllers(s_pad_proxy_pc);
                s_injected = true;
            }
            else
            {
                logger::error("context[1] is null");
            }
        }

        // asm hook
        __declspec(naked) void naked_enum_callback()
        {
            __asm
            {
                pushad
                pushfd
                
                mov eax, [esp + 44]
                push eax
                call do_xinput_injection
                add esp, 4
                
                popfd
                popad
                
                mov eax, 1
                ret 8
            }
        }

        bool install_jmp_hook(void* target, void* hook)
        {
            DWORD old_protect;
            if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old_protect))
            {
                logger::error("failed to unprotect memory for hook");
                return false;
            }
            
            std::memcpy(s_original_bytes, target, 5);
            s_enum_callback_addr = target;
            
            auto* p = static_cast<BYTE*>(target);
            p[0] = 0xE9;  // JMP rel32
            
            std::int32_t rel_addr = static_cast<std::int32_t>(
                reinterpret_cast<std::uintptr_t>(hook) - reinterpret_cast<std::uintptr_t>(target) - 5
            );
            std::memcpy(p + 1, &rel_addr, 4);
            
            VirtualProtect(target, 5, old_protect, &old_protect);
            
            s_hook_installed = true;
            logger::debug(std::format("JMP hook installed at 0x{:X} -> 0x{:X}", 
                reinterpret_cast<std::uintptr_t>(target), 
                reinterpret_cast<std::uintptr_t>(hook)));
            return true;
        }

        void remove_jmp_hook()
        {
            if (s_hook_installed && s_enum_callback_addr)
            {
                DWORD old_protect;
                if (VirtualProtect(s_enum_callback_addr, 5, PAGE_EXECUTE_READWRITE, &old_protect))
                {
                    std::memcpy(s_enum_callback_addr, s_original_bytes, 5);
                    VirtualProtect(s_enum_callback_addr, 5, old_protect, &old_protect);
                    logger::debug("JMP hook removed");
                }
                s_hook_installed = false;
            }
        }

        bool apply_xinput_detection_fix()
        {
            logger::info("applying xinput controller detection fix");

            HMODULE game_module = GetModuleHandleW(nullptr);
            if (!game_module)
            {
                logger::error("failed to get game module handle");
                return false;
            }

            if (auto addr = find_pattern(game_module, signatures::pad_xenon_ctor))
            {
                logger::info(std::format("found PadXenon ctor at: 0x{:X}", *addr));
                s_pad_xenon_ctor = reinterpret_cast<pad_xenon_ctor_t>(*addr);
            }
            else
            {
                logger::error("failed to find PadXenon constructor");
                return false;
            }

            if (auto addr = find_pattern(game_module, signatures::add_pad))
            {
                logger::info(std::format("found AddPad at: 0x{:X}", *addr));
                s_add_pad = reinterpret_cast<add_pad_t>(*addr);
            }
            else
            {
                logger::error("failed to find AddPad function");
                return false;
            }

            if (auto addr = find_pattern(game_module, signatures::enum_callback))
            {
                logger::info(std::format("found enum callback at: 0x{:X}", *addr));
                
                if (!install_jmp_hook(reinterpret_cast<void*>(*addr), 
                                      reinterpret_cast<void*>(naked_enum_callback)))
                {
                    logger::error("failed to install JMP hook");
                    return false;
                }
            }
            else
            {
                logger::error("failed to find enum callback");
                return false;
            }

            logger::info("xinput detection fix installed - waiting for enum callback");
            return true;
        }

        // cpu affinity

        bool apply_cpu_affinity_fix()
        {
            HANDLE h_process = GetCurrentProcess();

            DWORD_PTR process_affinity_mask = 0;
            DWORD_PTR system_affinity_mask = 0;

            if (!GetProcessAffinityMask(h_process, &process_affinity_mask, &system_affinity_mask))
            {
                logger::error(std::format("failed to get process affinity mask: {}", GetLastError()));
                return false;
            }

            const int core_count = std::popcount(system_affinity_mask);
            logger::info(std::format("detected {} CPU cores", core_count));

            // exclude CPU 0
            const DWORD_PTR new_affinity_mask = process_affinity_mask & ~static_cast<DWORD_PTR>(1);

            if (new_affinity_mask == 0)
            {
                logger::warn("cannot exclude CPU 0, only one core available");
                return false;
            }

            if (!SetProcessAffinityMask(h_process, new_affinity_mask))
            {
                logger::error(std::format("failed to set process affinity mask: {}", GetLastError()));
                return false;
            }

            logger::info("cpu affinity fix applied, excluded CPU 0");
            return true;
        }

        // punkbuster fix

        struct pb_patch
        {
            std::uintptr_t rva;
            const char* name;
        };

        constexpr std::array<std::uint8_t, 5> PB_EXPECTED_BYTES{ 0xA1, 0x20, 0x45, 0x66, 0x02 };
        constexpr std::array<std::uint8_t, 5> PB_PATCH_BYTES{ 0x31, 0xC0, 0xC3, 0x90, 0x90 };

        constexpr std::array<pb_patch, 4> PB_PATCHES{{
            { 0x013A5CD0, "isPbSvEnabled" },
            { 0x013A73C0, "isPbClEnabled" },
            { 0x013A5D30, "EnablePbSv" },
            { 0x013A7480, "EnablePbCl" },
        }};

        bool patch_punkbuster_function(std::uintptr_t base, const pb_patch& p)
        {
            auto* addr = reinterpret_cast<std::uint8_t*>(base + p.rva);

            std::array<std::uint8_t, 5> actual{};
            std::memcpy(actual.data(), addr, actual.size());

            if (actual != PB_EXPECTED_BYTES)
            {
                logger::warn(std::format("punkbuster fix: byte signature mismatch at {} (0x{:X}), skipping",
                    p.name, reinterpret_cast<std::uintptr_t>(addr)));
                return false;
            }

            DWORD old_protect;
            if (!VirtualProtect(addr, actual.size(), PAGE_EXECUTE_READWRITE, &old_protect))
                return false;

            std::memcpy(addr, PB_PATCH_BYTES.data(), PB_PATCH_BYTES.size());
            VirtualProtect(addr, actual.size(), old_protect, &old_protect);
            FlushInstructionCache(GetCurrentProcess(), addr, actual.size());

            logger::info(std::format("punkbuster fix: patched {} at 0x{:X}", p.name, reinterpret_cast<std::uintptr_t>(addr)));
            return true;
        }

        bool apply_punkbuster_fix()
        {
            auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            if (!base)
                return false;

            bool any = false;
            for (const auto& p : PB_PATCHES)
                any |= patch_punkbuster_function(base, p);

            return any;
        }
    }

    auto initialize() -> bool
    {
        const auto& settings = config::get();

        if (settings.fix.fix_cpu_affinity)
        {
            logger::info("applying cpu affinity fix");
            if (!apply_cpu_affinity_fix())
                logger::warn("cpu affinity fix could not be applied");
        }
        else
        {
            logger::info("cpu affinity fix disabled");
        }

        if (settings.fix.fix_xinput_detection)
        {
            logger::info("applying xinput detection fix");
            if (!apply_xinput_detection_fix())
                logger::warn("xinput detection fix could not be applied");
        }
        else
        {
            logger::info("xinput detection fix disabled");
        }

        if (settings.fix.fix_disable_punkbuster)
        {
            logger::info("applying punkbuster fix");
            if (!apply_punkbuster_fix())
                logger::warn("punkbuster fix could not be applied");
        }
        else
        {
            logger::info("punkbuster fix disabled");
        }

        s_active = true;
        return true;
    }

    auto shutdown() -> void
    {
        remove_jmp_hook();
        s_active = false;
    }

    [[nodiscard]] auto is_active() -> bool
    {
        return s_active;
    }
}
