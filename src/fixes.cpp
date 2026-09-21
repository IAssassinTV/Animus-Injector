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

        namespace ac3_intro
        {
            // AC3MP.exe build reference (2013-04-30, PE timestamp 0x517F6F88)
            constexpr std::uint32_t AC3MP_PE_TIMESTAMP = 0x517F6F88u;
            constexpr std::uint32_t AC3MP_IMAGE_SIZE = 0x015E1000u;
            constexpr std::uintptr_t STARTUP_VIDEO_CALL_RVA = 0x007CDAAEu;
            constexpr std::uintptr_t FINISH_VIDEO_SEQUENCE_RVA = 0x007C6A96u;
            constexpr std::uintptr_t ADVANCE_STARTUP_STATE_RVA = 0x007D46A0u;

            // StartupVideoManager::Start begins with `call StartNextVideo`
            constexpr std::array<std::uint8_t, 5> STARTUP_VIDEO_CALL_BYTES{
                0xE8, 0x9A, 0xF2, 0xFF, 0xFF
            };

            using game_method_t = void(__thiscall*)(void*);

            std::uint8_t* s_game_image = nullptr;

            // called in place of StartNextVideo; finishes the startup video
            // sequence immediately so the disclaimer/ubi logo never play.
            void __fastcall finish_startup_videos(void* manager, void* /*unused*/)
            {
                if (!manager || !s_game_image)
                    return;

                // mark every startup entry as consumed so no stale video is
                // queued later (an empty list never fires "video finished").
                auto* bytes = static_cast<std::uint8_t*>(manager);
                const auto video_count = *reinterpret_cast<const WORD*>(bytes + 0x36) & 0x3FFF;
                *reinterpret_cast<DWORD*>(bytes + 0x1C) = video_count;

                const auto finish_sequence = reinterpret_cast<game_method_t>(
                    s_game_image + FINISH_VIDEO_SEQUENCE_RVA);
                const auto advance_state = reinterpret_cast<game_method_t>(
                    s_game_image + ADVANCE_STARTUP_STATE_RVA);

                finish_sequence(manager);
                advance_state(manager);

                logger::info("skip intro videos fix: startup sequence completed");
            }
        }

        bool apply_skip_intro_videos_fix_ac3mp()
        {
            logger::info("applying skip intro videos fix");

            auto* executable = GetModuleHandleW(nullptr);
            if (!executable)
            {
                logger::error("failed to get game module handle");
                return false;
            }

            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(executable);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            {
                logger::error("skip intro videos fix: executable not recognized (invalid DOS header)");
                return false;
            }

            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
                reinterpret_cast<const std::uint8_t*>(executable) + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE ||
                nt->FileHeader.TimeDateStamp != ac3_intro::AC3MP_PE_TIMESTAMP ||
                nt->OptionalHeader.SizeOfImage != ac3_intro::AC3MP_IMAGE_SIZE)
            {
                logger::warn("skip intro videos fix: unsupported AC3MP.exe build");
                return false;
            }

            auto* call = reinterpret_cast<std::uint8_t*>(executable)
                + ac3_intro::STARTUP_VIDEO_CALL_RVA;
            if (std::memcmp(call, ac3_intro::STARTUP_VIDEO_CALL_BYTES.data(),
                            ac3_intro::STARTUP_VIDEO_CALL_BYTES.size()) != 0)
            {
                logger::warn("skip intro videos fix: startup video call signature mismatch");
                return false;
            }

            ac3_intro::s_game_image = reinterpret_cast<std::uint8_t*>(executable);

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

            logger::info("skip intro videos fix installed");
            return true;
        }

        // uplay proxy friend-service host fix (AC3MP)
        //
        // uplay_r1_loader.dll is the multiplayer proxy that serves the AC3 friend
        // list from the official online config service. Rewrite the base URL the proxy builds every
        // request from. Only the mapped image is touched, never the file itself.

        namespace uplay_proxy
        {
            inline constexpr std::string_view MODULE_NAME = "uplay_r1_loader.dll";
            inline constexpr std::wstring_view MODULE_NAME_WIDE = L"uplay_r1_loader.dll";
            inline constexpr std::string_view URL_SCHEME = "http://";

            // base URL literal used by the proxy (friend-proxy-20260614, .rdata)
            inline constexpr std::string_view ORIGINAL_URL = "http://onlineconfigservice.ubi.com";

            // the proxy is imported by AC3MP.exe but can be mapped after the
            // injector, so also wait for it on a worker thread
            inline constexpr int WAIT_ATTEMPTS = 120;
            inline constexpr DWORD WAIT_INTERVAL_MS = 250;

            std::atomic_bool s_patch_started{false};
            std::atomic_bool s_shutting_down{false};
            std::string s_replacement_url;
        }

        enum class proxy_patch_result
        {
            applied,
            module_not_loaded,
            url_not_found
        };

        [[nodiscard]] std::uint8_t* find_proxy_url(HMODULE module)
        {
            auto* base = reinterpret_cast<std::uint8_t*>(module);
            auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE)
                return nullptr;

            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE)
                return nullptr;

            const std::size_t length = uplay_proxy::ORIGINAL_URL.size();

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
                    if (std::memcmp(begin + offset, uplay_proxy::ORIGINAL_URL.data(), length) == 0)
                        return begin + offset;
                }
            }

            return nullptr;
        }

        [[nodiscard]] proxy_patch_result patch_proxy_url(std::string_view replacement_url)
        {
            const auto module = GetModuleHandleW(uplay_proxy::MODULE_NAME_WIDE.data());
            if (!module)
                return proxy_patch_result::module_not_loaded;

            auto* url = find_proxy_url(module);
            if (!url)
                return proxy_patch_result::url_not_found;

            const std::size_t length = uplay_proxy::ORIGINAL_URL.size() + 1;

            DWORD old_protect;
            if (!VirtualProtect(url, length, PAGE_EXECUTE_READWRITE, &old_protect))
            {
                logger::error(std::format("uplay proxy host fix: failed to unprotect the proxy URL at 0x{:X}",
                    reinterpret_cast<std::uintptr_t>(url)));
                return proxy_patch_result::url_not_found;
            }

            std::memcpy(url, replacement_url.data(), replacement_url.size());
            url[replacement_url.size()] = 0;

            VirtualProtect(url, length, old_protect, &old_protect);

            logger::info(std::format("uplay proxy host fix: {} -> {} at 0x{:X}",
                uplay_proxy::ORIGINAL_URL, replacement_url, reinterpret_cast<std::uintptr_t>(url)));

            return proxy_patch_result::applied;
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
                    logger::warn(std::format("uplay proxy host fix: URL not found in {}",
                        uplay_proxy::MODULE_NAME));
                    return 0;
                case proxy_patch_result::module_not_loaded:
                    break;
                }
            }

            logger::warn(std::format("uplay proxy host fix: {} was not loaded in time",
                uplay_proxy::MODULE_NAME));
            return 0;
        }

        [[nodiscard]] bool apply_uplay_proxy_host_fix()
        {
            const auto& redirect_host = config::get().net.redirect_host;

            if (redirect_host.empty())
            {
                logger::warn("uplay proxy host fix: no redirect host configured");
                return false;
            }

            if (_stricmp(redirect_host.c_str(), ORIGINAL_HOST.data()) == 0)
            {
                logger::info("uplay proxy host fix: redirect host is the official host, nothing to do");
                return true;
            }

            const std::string replacement = std::format("{}{}", uplay_proxy::URL_SCHEME, redirect_host);

            if (replacement.size() > uplay_proxy::ORIGINAL_URL.size())
            {
                logger::warn(std::format(
                    "uplay proxy host fix: '{}' is too long for the proxy URL ({} > {} bytes), "
                    "the proxy keeps using the hostname redirect",
                    replacement, replacement.size(), uplay_proxy::ORIGINAL_URL.size()));
                return false;
            }

            uplay_proxy::s_replacement_url = replacement;

            switch (patch_proxy_url(uplay_proxy::s_replacement_url))
            {
            case proxy_patch_result::applied:
                return true;
            case proxy_patch_result::url_not_found:
                logger::warn(std::format("uplay proxy host fix: URL not found in {}",
                    uplay_proxy::MODULE_NAME));
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
                    logger::warn("uplay proxy host fix: failed to start the wait thread");
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

        if (settings.fix.fix_xinput_detection)
        {
            if (!is_xinput_fix_supported())
            {
                logger::info("xinput detection fix not available for this game");
            }
            else
            {
                logger::info("applying xinput detection fix");
                if (!apply_xinput_detection_fix())
                    logger::warn("xinput detection fix could not be applied");
            }
        }
        else
        {
            logger::info("xinput detection fix disabled");
        }

        if (settings.fix.fix_disable_punkbuster)
        {
            if (!is_punkbuster_fix_supported())
            {
                logger::info("punkbuster fix not available for this game");
            }
            else
            {
                logger::info("applying punkbuster fix");
                if (!apply_punkbuster_fix())
                    logger::warn("punkbuster fix could not be applied");
            }
        }
        else
        {
            logger::info("punkbuster fix disabled");
        }

        if (settings.fix.fix_skip_intro_videos)
        {
            if (!is_skip_intro_videos_fix_supported())
            {
                logger::info("skip intro videos fix not available for this game");
            }
            else
            {
                logger::info("applying skip intro videos fix");
                if (!apply_skip_intro_videos_fix_ac3mp())
                    logger::warn("skip intro videos fix could not be applied");
            }
        }
        else
        {
            logger::info("skip intro videos fix disabled");
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
