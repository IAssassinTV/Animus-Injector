#include "fixes.h"
#include "animus_injector.h"
#include "config.h"
#include "logger.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Psapi.h>
#include <Xinput.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <array>
#include <atomic>
#include <span>
#include <string>
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

namespace animus_injector::fixes
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

        bool apply_xinput_fix_acbmp()
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

        // ACRMP xinput fix (replicates ACRMP-ControllerFix approach)

        namespace acr
        {
            constexpr std::uint8_t ctor_pattern[] = {
                0x55, 0x8b, 0xec, 0x56, 0x8b, 0xf1, 0xe8, 0x00, 0x00, 0x00, 0x00,
                0x8b, 0x45, 0x08, 0x89, 0x86, 0x40, 0x06, 0x00, 0x00, 0xc7, 0x06
            };
            constexpr char ctor_mask[] = "xxxxxxx????xxxxxxxxxxx";

            constexpr std::uint8_t add_pad_pattern[] = {
                0x55, 0x8b, 0xec, 0x53, 0x8b, 0xd9, 0x33,
                0xc0, 0x8d, 0x8b, 0x4c, 0x06, 0x00, 0x00
            };
            constexpr char add_pad_mask[] = "xxxxxxxxxxxxxx";

            constexpr std::uint8_t enum_callback_pattern[] = {
                0x55, 0x8b, 0xec, 0x83, 0xec, 0x18, 0x53, 0x8b, 0x5d,
                0x0c, 0x8b, 0x03, 0x8b, 0x08, 0x56, 0x33, 0xf6
            };
            constexpr char enum_callback_mask[] = "xxxxxxxxxxxxxxxxx";
        }

        using acr_pad_xenon_ctor_t = void*(__thiscall*)(void*, int);
        using acr_add_pad_t = unsigned char(__thiscall*)(void*, void*, int, const wchar_t*, short, short);

        inline constexpr std::size_t ACR_PADXENON_SIZE = 0x660;
        inline constexpr std::size_t ACR_SELECTED_PAD_OFFSET = 0x644;
        inline constexpr std::size_t ACR_PAD_SLOTS_OFFSET = 0x64c;
        inline constexpr std::size_t ACR_PAD_SLOT_SIZE = 0x21c;
        inline constexpr int ACR_PAD_SLOT_COUNT = 8;
        inline constexpr int ACR_PAD_MODE_XINPUT = 2;

        acr_pad_xenon_ctor_t acr_pad_xenon_ctor = nullptr;
        acr_add_pad_t acr_add_pad = nullptr;
        void* acr_enum_callback_addr = nullptr;
        std::uint8_t acr_original_callback_bytes[5]{};
        volatile LONG acr_injection_state = 0;
        bool acr_hook_installed = false;

        std::size_t acr_mask_length(const char* mask) {
            std::size_t length = 0;
            while (mask[length]) ++length;
            return length;
        }

        std::uint8_t* acr_find_pattern(HMODULE module, const std::uint8_t* pattern,
                                       const char* mask) {
            auto* base = reinterpret_cast<std::uint8_t*>(module);
            auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;

            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

            auto* section = IMAGE_FIRST_SECTION(nt);
            for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
                if (std::memcmp(section->Name, ".text", 5) != 0) continue;

                auto* begin = base + section->VirtualAddress;
                const std::size_t size = section->Misc.VirtualSize;
                const std::size_t length = acr_mask_length(mask);
                if (size < length) return nullptr;

                for (std::size_t offset = 0; offset <= size - length; ++offset) {
                    bool match = true;
                    for (std::size_t j = 0; j < length; ++j) {
                        if (mask[j] == 'x' && begin[offset + j] != pattern[j]) {
                            match = false;
                            break;
                        }
                    }
                    if (match) return begin + offset;
                }
                return nullptr;
            }
            return nullptr;
        }

        int acr_first_free_pad_slot(void* pad_proxy) {
            auto* proxy = static_cast<std::uint8_t*>(pad_proxy);
            for (int i = 0; i < ACR_PAD_SLOT_COUNT; ++i) {
                auto** pad = reinterpret_cast<void**>(
                    proxy + ACR_PAD_SLOTS_OFFSET + static_cast<std::size_t>(i) * ACR_PAD_SLOT_SIZE);
                if (!*pad) return i;
            }
            return -1;
        }

        bool acr_inject_xinput_pad(void* context_ptr) {
            if (!context_ptr || !acr_pad_xenon_ctor || !acr_add_pad) {
                logger::error("XInput injection called without resolved game functions");
                return false;
            }

            auto** context = static_cast<void**>(context_ptr);
            void* pad_proxy = context[1];
            if (!pad_proxy) {
                logger::error("DirectInput callback did not provide PadProxyPC");
                return false;
            }

            DWORD xinput_index = 0;
            bool connected = false;
            for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
                XINPUT_CAPABILITIES capabilities{};
                if (XInputGetCapabilities(i, XINPUT_FLAG_GAMEPAD, &capabilities) == ERROR_SUCCESS) {
                    xinput_index = i;
                    connected = true;
                    break;
                }
            }

            logger::info(connected
                ? "Connected XInput controller found"
                : "No controller connected yet; registering XInput port 0 for hotplug");

            const int slot = acr_first_free_pad_slot(pad_proxy);
            if (slot < 0) {
                logger::error("PadProxyPC has no free controller slots");
                return false;
            }

            void* memory = VirtualAlloc(nullptr, ACR_PADXENON_SIZE,
                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!memory) {
                logger::error("Could not allocate PadXenon");
                return false;
            }

            std::memset(memory, 0, ACR_PADXENON_SIZE);
            void* pad = acr_pad_xenon_ctor(memory, static_cast<int>(xinput_index));
            if (!pad) {
                logger::error("PadXenon constructor failed");
                VirtualFree(memory, 0, MEM_RELEASE);
                return false;
            }

            const wchar_t* const names[XUSER_MAX_COUNT] = {
                L"XInput Controller 1", L"XInput Controller 2",
                L"XInput Controller 3", L"XInput Controller 4"
            };
            if (!acr_add_pad(pad_proxy, pad, ACR_PAD_MODE_XINPUT,
                names[xinput_index], 0, 0)) {
                logger::error("PadProxyPC::AddPad rejected PadXenon");
                VirtualFree(memory, 0, MEM_RELEASE);
                return false;
            }

            auto* proxy = static_cast<std::uint8_t*>(pad_proxy);
            *reinterpret_cast<DWORD*>(proxy + ACR_SELECTED_PAD_OFFSET) =
                static_cast<DWORD>(slot);

            auto* context_dwords = static_cast<DWORD*>(context_ptr);
            ++context_dwords[2];

            logger::info(std::format("PadXenon registered and selected in slot {}, XInput port {}",
                slot, xinput_index));
            return true;
        }

        BOOL CALLBACK acr_hooked_enum_callback(const DIDEVICEINSTANCE*, void* context) {
            if (InterlockedCompareExchange(&acr_injection_state, 1, 0) == 0) {
                if (acr_inject_xinput_pad(context))
                    InterlockedExchange(&acr_injection_state, 2);
                else
                    InterlockedExchange(&acr_injection_state, 0);
            }

            // Do not let the game register the same physical controller through
            // DirectInput. PadXenon is now the only controller implementation.
            return DIENUM_CONTINUE;
        }

        bool acr_install_callback_hook(void* target, void* replacement) {
            DWORD old_protect{};
            if (!VirtualProtect(target, sizeof(acr_original_callback_bytes),
                PAGE_EXECUTE_READWRITE, &old_protect))
                return false;

            std::memcpy(acr_original_callback_bytes, target,
                sizeof(acr_original_callback_bytes));
            auto* bytes = static_cast<std::uint8_t*>(target);
            bytes[0] = 0xe9;
            const auto relative = static_cast<std::int32_t>(
                reinterpret_cast<std::uintptr_t>(replacement) -
                reinterpret_cast<std::uintptr_t>(target) - 5);
            std::memcpy(bytes + 1, &relative, sizeof(relative));

            VirtualProtect(target, sizeof(acr_original_callback_bytes),
                old_protect, &old_protect);
            FlushInstructionCache(GetCurrentProcess(), target,
                sizeof(acr_original_callback_bytes));
            return true;
        }

        void acr_remove_callback_hook() {
            if (!acr_hook_installed || !acr_enum_callback_addr) return;

            DWORD old_protect{};
            if (VirtualProtect(acr_enum_callback_addr, sizeof(acr_original_callback_bytes),
                PAGE_EXECUTE_READWRITE, &old_protect)) {
                std::memcpy(acr_enum_callback_addr, acr_original_callback_bytes,
                    sizeof(acr_original_callback_bytes));
                VirtualProtect(acr_enum_callback_addr, sizeof(acr_original_callback_bytes),
                    old_protect, &old_protect);
                FlushInstructionCache(GetCurrentProcess(), acr_enum_callback_addr,
                    sizeof(acr_original_callback_bytes));
            }
            acr_hook_installed = false;
        }

        bool apply_xinput_fix_acrmp() {
    #pragma warning(push)
    #pragma warning(disable: 4995)
            XInputEnable(TRUE);
    #pragma warning(pop)

            HMODULE game = GetModuleHandleW(nullptr);
            if (!game) {
                logger::error("Could not get ACRMP.exe module");
                return false;
            }

            acr_pad_xenon_ctor = reinterpret_cast<acr_pad_xenon_ctor_t>(
                acr_find_pattern(game, acr::ctor_pattern, acr::ctor_mask));
            acr_add_pad = reinterpret_cast<acr_add_pad_t>(
                acr_find_pattern(game, acr::add_pad_pattern, acr::add_pad_mask));
            acr_enum_callback_addr = acr_find_pattern(
                game, acr::enum_callback_pattern, acr::enum_callback_mask);

            if (!acr_pad_xenon_ctor || !acr_add_pad || !acr_enum_callback_addr) {
                logger::error("Unsupported ACRMP.exe: controller signatures not found");
                return false;
            }

            logger::info("ACRMP PadXenon, AddPad and DirectInput callback resolved");
            if (!acr_install_callback_hook(
                    acr_enum_callback_addr, reinterpret_cast<void*>(acr_hooked_enum_callback))) {
                logger::error("Could not install DirectInput enumeration hook");
                return false;
            }

            acr_hook_installed = true;
            logger::info("Native XInput controller injection installed");
            return true;
        }

        bool apply_xinput_detection_fix()
        {
            switch (config::get_game())
            {
            case config::game::acb:
                return apply_xinput_fix_acbmp();
            case config::game::acr:
                return apply_xinput_fix_acrmp();
            default:
                logger::warn("xinput detection fix not available for this game");
                return false;
            }
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

        constexpr std::uint8_t PB_OPCODE = 0xA1;
        constexpr std::array<std::uint8_t, 5> PB_PATCH_BYTES{ 0x31, 0xC0, 0xC3, 0x90, 0x90 };
        constexpr std::array<std::ptrdiff_t, 2> PB_RVA_VARIANTS{ 0, -5 };

        // per-game punkbuster patch definitions (RVAs are binary-specific)
        constexpr std::array<pb_patch, 4> PB_PATCHES_ACB{{
            { 0x013A5CD0, "isPbSvEnabled" },
            { 0x013A73C0, "isPbClEnabled" },
            { 0x013A5D30, "EnablePbSv" },
            { 0x013A7480, "EnablePbCl" },
        }};

        struct punkbuster_config
        {
            std::uintptr_t state_data_rva;
            std::span<const pb_patch> patches;
        };

        constexpr punkbuster_config ACBMP_PB_CONFIG{
            .state_data_rva = 0x02264520,
            .patches = PB_PATCHES_ACB,
        };

        [[nodiscard]] punkbuster_config get_punkbuster_config()
        {
            if (config::get_game() == config::game::acb)
                return ACBMP_PB_CONFIG;

            // no known punkbuster patch offsets for ACRMP/AC3MP
            return { .state_data_rva = 0, .patches = {} };
        }

        bool patch_punkbuster_function(std::uintptr_t base, std::uintptr_t state_data_rva, const pb_patch& p)
        {
            std::array<std::uint8_t, 5> expected{};
            expected[0] = PB_OPCODE;
            auto data_va = static_cast<std::uint32_t>(base + state_data_rva);
            std::memcpy(expected.data() + 1, &data_va, sizeof(data_va));

            std::array<std::uint8_t, 5> first_seen{};

            for (std::size_t i = 0; i < PB_RVA_VARIANTS.size(); ++i)
            {
                auto* addr = reinterpret_cast<std::uint8_t*>(base + p.rva + PB_RVA_VARIANTS[i]);

                std::array<std::uint8_t, 5> actual{};
                std::memcpy(actual.data(), addr, actual.size());

                if (i == 0)
                    first_seen = actual;

                if (actual != expected)
                    continue;

                DWORD old_protect;
                if (!VirtualProtect(addr, actual.size(), PAGE_EXECUTE_READWRITE, &old_protect))
                    return false;

                std::memcpy(addr, PB_PATCH_BYTES.data(), PB_PATCH_BYTES.size());
                VirtualProtect(addr, actual.size(), old_protect, &old_protect);
                FlushInstructionCache(GetCurrentProcess(), addr, actual.size());

                logger::info(std::format("punkbuster fix: patched {} at 0x{:X}", p.name, reinterpret_cast<std::uintptr_t>(addr)));
                return true;
            }

            logger::warn(std::format("punkbuster fix: byte signature mismatch for {}, expected {:02X} {:02X} {:02X} {:02X} {:02X}, found {:02X} {:02X} {:02X} {:02X} {:02X}",
                p.name,
                expected[0], expected[1], expected[2], expected[3], expected[4],
                first_seen[0], first_seen[1], first_seen[2], first_seen[3], first_seen[4]));
            return false;
        }

        bool apply_punkbuster_fix()
        {
            const auto config = get_punkbuster_config();
            if (config.patches.empty())
            {
                logger::warn("punkbuster fix: no patch offsets available for this game");
                return false;
            }

            auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            if (!base)
                return false;

            bool any = false;
            for (const auto& p : config.patches)
                any |= patch_punkbuster_function(base, config.state_data_rva, p);

            return any;
        }

        // skip intro videos fix (AC3MP)
        //
        // located by signature rather than fixed addresses, so it applies to any
        // AC3MP.exe build that still contains the same startup video code.

        namespace ac3_intro
        {
            // tail of StartupVideoManager::Start: loop over the startup entries,
            // then `mov ecx, esi; call StartNextVideo`
            constexpr signature startup_video_call{
                "\x0F\xB7\x46\x36\xFF\x45\xFC\x25\xFF\x3F\x00\x00\x83\xC3\x2C\x39\x45\xFC\x72\x00\x8B\xCE\xE8\x00\x00\x00\x00\x5E\x5B\xC9\xC3",
                "xxxxxxxxxxxxxxxxxxx?xxx????xxxx"
            };
            constexpr std::size_t STARTUP_VIDEO_CALL_OFFSET = 22;

            // StartNextVideo reads the entry count (+0x36) and the current
            // index (+0x1C) that finish_startup_videos writes
            constexpr signature start_next_video_body{
                "\x0F\xB7\x4E\x36\x8B\x46\x1C",
                "xxxxxxx"
            };
            constexpr std::size_t START_NEXT_VIDEO_BODY_OFFSET = 8;

            constexpr signature finish_video_sequence{
                "\x56\x8B\xF1\x8B\x46\x38\x85\xC0\x74\x02\xFF\xD0\xC6\x46\x21\x01\xE8\x00\x00\x00\x00\x5E\x85\xC0\x74\x07\x8B\xC8\xE9",
                "xxxxxxxxxxxxxxxxx????xxxxxxxx"
            };

            constexpr signature advance_startup_state{
                "\x80\x79\x21\x00\x75\x00\x83\x79\x24\x00\x75\x00\x8B\x49\x28\x8B\x41\x18\x83\xF8\x02\x74\x00\x83\xF8\x03\x75\x00\x6A\x01\xE8",
                "xxxxx?xxxxx?xxxxxxxxxx?xxxx?xxx"
            };

            using game_method_t = void(__thiscall*)(void*);

            game_method_t s_finish_sequence = nullptr;
            game_method_t s_advance_state = nullptr;

            // called in place of StartNextVideo; finishes the startup video
            // sequence immediately so the disclaimer/ubi logo never play.
            void __fastcall finish_startup_videos(void* manager, void* /*unused*/)
            {
                if (!manager || !s_finish_sequence || !s_advance_state)
                    return;

                // mark every startup entry as consumed so no stale video is
                // queued later (an empty list never fires "video finished").
                auto* bytes = static_cast<std::uint8_t*>(manager);
                const auto video_count = *reinterpret_cast<const WORD*>(bytes + 0x36) & 0x3FFF;
                *reinterpret_cast<DWORD*>(bytes + 0x1C) = video_count;

                s_finish_sequence(manager);
                s_advance_state(manager);

                logger::info("skip intro videos fix: startup sequence completed");
            }

            // a signature that matches more than once cannot be trusted on an
            // unknown build, so require exactly one match in the image
            [[nodiscard]] std::uint8_t* find_unique(HMODULE module, const signature& sig, std::string_view name)
            {
                MODULEINFO mod_info{};
                if (!GetModuleInformation(GetCurrentProcess(), module, &mod_info, sizeof(mod_info)))
                    return nullptr;

                const std::span<const std::uint8_t> memory{
                    static_cast<const std::uint8_t*>(mod_info.lpBaseOfDll), mod_info.SizeOfImage};

                const auto first = scan_signature(memory, sig);
                if (!first)
                {
                    logger::warn(std::format("skip intro videos fix: {} not found", name));
                    return nullptr;
                }

                const auto offset = *first - reinterpret_cast<std::uintptr_t>(memory.data()) + 1;
                if (scan_signature(memory.subspan(offset), sig))
                {
                    logger::warn(std::format("skip intro videos fix: {} is ambiguous", name));
                    return nullptr;
                }

                return reinterpret_cast<std::uint8_t*>(*first);
            }
        }

        bool apply_skip_intro_videos_fix_ac3mp()
        {
            auto* executable = GetModuleHandleW(nullptr);
            if (!executable)
            {
                logger::error("failed to get game module handle");
                return false;
            }

            auto* site = ac3_intro::find_unique(executable, ac3_intro::startup_video_call, "startup video call");
            auto* finish = ac3_intro::find_unique(executable, ac3_intro::finish_video_sequence, "FinishVideoSequence");
            auto* advance = ac3_intro::find_unique(executable, ac3_intro::advance_startup_state, "AdvanceStartupState");
            if (!site || !finish || !advance)
                return false;

            auto* call = site + ac3_intro::STARTUP_VIDEO_CALL_OFFSET;

            // make sure the call still goes to StartNextVideo before replacing it
            std::int32_t original_relative;
            std::memcpy(&original_relative, call + 1, sizeof(original_relative));
            const auto* start_next_video = call + 5 + original_relative;

            const auto& body = ac3_intro::start_next_video_body;
            if (std::memcmp(start_next_video + ac3_intro::START_NEXT_VIDEO_BODY_OFFSET,
                            body.pattern.data(), body.length()) != 0)
            {
                logger::warn("skip intro videos fix: startup video call does not target StartNextVideo");
                return false;
            }

            ac3_intro::s_finish_sequence = reinterpret_cast<ac3_intro::game_method_t>(finish);
            ac3_intro::s_advance_state = reinterpret_cast<ac3_intro::game_method_t>(advance);

            std::array<std::uint8_t, 5> replacement{ 0xE8, 0, 0, 0, 0 };
            const auto relative = static_cast<std::int32_t>(
                reinterpret_cast<std::uint8_t*>(ac3_intro::finish_startup_videos) - (call + 5));
            std::memcpy(replacement.data() + 1, &relative, sizeof(relative));

            DWORD old_protect;
            if (!VirtualProtect(call, replacement.size(), PAGE_EXECUTE_READWRITE, &old_protect))
            {
                logger::error("failed to unprotect memory for skip intro videos patch");
                return false;
            }

            std::memcpy(call, replacement.data(), replacement.size());
            FlushInstructionCache(GetCurrentProcess(), call, replacement.size());
            VirtualProtect(call, replacement.size(), old_protect, &old_protect);

            const auto base = reinterpret_cast<std::uintptr_t>(executable);
            logger::info(std::format("skip intro videos fix installed (call 0x{:X}, finish 0x{:X}, advance 0x{:X})",
                reinterpret_cast<std::uintptr_t>(call) - base,
                reinterpret_cast<std::uintptr_t>(finish) - base,
                reinterpret_cast<std::uintptr_t>(advance) - base));
            return true;
        }

        // uplay proxy friend-service host fix (AC3MP)
        //
        // uplay_r1_loader.dll is the multiplayer proxy that serves the AC3 friend
        // list from the official online config service. It talks HTTP through
        // WinINet, which the hostname redirect does not cover, so the proxy's
        // base URLs are replaced here. Every reference to a URL literal is an
        // absolute address with a base relocation; those references are pointed
        // at a copy of the new URL owned by the injector, so the host can be any
        // length. Only the mapped image is touched, never the file itself.

        namespace uplay_proxy
        {
            inline constexpr std::string_view MODULE_NAME = "uplay_r1_loader.dll";
            inline constexpr std::wstring_view MODULE_NAME_WIDE = L"uplay_r1_loader.dll";
            inline constexpr std::string_view URL_SCHEME = "http://";

            // base URL literal used by the proxy (friend-proxy-20260614, .rdata)
            inline constexpr std::string_view ORIGINAL_URL = "http://onlineconfigservice.ubi.com";

            // the proxy retries a private address when the primary URL fails,
            // which is unreachable for players; retry the redirect host instead
            inline constexpr std::string_view RETRY_URL = "http://10.163.216.209";

            // the proxy is imported by AC3MP.exe but can be mapped after the
            // injector, so also wait for it on a worker thread
            inline constexpr int WAIT_ATTEMPTS = 120;
            inline constexpr DWORD WAIT_INTERVAL_MS = 250;

            std::atomic_bool s_patch_started{false};
            std::atomic_bool s_shutting_down{false};
            std::string s_replacement_url;

            // never freed: the proxy may still read it after the injector unloads
            const char* s_url_copy{nullptr};
        }

        enum class proxy_patch_result
        {
            applied,
            module_not_loaded,
            url_not_found,
            patch_failed
        };

        [[nodiscard]] IMAGE_NT_HEADERS* get_nt_headers(HMODULE module)
        {
            auto* base = reinterpret_cast<std::uint8_t*>(module);
            auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE)
                return nullptr;

            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE)
                return nullptr;

            return nt;
        }

        // finds a NUL-terminated string literal in the readable sections
        [[nodiscard]] std::uint8_t* find_proxy_string(HMODULE module, std::string_view text)
        {
            auto* nt = get_nt_headers(module);
            if (!nt)
                return nullptr;

            auto* base = reinterpret_cast<std::uint8_t*>(module);
            const std::size_t length = text.size() + 1;

            auto* section = IMAGE_FIRST_SECTION(nt);
            for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
            {
                if ((section->Characteristics & IMAGE_SCN_MEM_READ) == 0)
                    continue;

                auto* begin = base + section->VirtualAddress;
                const std::size_t size = section->Misc.VirtualSize;
                if (size < length)
                    continue;

                for (std::size_t offset = 0; offset <= size - length; ++offset)
                {
                    if (std::memcmp(begin + offset, text.data(), text.size()) == 0 && begin[offset + text.size()] == 0)
                        return begin + offset;
                }
            }

            return nullptr;
        }

        // rewrites every relocated absolute reference to `target` so it points
        // at `replacement`; returns the number of references changed
        [[nodiscard]] std::size_t repoint_references(HMODULE module, const void* target, const void* replacement)
        {
            auto* nt = get_nt_headers(module);
            if (!nt)
                return 0;

            const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
            if (directory.VirtualAddress == 0 || directory.Size == 0)
                return 0;

            auto* base = reinterpret_cast<std::uint8_t*>(module);
            const auto old_value = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(target));
            const auto new_value = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(replacement));

            std::size_t patched = 0;
            auto* block = base + directory.VirtualAddress;
            auto* const end = block + directory.Size;

            while (block + sizeof(IMAGE_BASE_RELOCATION) <= end)
            {
                const auto* header = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(block);
                if (header->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION))
                    break;

                const auto* entries = reinterpret_cast<const WORD*>(block + sizeof(IMAGE_BASE_RELOCATION));
                const std::size_t count = (header->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);

                for (std::size_t i = 0; i < count; ++i)
                {
                    if ((entries[i] >> 12) != IMAGE_REL_BASED_HIGHLOW)
                        continue;

                    auto* slot = base + header->VirtualAddress + (entries[i] & 0x0FFF);

                    std::uint32_t value;
                    std::memcpy(&value, slot, sizeof(value));
                    if (value != old_value)
                        continue;

                    DWORD old_protect;
                    if (!VirtualProtect(slot, sizeof(value), PAGE_EXECUTE_READWRITE, &old_protect))
                    {
                        logger::error(std::format("uplay proxy host fix: failed to unprotect reference at 0x{:X}",
                            reinterpret_cast<std::uintptr_t>(slot)));
                        continue;
                    }

                    std::memcpy(slot, &new_value, sizeof(new_value));
                    VirtualProtect(slot, sizeof(value), old_protect, &old_protect);
                    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(value));
                    ++patched;
                }

                block += header->SizeOfBlock;
            }

            return patched;
        }

        [[nodiscard]] const char* get_url_copy(std::string_view url)
        {
            if (uplay_proxy::s_url_copy)
                return uplay_proxy::s_url_copy;

            auto* copy = static_cast<char*>(VirtualAlloc(nullptr, url.size() + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            if (!copy)
                return nullptr;

            std::memcpy(copy, url.data(), url.size());
            copy[url.size()] = 0;

            DWORD old_protect;
            VirtualProtect(copy, url.size() + 1, PAGE_READONLY, &old_protect);

            uplay_proxy::s_url_copy = copy;
            return copy;
        }

        [[nodiscard]] proxy_patch_result patch_proxy_url(std::string_view replacement_url)
        {
            const auto module = GetModuleHandleW(uplay_proxy::MODULE_NAME_WIDE.data());
            if (!module)
                return proxy_patch_result::module_not_loaded;

            auto* url = find_proxy_string(module, uplay_proxy::ORIGINAL_URL);
            if (!url)
                return proxy_patch_result::url_not_found;

            const char* copy = get_url_copy(replacement_url);
            if (!copy)
            {
                logger::error(std::format("uplay proxy host fix: failed to allocate the replacement URL (error {})",
                    GetLastError()));
                return proxy_patch_result::patch_failed;
            }

            const auto references = repoint_references(module, url, copy);
            if (references == 0)
            {
                logger::error(std::format("uplay proxy host fix: no references to {} found in {}",
                    uplay_proxy::ORIGINAL_URL, uplay_proxy::MODULE_NAME));
                return proxy_patch_result::patch_failed;
            }

            logger::info(std::format("uplay proxy host fix: {} -> {} ({} references)",
                uplay_proxy::ORIGINAL_URL, replacement_url, references));

            if (auto* retry_url = find_proxy_string(module, uplay_proxy::RETRY_URL))
            {
                const auto retry_references = repoint_references(module, retry_url, copy);
                logger::info(std::format("uplay proxy host fix: retry {} -> {} ({} references)",
                    uplay_proxy::RETRY_URL, replacement_url, retry_references));
            }

            return proxy_patch_result::applied;
        }

        auto log_proxy_not_redirected(std::string_view reason) -> void
        {
            logger::error(std::format("uplay proxy host fix: {}; the AC3 friend list will not be redirected", reason));
        }

        DWORD WINAPI uplay_proxy_patch_thread([[maybe_unused]] LPVOID lpParam)
        {
            for (int attempt = 0; attempt < uplay_proxy::WAIT_ATTEMPTS; ++attempt)
            {
                Sleep(uplay_proxy::WAIT_INTERVAL_MS);

                if (uplay_proxy::s_shutting_down)
                    return 0;

                switch (patch_proxy_url(uplay_proxy::s_replacement_url))
                {
                case proxy_patch_result::applied:
                    return 0;
                case proxy_patch_result::url_not_found:
                    log_proxy_not_redirected(std::format("URL not found in {}", uplay_proxy::MODULE_NAME));
                    return 0;
                case proxy_patch_result::patch_failed:
                    log_proxy_not_redirected("patching failed");
                    return 0;
                case proxy_patch_result::module_not_loaded:
                    break;
                }
            }

            log_proxy_not_redirected(std::format("{} was not loaded in time", uplay_proxy::MODULE_NAME));
            return 0;
        }

        [[nodiscard]] bool apply_uplay_proxy_host_fix()
        {
            const auto& redirect_host = config::get().net.redirect_host;

            if (redirect_host.empty())
            {
                log_proxy_not_redirected("no redirect host configured");
                return false;
            }

            if (_stricmp(redirect_host.c_str(), ORIGINAL_HOST.data()) == 0)
            {
                logger::info("uplay proxy host fix: redirect host is the official host, nothing to do");
                return true;
            }

            uplay_proxy::s_replacement_url = std::format("{}{}", uplay_proxy::URL_SCHEME, redirect_host);

            switch (patch_proxy_url(uplay_proxy::s_replacement_url))
            {
            case proxy_patch_result::applied:
                return true;
            case proxy_patch_result::url_not_found:
                log_proxy_not_redirected(std::format("URL not found in {}", uplay_proxy::MODULE_NAME));
                return false;
            case proxy_patch_result::patch_failed:
                log_proxy_not_redirected("patching failed");
                return false;
            case proxy_patch_result::module_not_loaded:
                break;
            }

            logger::info(std::format("uplay proxy host fix: waiting for {} to be loaded",
                uplay_proxy::MODULE_NAME));

            if (!uplay_proxy::s_patch_started.exchange(true))
            {
                if (HANDLE thread = CreateThread(nullptr, 0, uplay_proxy_patch_thread, nullptr, 0, nullptr))
                    CloseHandle(thread);
                else
                    log_proxy_not_redirected("failed to start the wait thread");
            }

            return true;
        }

        [[nodiscard]] bool is_skip_intro_videos_fix_supported()
        {
            // startup video patch offsets are only known for AC3MP
            return config::get_game() == config::game::ac3;
        }

        [[nodiscard]] bool is_xinput_fix_supported()
        {
            // XInput fix is not needed for AC3MP
            const auto game = config::get_game();
            return game == config::game::acb || game == config::game::acr;
        }

        [[nodiscard]] bool is_punkbuster_fix_supported()
        {
            // PunkBuster patch offsets are only known for ACBMP
            return config::get_game() == config::game::acb;
        }

        [[nodiscard]] bool is_uplay_proxy_host_fix_supported()
        {
            // only the AC3MP uplay proxy serves the friend list through the
            // official online config service host
            return config::get_game() == config::game::ac3;
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

        // game-specific fixes stay silent in games they do not apply to

        if (is_xinput_fix_supported())
        {
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
        }

        if (is_punkbuster_fix_supported())
        {
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
        }

        if (is_skip_intro_videos_fix_supported())
        {
            if (settings.fix.fix_skip_intro_videos)
            {
                logger::info("applying skip intro videos fix");
                if (!apply_skip_intro_videos_fix_ac3mp())
                    logger::warn("skip intro videos fix could not be applied");
            }
            else
            {
                logger::info("skip intro videos fix disabled");
            }
        }

        if (is_uplay_proxy_host_fix_supported())
        {
            logger::info("applying uplay proxy host fix");
            (void)apply_uplay_proxy_host_fix();
        }

        s_active = true;
        return true;
    }

    auto shutdown() -> void
    {
        uplay_proxy::s_shutting_down = true;
        remove_jmp_hook();
        acr_remove_callback_hook();
        s_active = false;
    }

    [[nodiscard]] auto is_active() -> bool
    {
        return s_active;
    }
}
